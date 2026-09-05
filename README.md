# 1brc-os

A single-purpose operating system for the [One Billion Row
Challenge](https://github.com/gunnarmorling/1brc). The machine boots
from UEFI firmware into a purpose-built kernel, parses one billion
temperature measurements, prints the official result to the
framebuffer, and halts. There is no scheduler, no filesystem, no
storage driver, and no libc. The kernel, its UEFI bindings, and its
tooling are written from scratch in C11 with no third-party code.

The parse core is ported from [1brc-c], and its output is verified
byte-identical to that implementation's golden reference output on
every run (FNV-1a 64 over the full output line).

[1brc-c]: https://git.airies.net/vifair22/1brc-c

## Results

Measured with the official 1BRC protocol: five timed runs over the
resident dataset, best and worst discarded, remaining three averaged.
Each run covers table reset, parse, merge, and output formatting.
Dataset: 1,000,000,000 rows, 13.8 GB, 413 stations.

| Environment                      | Threads | Trimmed mean | Rate (wall)    |
| -------------------------------- | ------- | ------------ | -------------- |
| Ryzen 9 5900XT, bare metal       | 32      | 373 ms       | 1.2 cycles/row |
| Ryzen 9 5900XT, bare metal       | 16 phys | 467 ms       | 1.5 cycles/row |
| QEMU/KVM on the same host        | 16      | 381 ms       | 1.3 cycles/row |
| 1brc-c native Linux (reference)  | 32      | 330 ms       | 1.1 cycles/row |

Run-to-run spread on bare metal is under 1% (370-374 ms): with no
other software on the machine there is nothing to cause jitter, and
the boot-time steal probe confirms zero SMM interference during the
benchmark window.

Power-button to answer on screen is 84 s, of which 59 s is the
dataset load through firmware BlockIo from a USB stick (232 MB/s).
The load stage is reported separately from the benchmark, matching
the reference implementations, which read a page-cached file.

## Design

UEFI is used as a bootloader and storage driver, then discarded:

1. Firmware loads `BOOTX64.EFI` from the ESP, already in 64-bit long
   mode with identity-mapped memory.
2. While boot services are alive, the loader disables the firmware
   watchdog, captures the GOP framebuffer and ACPI RSDP, calibrates
   the TSC, and reads the dataset from a raw block device into RAM
   via `EFI_BLOCK_IO_PROTOCOL`. The dataset sits behind a one-block
   magic header on any device the firmware can enumerate; no
   filesystem is involved.
3. `ExitBootServices()`. The kernel installs its own GDT, an
   exception IDT, and a 512 GiB identity map built from 1 GiB pages,
   then wakes every core with INIT-SIPI-SIPI through a real-mode
   trampoline at 0x8000. Each core enables AVX2 (CR4/XCR0), requests
   full clocks (HWCR core-performance-boost, P-state 0), and parks in
   a dispatch barrier.
4. The challenge runs. There are no threads: each core executes one
   worker function to completion, pulling 8 MB line-aligned chunks
   off a shared atomic cursor. Results merge on the BSP, the official
   output line is built and hashed, and the stage summary prints.
5. The machine halts with the results on screen.

Two properties fall out of owning the whole machine. The dataset is
resident before the first parsed byte, so the hot loop takes zero
page faults. And the loader guarantees 64 zeroed bytes after the
dataset, so the SIMD parse needs no tail special-case anywhere.

## Source layout

| Path           | Contents                                              |
| -------------- | ----------------------------------------------------- |
| `src/efi/`     | Boot-services phase: UEFI bindings, loader, dataset load |
| `src/kernel/`  | Post-EBS core: GDT/IDT, paging, ACPI, SMP, CPU setup, diagnostics |
| `src/console/` | Framebuffer text renderer, kprintf, QEMU debugcon mirror |
| `src/lib/`     | Freestanding memcpy/memset/memmove/memcmp and heapsort |
| `src/brc/`     | The challenge port, kept diffable against 1brc-c      |
| `tools/`       | Dataset/boot image builders, golden generator, font converter |

The UEFI bindings (`src/efi/efi.h`) are a transcription of the
protocols this project touches, nothing more. The whole tree compiles
with `clang --target=x86_64-unknown-windows` (UEFI uses the MS x64
calling convention) and links to PE directly with lld-link.

## Building and testing

Requires clang, lld, QEMU, and OVMF (`sys-firmware/edk2-ovmf`).

```sh
make          # build/bin/BOOTX64.EFI
make run      # boot in QEMU with a generated 1M-row dataset
make test     # headless boot; asserts output matches tools/golden.py
```

`make test` is the regression gate: it boots the full system under
QEMU/KVM, runs the complete pipeline, and diffs the challenge output
against an independently computed golden line. QEMU runs use KVM when
`/dev/kvm` is writable, so in-guest cycle counts are meaningful.

`SMP=16 MEM=18G DATA=/path/img make run` overrides the VM shape and
dataset for full-size runs.

## Dataset and boot media

`tools/mkdataimg.py` wraps a measurements file (or generates a
synthetic one) behind the 4 KiB header the loader probes for. The
`--quiet` flag marks the image so the kernel prints the output's
length and FNV-1a hash instead of 10 KB of station data; verify
against a reference file with `tools/fnvhash.py`.

`tools/mkbootimg.py` builds a self-contained boot image: GPT, a 64 MiB
FAT16 ESP carrying `BOOTX64.EFI`, and the dataset as a raw second
partition. The FAT filesystem is written directly by the script, so
no root, loop devices, or mtools are required. The image boots three
ways:

- as a QEMU disk, for development;
- copied as a single file onto a Ventoy stick, which maps it as a
  virtual disk and boots it (no repartitioning, no boot entries);
- written with `dd` to any raw disk or partition.

`mkbootimg.py --update-efi IMG BOOTX64.EFI` rewrites only the ESP
inside an existing image, so a kernel change deploys in seconds
rather than recopying the 13 GB dataset.

## Boot-time diagnostics

Rebooting hardware is the most expensive operation in this project,
so every boot answers a full matrix of questions before the
benchmark:

| Line          | Question answered                                     |
| ------------- | ----------------------------------------------------- |
| `wake cr0`    | What CR0 state did the APs really wake with           |
| `diag: mtrr`  | Do all cores share one MTRR/PAT view of memory        |
| `diag: read bw` | Is the raw memory path healthy, solo and loaded     |
| `diag: steal` | Is firmware SMM stealing cycles during compute        |
| `diag: solo parse` | Per-core parse speed, BSP vs AP, against the userspace baseline |
| `diag: physical cores` | Scaling with one thread per physical core    |
| `clock`       | Effective GHz during the benchmark (APERF/MPERF)      |

## Performance notes

Three findings from bringing the port to parity with its userspace
performance, verified by measurement on both KVM and hardware:

- `-ffreestanding` implies `-fno-builtin`, which stops clang from
  folding the hot loop's `memcpy(&x, p, 8)` unaligned-load idiom into
  register loads; the hash operands become stack traffic and the
  parse runs 4x slower. The flag is scoped to kernel-layer objects
  only; the compute objects build without it. This cost is invisible
  to hardware-level investigation because it is identical in every
  environment.
- There is no OS governor on bare metal, so nobody requests boost
  unless the kernel does. Each core clears `HWCR.CpbDis` and requests
  P-state 0; APERF/MPERF confirms 4.13 GHz effective during the
  benchmark against a 3.3 GHz base clock.
- Firmware page tables are functionally correct but not tuned; the
  kernel replaces them with a 512 GiB identity map of 1 GiB pages on
  every core. Worth ~7% under KVM, less on bare metal.

## Limitations

- Requires x86-64 UEFI and a CPU with AVX2, BMI2, and 1 GiB pages
  (any Zen, Haswell or later).
- The dataset load runs through firmware BlockIo (232 MB/s observed
  via Ventoy on USB). A native NVMe driver is the obvious next step
  if load time ever matters.
- The machine halts rather than powering off; ACPI S5 requires an AML
  interpreter, which is out of scope.
- Up to 64 cores (xAPIC and x2APIC are both supported; APIC IDs must
  fit the MADT the firmware provides).
