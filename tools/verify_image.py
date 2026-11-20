#!/usr/bin/env python3
"""Verify an image produced by nanddump (Brain NAND / ROM backup tool).

Usage:
    python verify_image.py DSK8.bin
    python verify_image.py DSK8.bin --extract raw.bin

The header is 84 bytes little-endian:
    8s III 32s IIIIII Q
"""

import argparse
import os
import struct
import sys
import zlib

MAGIC = b"BRNDMP01"
HEADER_FMT = "<8sIII32sIIIIIIQ"
HEADER_SIZE = struct.calcsize(HEADER_FMT)  # 84


def main():
    ap = argparse.ArgumentParser(description="Verify a Brain NAND dump image")
    ap.add_argument("image", help="path to a .bin dump")
    ap.add_argument("--extract", metavar="OUT",
                    help="also write the raw payload (without header) to OUT")
    args = ap.parse_args()

    if HEADER_SIZE != 84:
        sys.exit("internal error: unexpected header size %d" % HEADER_SIZE)

    with open(args.image, "rb") as f:
        hdr = f.read(HEADER_SIZE)
        if len(hdr) != HEADER_SIZE:
            sys.exit("file is too short to contain a dump header")

        (magic, hdr_size, version, flags, dev_raw, bps, sectors,
         size_lo, size_hi, crc_stored, reserved, ft) = struct.unpack(
            HEADER_FMT, hdr)

        if magic != MAGIC:
            sys.exit("bad magic (not a Brain dump image): %r" % magic)

        dev = dev_raw.decode("utf-16-le", "replace").split("\x00", 1)[0]
        total = (size_hi << 32) | size_lo

        print("Device      : %s" % dev)
        print("Version     : %d (header %d bytes)" % (version, hdr_size))
        print("Sector size : %d bytes" % bps)
        print("Sectors     : %d" % sectors)
        print("Payload size: %d bytes (%.2f MiB)" %
              (total, total / 1048576.0))
        print("Stored CRC  : %08X" % crc_stored)

        crc = 0xFFFFFFFF
        remaining = total
        out = open(args.extract, "wb") if args.extract else None
        try:
            while remaining > 0:
                chunk = f.read(min(1 << 20, remaining))
                if not chunk:
                    break
                crc = zlib.crc32(chunk, crc)
                if out:
                    out.write(chunk)
                remaining -= len(chunk)
        finally:
            if out:
                out.close()

        crc ^= 0xFFFFFFFF
        print("Computed CRC: %08X" % crc)

        if remaining != 0:
            print("RESULT      : NG (file truncated: %d bytes missing)" %
                  remaining)
            return 1

        if crc == crc_stored:
            print("RESULT      : OK")
            return 0
        print("RESULT      : NG (CRC mismatch)")
        return 1


if __name__ == "__main__":
    sys.exit(main())
