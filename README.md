# Brain NAND / ROM Backup Tool

SHARP 製電子辞書 **Brain**（Windows CE）の実機が持つ NAND ブロックデバイスを、本体上で動作するアプリから丸ごとバックアップするツールです。

[Brain Wiki](https://brain.fandom.com/ja/wiki/Brain_Wiki) の [ハードウェア](https://brain.fandom.com/ja/wiki/%E3%83%8F%E3%83%BC%E3%83%89%E3%82%A6%E3%82%A7%E3%82%A2) ページに記載されたデバイスプレフィックスを元に、生のブロックを読み出してイメージファイルに保存します。

## 重要な注意

- **自分が所有する実機のバックアップ・保全・解析目的でのみ使用してください。**
- ダンプしたイメージには辞書データやファームウェアといった著作物が含まれます。Wiki にもある通り、**権利者許諾のない ROM の配布は著作権侵害**です。イメージを公開・共有しないでください。
- 解析・改造は自己責任で行ってください。読み取り専用 (`GENERIC_READ`) ですが、機種によっては挙動が異なる可能性があります。事前に重要なデータは別途バックアップしてください。
- 第 5 世代 (2021 年以降) は既存アプリの動作方法が不明と Wiki に記載されています。本ツールは主に第 1〜4 世代を対象としています。

## 対応デバイス（Wiki の「デバイスのプレフィックス」表より）

| チェックボックス | デバイス名 | 内容 (Wiki 記載) | DLL |
| --- | --- | --- | --- |
| NDF1: | NandBase | NAND ベース | `nandbase.dll` |
| DSK6: | NandDic | 辞書データ領域 | `nanddic.dll` |
| DSK7: | NandExtra | 追加領域 | `nandextra.dll` |
| DSK8: | NandSystem | OS / システム領域 | `nandsys.dll` |
| DSK9: | NandUser | ユーザー領域 | `nanduser.dll` |

起動時に `CreateFile` で存在を確認し、見つからないデバイスのチェックボックスは無効化します。「Select all」で有効なものをまとめて選択できます。

## ファイル構成

```
TXDic/
├── README.md             このファイル
├── src/
│   ├── nanddump.c        本体 (Win32 C / C89 準拠)
│   ├── nanddump.rc       ダイアログリソース
│   └── resource.h        コントロール ID
└── tools/
    └── verify_image.py   PC 側でイメージの CRC を検証するスクリプト
```

## ビルド方法

Wiki の [開発環境・SDK](https://brain.fandom.com/ja/wiki/%E9%96%8B%E7%99%BA%E7%92%B0%E5%A2%83%E3%83%BBSDK) に記載された環境でビルドできます。コードは C89 / C++98 の範囲で書いており、新しい規格を必要としません。

### Microsoft eMbedded Visual C++ 4.0 (evc4)

1. 新規の **WCE Application** プロジェクトを作る。
2. `src/nanddump.c`, `src/nanddump.rc`, `src/resource.h` を追加する。
3. リンク時に `commctrl.lib`, `coredll.lib` が入っていることを確認する。
4. ターゲット CPU (ARM など) を実機に合わせてビルドする。

### Visual Studio 2005 / 2008 + CE SDK

`Win32 Smart Device Project` として同じファイルを追加してビルドします。SDK は Wiki 記載の `Toradex CE6 SDK` などを使用してください。

### GitHub Actions (CeGCC / arm-mingw32ce)

`.github/workflows/build.yml` により、push / PR / 手動実行で ARM 用 `nanddump.exe` をビルドします。
ビルド環境には公開 Docker イメージ `ghcr.io/enlyze/windows-ce-build-environment-arm:latest`（CeGCC 9.3.0, `arm-mingw32ce`）を使用するため、Windows CE SDK のインストールは不要です。成果物は Artifacts にアップロードされます。

ローカルで同じビルドを再現する場合:

```
arm-mingw32ce-gcc -std=gnu89 -O2 -Wall -c src/nanddump.c -o nanddump.o
arm-mingw32ce-windres src/nanddump.rc -o nanddump_res.o
arm-mingw32ce-gcc nanddump.o nanddump_res.o -o nanddump.exe -lcommctrl
```

※ `diskio.h` は CeGCC に含まれないため、ソース側で `ND_DISK_INFO` / `ND_IOCTL_DISK_GETINFO` を定義しています（値は CE 5.0/6.0 の `diskio.h` に準拠）。

## 使い方

1. ビルドした `nanddump.exe` を実機にコピーして起動する。
2. ダンプしたいデバイスをチェックする（通常は `DSK8:` と `DSK6:` など）。
3. **Output folder** に十分な空き容量のあるフォルダを指定する（例: `\Storage Card\NAND_DUMP`）。
4. **Dump** を押す。進捗バーとステータスが更新される。**Cancel** で中断可能。
5. 完了後、指定フォルダに以下が生成される:
   - `<デバイス名>.bin` … 生データ + ヘッダ (例: `DSK8.bin`)
   - `manifest.txt` … 各イメージのサイズ / セクタ情報 / CRC32

### 出力イメージの形式

先頭 84 バイトのヘッダに続けて、デバイスの生データがそのまま並びます。数値はすべてリトルエンディアンです。

| オフセット | サイズ | 内容 |
| --- | --- | --- |
| 0 | 8 | マジック `"BRNDMP01"` |
| 8 | 4 | ヘッダサイズ (84) |
| 12 | 4 | バージョン (1) |
| 16 | 4 | フラグ (0) |
| 20 | 32 | デバイス名 (UTF-16LE, `L"DSK8:"` など) |
| 52 | 4 | セクタバイト数 |
| 56 | 4 | 全セクタ数 |
| 60 | 4 | データサイズ 下位 32bit |
| 64 | 4 | データサイズ 上位 32bit |
| 68 | 4 | データの CRC32 |
| 72 | 4 | 予約 (0) |
| 76 | 8 | ダンプ開始時刻 (FILETIME) |
| 84 | - | 生データ |

CRC32 は生データ部分のみに対する標準的な IEEE CRC-32 (zlib と同一) です。

## 検証 (PC 側)

```bat
python tools/verify_image.py NAND_DUMP/DSK8.bin
```

CRC が一致すれば `OK`、不一致なら `NG` と表示されます。`--extract out.bin` を付けるとヘッダを除いた生データだけを取り出せます。

## 仕組み

- `CreateFile("DSKx:")` でブロックデバイスを開く（読み取り専用）。
- `IOCTL_DISK_GETINFO` でセクタサイズとセクタ数を取得。対応していない場合は `GetFileSize` → 順次読み出しにフォールバック。
- `SetFilePointer` (上位/下位 32bit) で 64bit オフセットを指定し、64 KiB 単位で `ReadFile` / `WriteFile`。
- 読み出しは別スレッドで行い、進捗は `WM_APP+1` で UI に通知するため、操作中も画面が固まりません。

## 参考

- Brain Wiki: https://brain.fandom.com/ja/wiki/Brain_Wiki
- ハードウェア (デバイスプレフィックス): https://brain.fandom.com/ja/wiki/%E3%83%8F%E3%83%BC%E3%83%89%E3%82%A6%E3%82%A7%E3%82%A2
- C/C++ で Brain 用アプリを開発する: https://brain.fandom.com/ja/wiki/C/C%2B%2B%E3%81%A7Brain%E7%94%A8%E3%82%A2%E3%83%97%E3%83%AA%E3%82%92%E9%96%8B%E7%99%BA%E3%81%99%E3%82%8B
- 開発環境・SDK: https://brain.fandom.com/ja/wiki/%E9%96%8B%E7%99%BA%E7%92%B0%E5%A2%83%E3%83%BBSDK
- IOCTL_DISK_GETINFO / DISK_INFO: https://learn.microsoft.com/en-us/previous-versions/windows/embedded/ms901390(v=msdn.10)
