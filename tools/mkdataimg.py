#!/usr/bin/env python3
"""Build a raw dataset image for 1brc-os.

Layout matches src/efi/blockio.h: a 4096-byte header block (16-byte
magic, u32 version, u32 reserved, u64 payload length) followed by the
raw measurements payload. The image is dd-able onto a partition or
attached to QEMU as a disk.

Usage:
  mkdataimg.py OUT --from FILE          wrap an existing measurements file
  mkdataimg.py OUT --rows N [--seed S]  generate N synthetic rows
"""

import argparse
import random
import struct
import sys

MAGIC = b"1BRC-OS-DATASET\x00"
HEADER_BYTES = 4096

STATIONS = [
    "Hamburg", "Bulawayo", "Palembang", "St. John's", "Cracow", "Bridgetown",
    "Istanbul", "Roseau", "Conakry", "Accra", "Abidjan", "Denpasar",
    "Ouagadougou", "Yaounde", "Wellington", "Tirana", "Vaduz", "Ushuaia",
    "Reykjavik", "Nuuk", "Dikson", "Yellowknife", "Fairbanks", "Anchorage",
    "Oslo", "Helsinki", "Riga", "Vilnius", "Minsk", "Kyiv", "Chisinau",
    "Bucharest", "Sofia", "Skopje", "Podgorica", "Sarajevo", "Zagreb",
    "Ljubljana", "Bratislava", "Budapest",
]


FLAG_QUIET = 1  # suppress the challenge line; print length + hash


def write_header(f, payload_bytes, flags):
    f.write(MAGIC)
    f.write(struct.pack("<IIQ", 1, flags, payload_bytes))
    f.write(b"\x00" * (HEADER_BYTES - 32))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("--from", dest="src")
    p.add_argument("--rows", type=int)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--quiet", action="store_true",
                   help="mark the image quiet: the OS prints only the "
                        "output's length and FNV-1a hash")
    args = p.parse_args()
    if bool(args.src) == bool(args.rows):
        sys.exit("exactly one of --from / --rows required")
    flags = FLAG_QUIET if args.quiet else 0

    with open(args.out, "wb") as f:
        write_header(f, 0, flags)
        payload = 0
        if args.src:
            with open(args.src, "rb") as src:
                while True:
                    chunk = src.read(1 << 24)
                    if not chunk:
                        break
                    f.write(chunk)
                    payload += len(chunk)
        else:
            rng = random.Random(args.seed)
            buf = []
            for _ in range(args.rows):
                t = rng.randint(-999, 999)
                buf.append("%s;%s%d.%d\n" % (rng.choice(STATIONS),
                                             "-" if t < 0 else "",
                                             abs(t) // 10, abs(t) % 10))
                if len(buf) == 100000:
                    payload += f.write("".join(buf).encode())
                    buf = []
            payload += f.write("".join(buf).encode())
        f.seek(0)
        write_header(f, payload, flags)
    print("%s: %d payload bytes" % (args.out, payload))


if __name__ == "__main__":
    main()
