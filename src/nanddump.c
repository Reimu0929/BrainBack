/*
 * nanddump.c - Brain NAND / ROM backup utility for SHARP Brain (Windows CE)
 *
 * Reads the raw contents of the NAND block devices documented on Brain Wiki
 * ("Hardware" page - device prefixes) and writes them to image files with a
 * small header + CRC32 so a backup can be verified later.
 *
 * Targets: Windows CE .NET 4.x / 5.0 / 6.0 (the OS used by Brain).
 * Style:   C89 / Win32 only, Unicode (WCHAR) APIs - eVC4 and CeGCC friendly.
 *
 * This is a backup tool for a device you own. Do not redistribute the
 * dictionary data or any copyrighted firmware contained in the images.
 *
 * References:
 *   https://brain.fandom.com/ja/wiki/Brain_Wiki
 *   https://brain.fandom.com/ja/wiki/%E3%83%8F%E3%83%BC%E3%83%89%E3%82%A6%E3%82%A7%E3%82%A2
 *   https://brain.fandom.com/ja/wiki/C/C%2B%2B%E3%81%A7Brain%E7%94%A8%E3%82%A2%E3%83%97%E3%83%AA%E3%82%92%E9%96%8B%E7%99%BA%E3%81%99%E3%82%8B
 */

#include <windows.h>
#include <commctrl.h>
#include <winioctl.h>        /* CTL_CODE, IOCTL_DISK_GET_DRIVE_GEOMETRY, DISK_GEOMETRY */
#include <wchar.h>
#include <stddef.h>

#include "resource.h"

/* Windows CE's diskio.h is not shipped with every CE toolchain (notably
 * CeGCC), so define the small piece we need locally with unique names.
 * Layout and control code match Windows CE 5.0/6.0 diskio.h. */
typedef struct _ND_DISK_INFO {
    DWORD di_total_sectors;
    DWORD di_bytes_per_sect;
    DWORD di_cylinders;
    DWORD di_heads;
    DWORD di_sectors;
    DWORD di_flags;
} ND_DISK_INFO;

#define ND_IOCTL_DISK_GETINFO \
    CTL_CODE(FILE_DEVICE_DISK, 0x700, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ------------------------------------------------------------------ */
/* Compiler-friendly 64-bit unsigned type (C89 has no "long long").    */
/* ------------------------------------------------------------------ */
#if defined(__GNUC__)
typedef unsigned long long DL_UINT64;
#else
typedef unsigned __int64 DL_UINT64;
#endif

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define WM_DUMP_PROGRESS    (WM_APP + 1)
#define WM_DUMP_DONE        (WM_APP + 2)

#define DEV_COUNT           5
#define DUMP_CHUNK_SECTORS  128          /* 64 KiB at 512 B/sector */
#define DUMP_MAX_BYTES      ((DL_UINT64)64 << 30)  /* safety cap if size unknown */
#define DUMP_MAGIC          "BRNDMP01"
#define DUMP_VERSION        1

/* ------------------------------------------------------------------ */
/* On-disk image header (little-endian, packed).                       */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)
typedef struct _DUMP_HEADER {
    char     magic[8];          /* "BRNDMP01" */
    DWORD    dwHeaderSize;      /* sizeof(DUMP_HEADER) */
    DWORD    dwVersion;         /* 1 */
    DWORD    dwFlags;           /* reserved (0) */
    WCHAR    wszDevice[16];     /* e.g. L"DSK8:" */
    DWORD    dwBytesPerSector;
    DWORD    dwTotalSectors;
    DWORD    dwDataSizeLow;     /* raw data size, low 32 bits */
    DWORD    dwDataSizeHigh;    /* raw data size, high 32 bits */
    DWORD    dwCrc32;           /* CRC-32 (IEEE) of the raw data */
    DWORD    dwReserved;
    FILETIME ftStartTime;
} DUMP_HEADER;
#pragma pack(pop)

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */
static const WCHAR *g_wszDevNames[DEV_COUNT] = {
    L"NDF1:", L"DSK6:", L"DSK7:", L"DSK8:", L"DSK9:"
};

static const int g_devIds[DEV_COUNT] = {
    IDC_DEV_NDF1, IDC_DEV_DSK6, IDC_DEV_DSK7, IDC_DEV_DSK8, IDC_DEV_DSK9
};

typedef struct _DUMP_PARAMS {
    HWND      hDlg;
    BOOL      bDev[DEV_COUNT];
    WCHAR     wszOutDir[MAX_PATH];
} DUMP_PARAMS;

