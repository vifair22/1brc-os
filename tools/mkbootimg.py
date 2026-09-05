#!/usr/bin/env python3
"""Build the single-file boot image for Ventoy / raw dd.

Layout: GPT with a 64 MiB FAT16 ESP holding EFI/BOOT/BOOTX64.EFI, and a
second raw partition carrying a mkdataimg.py dataset image verbatim.
Ventoy (UEFI) maps the .img as a virtual BlockIo disk and boots the
ESP; the loader then finds the dataset by its magic on partition 2 of
the same virtual disk. The image also works dd'd to a real disk.

No root, no mtools: sgdisk and mkfs.vfat operate on plain files, and
the one file the ESP needs is injected by writing FAT structures
directly (8.3 names only, which EFI/BOOT/BOOTX64.EFI satisfies).

Usage:
  mkbootimg.py OUT.img BOOTX64.EFI DATASET.img   build from scratch
  mkbootimg.py --update-efi IMG BOOTX64.EFI      rewrite only the ESP
                                                 inside an existing image
                                                 (fast path for kernel
                                                 changes: 64 MiB, not
                                                 the whole dataset)
"""

import os
import struct
import subprocess
import sys

ESP_MIB = 64
ALIGN_MIB = 1


def run(*cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def fat16_inject(esp, path_parts, payload):
    """Write payload as /EFI/BOOT/<name> into an empty FAT16 image."""
    bps, spc = struct.unpack_from("<HB", esp, 11)
    reserved, nfats = struct.unpack_from("<HB", esp, 14)
    root_ents, = struct.unpack_from("<H", esp, 17)
    fatsz, = struct.unpack_from("<H", esp, 22)

    fat_off = reserved * bps
    root_off = (reserved + nfats * fatsz) * bps
    data_off = root_off + root_ents * 32
    cluster_bytes = spc * bps

    next_free = [3]  # cluster 2 goes to /EFI, allocate upward from 3

    def alloc_chain(nbytes):
        n = max(1, (nbytes + cluster_bytes - 1) // cluster_bytes)
        chain = list(range(next_free[0], next_free[0] + n))
        next_free[0] += n
        for i, c in enumerate(chain):
            nxt = 0xFFFF if i == n - 1 else chain[i + 1]
            for f in range(nfats):
                struct.pack_into("<H", esp, fat_off + f * fatsz * bps + c * 2,
                                 nxt)
        return chain

    def cl_off(c):
        return data_off + (c - 2) * cluster_bytes

    def dirent(name83, attr, cluster, size):
        e = bytearray(32)
        e[0:11] = name83
        e[11] = attr
        struct.pack_into("<H", e, 26, cluster)
        struct.pack_into("<L", e, 28, size)
        return bytes(e)

    def name83(name):
        if "." in name:
            base, ext = name.rsplit(".", 1)
        else:
            base, ext = name, ""
        return (base.upper().ljust(8) + ext.upper().ljust(3)).encode()

    # /EFI at cluster 2 (mark it used), /EFI/BOOT next, then the file.
    for f in range(nfats):
        struct.pack_into("<H", esp, fat_off + f * fatsz * bps + 2 * 2, 0xFFFF)
    efi_cl = 2
    boot_cl = alloc_chain(1)[0]
    file_chain = alloc_chain(len(payload))

    esp[root_off:root_off + 32] = dirent(name83(path_parts[0]), 0x10, efi_cl,
                                         0)
    o = cl_off(efi_cl)
    esp[o:o + 32] = dirent(b".          ", 0x10, efi_cl, 0)
    esp[o + 32:o + 64] = dirent(b"..         ", 0x10, 0, 0)
    esp[o + 64:o + 96] = dirent(name83(path_parts[1]), 0x10, boot_cl, 0)
    o = cl_off(boot_cl)
    esp[o:o + 32] = dirent(b".          ", 0x10, boot_cl, 0)
    esp[o + 32:o + 64] = dirent(b"..         ", 0x10, efi_cl, 0)
    esp[o + 64:o + 96] = dirent(name83(path_parts[2]), 0x20, file_chain[0],
                                len(payload))

    for i, c in enumerate(file_chain):
        chunk = payload[i * cluster_bytes:(i + 1) * cluster_bytes]
        esp[cl_off(c):cl_off(c) + len(chunk)] = chunk


def part_start_sectors(img, num):
    txt = subprocess.run(["sgdisk", "-i", str(num), img], check=True,
                         capture_output=True, text=True).stdout
    for line in txt.splitlines():
        if line.startswith("First sector:"):
            return int(line.split()[2])
    sys.exit("sgdisk -i gave no start sector")


def build_esp(efi_path):
    esp = bytearray(ESP_MIB << 20)
    tmp = "/tmp/mkbootimg-esp.%d" % os.getpid()
    with open(tmp, "wb") as f:
        f.truncate(len(esp))
    run("mkfs.vfat", "-F", "16", "-n", "1BRCOS", tmp)
    with open(tmp, "rb") as f:
        esp[:] = f.read()
    os.unlink(tmp)
    with open(efi_path, "rb") as f:
        fat16_inject(esp, ["EFI", "BOOT", "BOOTX64.EFI"], f.read())
    return esp


def update_efi(img, efi_path):
    esp = build_esp(efi_path)
    p1 = part_start_sectors(img, 1) * 512
    with open(img, "r+b") as f:
        f.seek(p1)
        f.write(esp)
    print("%s: ESP rewritten (%d MiB at offset %d)" % (img, ESP_MIB, p1))


def main():
    if sys.argv[1] == "--update-efi":
        update_efi(sys.argv[2], sys.argv[3])
        return
    out, efi_path, data_path = sys.argv[1], sys.argv[2], sys.argv[3]
    data_bytes = os.path.getsize(data_path)

    data_mib = ((data_bytes + (1 << 20) - 1) >> 20) + 1
    total_mib = ALIGN_MIB + ESP_MIB + data_mib + 2  # +2 MiB GPT slack

    with open(out, "wb") as f:
        f.truncate(total_mib << 20)
    run("sgdisk", "-Z", out)
    run("sgdisk", "-n", "1:0:+%dM" % ESP_MIB, "-t", "1:EF00",
        "-c", "1:1brc-os", "-n", "2:0:0", "-t", "2:8300",
        "-c", "2:1brc-data", out)

    esp = build_esp(efi_path)

    p1 = part_start_sectors(out, 1) * 512
    p2 = part_start_sectors(out, 2) * 512
    with open(out, "r+b") as f:
        f.seek(p1)
        f.write(esp)
        f.seek(p2)
        with open(data_path, "rb") as src:
            while True:
                chunk = src.read(1 << 24)
                if not chunk:
                    break
                f.write(chunk)
    print("%s: %d MiB, dataset %d bytes at partition 2" %
          (out, total_mib, data_bytes))


if __name__ == "__main__":
    main()
