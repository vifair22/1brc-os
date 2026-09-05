#!/usr/bin/env python3
"""Compute the expected challenge output for a dataset image.

Reads a mkdataimg.py image (4096-byte header + payload), aggregates in
exact integer tenths, and prints the official {Name=min/mean/max, ...}
line. The mean goes through double arithmetic with round-half-away,
matching the Java reference implementation's (and therefore 1brc-os's)
rounding behavior.

Usage: golden.py data.img > golden.txt
"""

import struct
import sys


def llround(x):
    return int(x + 0.5) if x >= 0 else int(x - 0.5)


def tenths(t):
    sign = "-" if t < 0 else ""
    t = abs(t)
    return "%s%d.%d" % (sign, t // 10, t % 10)


def main():
    with open(sys.argv[1], "rb") as f:
        hdr = f.read(4096)
        if hdr[:16] != b"1BRC-OS-DATASET\x00":
            sys.exit("not a dataset image")
        payload_bytes, = struct.unpack("<Q", hdr[24:32])
        data = f.read(payload_bytes)

    stats = {}
    for line in data.decode().splitlines():
        name, _, val = line.rpartition(";")
        t = int(val.replace(".", ""))
        s = stats.get(name)
        if s is None:
            stats[name] = [t, t, t, 1]
        else:
            if t < s[0]:
                s[0] = t
            if t > s[1]:
                s[1] = t
            s[2] += t
            s[3] += 1

    parts = []
    for name in sorted(stats, key=lambda n: n.encode()):
        mn, mx, sm, cnt = stats[name]
        mean = llround(sm / 10.0 / cnt * 10.0)
        parts.append("%s=%s/%s/%s" % (name, tenths(mn), tenths(mean),
                                      tenths(mx)))
    sys.stdout.write("{" + ", ".join(parts) + "}\n")


if __name__ == "__main__":
    main()