typedef struct _GEOM {
    DWORD     dwSectorSize;
    DWORD     dwTotalSectors;
    DL_UINT64 ullTotalBytes;
    BOOL      bKnown;
} GEOM;

static volatile BOOL g_bCancel = FALSE;
static BOOL          g_bRunning = FALSE;
static HANDLE        g_hThread = NULL;
static WCHAR         g_szStatus[256];

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
static void SetStatus(LPCWSTR s)
{
    int i;
    for (i = 0; i < 255 && s[i]; i++)
        g_szStatus[i] = s[i];
    g_szStatus[i] = L'\0';
}

static void ShortName(LPCWSTR wszDev, WCHAR *wszShort, int cch)
{
    int i;
    for (i = 0; i < cch - 1 && wszDev[i] && wszDev[i] != L':'; i++)
        wszShort[i] = wszDev[i];
    wszShort[i] = L'\0';
}

/* 64-bit safe SetFilePointer (SetFilePointerEx is not on old CE). */
static BOOL SetPos64(HANDLE h, DL_UINT64 off)
{
    LONG high = (LONG)(off >> 32);
    DWORD r = SetFilePointer(h, (LONG)(off & 0xFFFFFFFFUL), &high, FILE_BEGIN);
    if (r == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR)
        return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* CRC-32 (IEEE 802.3, same as zlib/zip)                               */
/* ------------------------------------------------------------------ */
static DWORD g_crcTable[256];
static BOOL  g_crcReady = FALSE;

static void InitCrc(void)
{
    DWORD i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        g_crcTable[i] = c;
    }
    g_crcReady = TRUE;
}

