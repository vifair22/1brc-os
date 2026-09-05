#!/usr/bin/env python3
"""FNV-1a 64 of a file, printed as bare hex.

Matches the hash 1brc-os prints for a quiet-mode run, so
`fnvhash.py golden.txt` == the on-screen value proves the kernel's
output line is byte-identical to the golden file.
"""

import sys


def main():
    h = 0xcbf29ce484222325
    with open(sys.argv[1], "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            for b in chunk:
                h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    print("%x" % h)


if __name__ == "__main__":
    main()