static DWORD UpdateCrc(DWORD crc, const BYTE *buf, DWORD len)
{
    DWORD i;
    if (!g_crcReady)
        InitCrc();
    for (i = 0; i < len; i++)
        crc = g_crcTable[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

/* ------------------------------------------------------------------ */
/* Geometry discovery                                                  */
/* ------------------------------------------------------------------ */
static void GetGeometry(HANDLE hDev, GEOM *g)
{
    ND_DISK_INFO di;
    DISK_GEOMETRY dg;
    DWORD ret = 0;
    DL_UINT64 sectors;

    ZeroMemory(&di, sizeof(di));
    if (DeviceIoControl(hDev, ND_IOCTL_DISK_GETINFO, NULL, 0,
                        &di, sizeof(di), &ret, NULL) &&
        di.di_bytes_per_sect != 0 && di.di_total_sectors != 0) {
        g->dwSectorSize   = di.di_bytes_per_sect;
        g->dwTotalSectors = di.di_total_sectors;
        g->ullTotalBytes  = (DL_UINT64)di.di_total_sectors *
                            (DL_UINT64)di.di_bytes_per_sect;
        g->bKnown = TRUE;
        return;
    }

    /* Some drivers only answer the generic disk-geometry IOCTL. */
    ZeroMemory(&dg, sizeof(dg));
    if (DeviceIoControl(hDev, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                        &dg, sizeof(dg), &ret, NULL) &&
        dg.BytesPerSector != 0 && dg.SectorsPerTrack != 0) {
        sectors = (DL_UINT64)dg.Cylinders.QuadPart *
                  (DL_UINT64)dg.TracksPerCylinder *
                  (DL_UINT64)dg.SectorsPerTrack;
        if (sectors != 0) {
            g->dwSectorSize   = dg.BytesPerSector;
            g->dwTotalSectors = (DWORD)sectors;
            g->ullTotalBytes  = sectors * (DL_UINT64)dg.BytesPerSector;
            g->bKnown = TRUE;
            return;
        }
    }

    /* Some drivers do not implement either IOCTL but do allow GetFileSize
       on the block device. */
    {
        DWORD sz = GetFileSize(hDev, NULL);
        if (sz != INVALID_FILE_SIZE && sz != 0) {
            g->dwSectorSize   = 512;
            g->dwTotalSectors = sz / 512;
            g->ullTotalBytes  = (DL_UINT64)sz;
            g->bKnown = TRUE;
            return;
        }
    }

    /* Last resort: read sequentially until the device stops returning
       data. Progress will be indeterminate in this mode. */
    g->dwSectorSize   = 512;
    g->dwTotalSectors = 0;
    g->ullTotalBytes  = 0;
    g->bKnown = FALSE;
}

/* ------------------------------------------------------------------ */
/* Dump one block device into one image file                           */
/* ------------------------------------------------------------------ */
static void DumpOneDevice(HWND hDlg, LPCWSTR wszDev, LPCWSTR wszOutDir,
                          HANDLE hManifest)
{
    HANDLE      hDev = INVALID_HANDLE_VALUE;
    HANDLE      hFile = INVALID_HANDLE_VALUE;
    GEOM        geom;
    DUMP_HEADER hdr;
    BYTE       *buf = NULL;
    DWORD       bufSize, read, written, crc, i;
    DL_UINT64   cur = 0, total;
    WCHAR       path[MAX_PATH], shortName[16], line[256];
    DWORD       mb, mbTotal;

    hDev = CreateFileW(wszDev, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
    if (hDev == INVALID_HANDLE_VALUE) {
        wsprintfW(g_szStatus, L"%s: cannot open (error %u)",
                  wszDev, GetLastError());
        PostMessage(hDlg, WM_DUMP_PROGRESS, 0, 0);
        return;
    }

    GetGeometry(hDev, &geom);
    total = geom.ullTotalBytes;

    ShortName(wszDev, shortName, 16);
    wsprintfW(path, L"%s\\%s.bin", wszOutDir, shortName);
    hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        wsprintfW(g_szStatus, L"Cannot create %s (error %u)",
                  path, GetLastError());
        PostMessage(hDlg, WM_DUMP_PROGRESS, 0, 0);
        CloseHandle(hDev);
        return;
    }

    bufSize = DUMP_CHUNK_SECTORS * geom.dwSectorSize;
    buf = (BYTE *)LocalAlloc(LPTR, bufSize);
    if (buf == NULL) {
        SetStatus(L"Out of memory");
        PostMessage(hDlg, WM_DUMP_PROGRESS, 0, 0);
        CloseHandle(hFile);
        CloseHandle(hDev);
        return;
    }

    ZeroMemory(&hdr, sizeof(hdr));
    memcpy(hdr.magic, DUMP_MAGIC, 8);
    hdr.dwHeaderSize    = (DWORD)sizeof(DUMP_HEADER);
    hdr.dwVersion       = DUMP_VERSION;
    hdr.dwFlags         = 0;
    for (i = 0; i < 16; i++)
        hdr.wszDevice[i] = wszDev[i] ? wszDev[i] : 0;
    hdr.dwBytesPerSector = geom.dwSectorSize;
    hdr.dwTotalSectors   = geom.dwTotalSectors;
    hdr.dwDataSizeLow    = (DWORD)(total & 0xFFFFFFFFUL);
    hdr.dwDataSizeHigh   = (DWORD)(total >> 32);
    hdr.dwCrc32          = 0;
    GetSystemTimeAsFileTime(&hdr.ftStartTime);
    WriteFile(hFile, &hdr, sizeof(hdr), &written, NULL);

    wsprintfW(g_szStatus, L"%s: dumping...", wszDev);
    PostMessage(hDlg, WM_DUMP_PROGRESS, 0, 0);

    crc = 0xFFFFFFFFUL;
    while (!g_bCancel) {
        DWORD toRead = bufSize;

        if (geom.bKnown && total != 0) {
            if (cur >= total)
                break;
            if (total - cur < (DL_UINT64)toRead)
                toRead = (DWORD)(total - cur);
        }

        if (!SetPos64(hDev, cur)) {
            wsprintfW(g_szStatus, L"%s: seek error at %u MB",
                      wszDev, (DWORD)(cur >> 20));
            PostMessage(hDlg, WM_DUMP_PROGRESS, 0, 0);
            break;
        }

        if (!ReadFile(hDev, buf, toRead, &read, NULL) || read == 0) {
            if (geom.bKnown)
                wsprintfW(g_szStatus, L"%s: read stopped at %u MB (error %u)",
                          wszDev, (DWORD)(cur >> 20), GetLastError());
            else
                wsprintfW(g_szStatus, L"%s: end of device at %u MB",
                          wszDev, (DWORD)(cur >> 20));
            PostMessage(hDlg, WM_DUMP_PROGRESS, 0, 0);
            break;
        }

        if (!WriteFile(hFile, buf, read, &written, NULL) || written != read) {
            wsprintfW(g_szStatus, L"%s: write error (error %u)",
                      path, GetLastError());
            PostMessage(hDlg, WM_DUMP_PROGRESS, 0, 0);
            break;
        }

        crc = UpdateCrc(crc, buf, read);
        cur += read;

        mb      = (DWORD)(cur >> 20);
        mbTotal = (DWORD)(total >> 20);
        if (geom.bKnown && total != 0) {
            DWORD pct = (DWORD)((cur * 100) / total);
            if (pct > 100)
                pct = 100;
            wsprintfW(g_szStatus, L"%s: %u / %u MB (%u%%)",
                      wszDev, mb, mbTotal, pct);
            PostMessage(hDlg, WM_DUMP_PROGRESS, pct, 0);
        } else {
            wsprintfW(g_szStatus, L"%s: %u MB (size unknown)", wszDev, mb);
            PostMessage(hDlg, WM_DUMP_PROGRESS, 0, 0);
        }

        /* Guard against endless reads if the driver never reports an end. */
        if (!geom.bKnown && cur >= DUMP_MAX_BYTES) {
            SetStatus(L"Reached the 64 GB safety limit");
            PostMessage(hDlg, WM_DUMP_PROGRESS, 0, 0);
            break;
        }
    }

    crc ^= 0xFFFFFFFFUL;
    if (SetPos64(hFile, (DL_UINT64)offsetof(DUMP_HEADER, dwCrc32)))
        WriteFile(hFile, &crc, sizeof(crc), &written, NULL);

    /* The geometry may have been unknown when the header was first written.
       Always record the number of bytes we actually dumped, otherwise the
       PC-side verifier sees a zero-length payload and reports a false NG. */
    if (SetPos64(hFile, (DL_UINT64)offsetof(DUMP_HEADER, dwDataSizeLow))) {
        DWORD lo = (DWORD)(cur & 0xFFFFFFFFUL);
        DWORD hi = (DWORD)(cur >> 32);
        WriteFile(hFile, &lo, sizeof(lo), &written, NULL);
        WriteFile(hFile, &hi, sizeof(hi), &written, NULL);
    }

    wsprintfW(line, L"%s  size=%u MB  sectors=%u  sector=%u B  CRC=%08X  file=%s.bin\r\n",
              wszDev, (DWORD)(cur >> 20), geom.dwTotalSectors,
              geom.dwSectorSize, crc, shortName);
    if (hManifest != INVALID_HANDLE_VALUE)
        WriteFile(hManifest, line, lstrlenW(line) * sizeof(WCHAR),
                  &written, NULL);

    wsprintfW(g_szStatus, L"%s: done (%u MB, CRC=%08X)",
              wszDev, (DWORD)(cur >> 20), crc);
    PostMessage(hDlg, WM_DUMP_PROGRESS, 100, 0);

    LocalFree(buf);
    CloseHandle(hFile);
    CloseHandle(hDev);
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */
static DWORD WINAPI DumpThread(LPVOID lpParam)
{
    DUMP_PARAMS *p = (DUMP_PARAMS *)lpParam;
    HANDLE hManifest = INVALID_HANDLE_VALUE;
    WCHAR path[MAX_PATH];
    WORD bom = 0xFEFF;
    DWORD written;
    int i;

    wsprintfW(path, L"%s\\manifest.txt", p->wszOutDir);
    hManifest = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hManifest != INVALID_HANDLE_VALUE)
        WriteFile(hManifest, &bom, sizeof(bom), &written, NULL);

    for (i = 0; i < DEV_COUNT; i++) {
        if (g_bCancel)
            break;
        if (p->bDev[i])
            DumpOneDevice(p->hDlg, g_wszDevNames[i], p->wszOutDir, hManifest);
    }

    if (hManifest != INVALID_HANDLE_VALUE)
        CloseHandle(hManifest);

    if (g_bCancel)
        SetStatus(L"Cancelled.");
    else
        SetStatus(L"Finished. Check manifest.txt for CRCs.");

    PostMessage(p->hDlg, WM_DUMP_DONE, 0, (LPARAM)p);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Dialog                                                              */
/* ------------------------------------------------------------------ */
static void StartDump(HWND hDlg)
{
    DUMP_PARAMS *p;
    DWORD tid;
    int i;
    BOOL any = FALSE;

    if (g_bRunning)
        return;

    for (i = 0; i < DEV_COUNT; i++)
        if (IsDlgButtonChecked(hDlg, g_devIds[i]))
            any = TRUE;
    if (!any) {
        MessageBoxW(hDlg, L"Select at least one device.", L"Brain NAND Backup",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    p = (DUMP_PARAMS *)LocalAlloc(LPTR, sizeof(DUMP_PARAMS));
    if (p == NULL) {
        SetStatus(L"Out of memory");
        SetDlgItemTextW(hDlg, IDC_STATUS, g_szStatus);
        return;
    }

    p->hDlg = hDlg;
    for (i = 0; i < DEV_COUNT; i++)
        p->bDev[i] = (BOOL)IsDlgButtonChecked(hDlg, g_devIds[i]);

    GetDlgItemTextW(hDlg, IDC_OUTDIR, p->wszOutDir, MAX_PATH);
    if (p->wszOutDir[0] == L'\0')
        wsprintfW(p->wszOutDir, L"\\NAND_DUMP");
    CreateDirectoryW(p->wszOutDir, NULL);

    g_bCancel = FALSE;
    g_bRunning = TRUE;
    EnableWindow(GetDlgItem(hDlg, IDC_DUMP), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_SELECTALL), FALSE);
    EnableWindow(GetDlgItem(hDlg, IDC_CANCEL), TRUE);
    SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);

    g_hThread = CreateThread(NULL, 0, DumpThread, p, 0, &tid);
    if (g_hThread == NULL) {
        SetStatus(L"Cannot start dump thread");
        SetDlgItemTextW(hDlg, IDC_STATUS, g_szStatus);
        EnableWindow(GetDlgItem(hDlg, IDC_DUMP), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_SELECTALL), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_CANCEL), FALSE);
        LocalFree(p);
        g_bRunning = FALSE;
    }
}

static BOOL CALLBACK MainDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    HANDLE h;
    int i;

    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(hDlg, IDC_OUTDIR, L"\\NAND_DUMP");
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETRANGE, 0,
                            MAKELPARAM(0, 100));
        EnableWindow(GetDlgItem(hDlg, IDC_CANCEL), FALSE);

        /* Disable check boxes for devices that are not present. */
        for (i = 0; i < DEV_COUNT; i++) {
            h = CreateFileW(g_wszDevNames[i], GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE)
                EnableWindow(GetDlgItem(hDlg, g_devIds[i]), FALSE);
            else
                CloseHandle(h);
        }
        SetStatus(L"Select devices and press Dump.");
        SetDlgItemTextW(hDlg, IDC_STATUS, g_szStatus);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_DUMP:
        case IDOK:
            StartDump(hDlg);
            return TRUE;

        case IDC_CANCEL:
        case IDCANCEL:
            if (g_bRunning) {
                g_bCancel = TRUE;
                SetStatus(L"Cancelling...");
                SetDlgItemTextW(hDlg, IDC_STATUS, g_szStatus);
            } else {
                EndDialog(hDlg, 0);
            }
            return TRUE;

        case IDC_SELECTALL:
            for (i = 0; i < DEV_COUNT; i++)
                if (IsWindowEnabled(GetDlgItem(hDlg, g_devIds[i])))
                    CheckDlgButton(hDlg, g_devIds[i], BST_CHECKED);
            return TRUE;

        case IDC_ABOUT:
            MessageBoxW(hDlg,
                L"Brain NAND / ROM Backup v1.0\n\n"
                L"Backs up the raw contents of NAND block devices on a "
                L"SHARP Brain (Windows CE).\n"
                L"Device names are from the Brain Wiki hardware page.\n\n"
                L"Use only on your own device, for backup / preservation.\n"
                L"Do not redistribute copyrighted dictionary data.",
                L"About", MB_OK | MB_ICONINFORMATION);
            return TRUE;
        }
        return FALSE;

    case WM_DUMP_PROGRESS:
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, wParam, 0);
        SetDlgItemTextW(hDlg, IDC_STATUS, g_szStatus);
        UpdateWindow(hDlg);
        return TRUE;

    case WM_DUMP_DONE:
        SetDlgItemTextW(hDlg, IDC_STATUS, g_szStatus);
        SendDlgItemMessageW(hDlg, IDC_PROGRESS, PBM_SETPOS, 0, 0);
        EnableWindow(GetDlgItem(hDlg, IDC_DUMP), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_SELECTALL), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_CANCEL), FALSE);
        if (g_hThread != NULL) {
            CloseHandle(g_hThread);
            g_hThread = NULL;
        }
        if (lParam != 0)
            LocalFree((HLOCAL)lParam);
        g_bRunning = FALSE;
        return TRUE;

    case WM_CLOSE:
        if (g_bRunning) {
            g_bCancel = TRUE;
            SetStatus(L"Cancelling... close the window when done.");
            SetDlgItemTextW(hDlg, IDC_STATUS, g_szStatus);
            return TRUE;
        }
        EndDialog(hDlg, 0);
        return TRUE;
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShow)
{
    INITCOMMONCONTROLSEX icc;

    (void)hPrev;
    (void)lpCmdLine;
    (void)nShow;

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControls();
    InitCommonControlsEx(&icc);

    DialogBox(hInst, MAKEINTRESOURCE(IDD_MAIN), NULL, MainDlgProc);
    return 0;
}
