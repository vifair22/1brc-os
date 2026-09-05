# 1brc-os: a UEFI application that owns the whole machine to run the
# One Billion Row Challenge. clang cross-compiles to PE directly; no
# gnu-efi, no EDK2, no libc.

CC := clang
LD := lld-link

TARGET := --target=x86_64-unknown-windows

# Freestanding: no red zone (we field interrupts on these stacks later),
# no stack protector (no runtime to catch it). Codegen stays baseline
# x86-64: AVX is only legal after each core's XCR0 is set up, so -mavx2
# is confined to the brc/ hot loop objects when those land.
# -ffreestanding lives outside CFLAGS: it implies -fno-builtin, which
# turns the hot loop's memcpy() unaligned-load idiom into real calls
# with stack-resident operands (measured 4x on the parse). Kernel-layer
# objects get it; the brc/ compute objects must not.
FREESTANDING := -ffreestanding

CFLAGS := -std=c11 -O2 $(TARGET) \
	-fno-stack-protector -mno-red-zone \
	-fno-asynchronous-unwind-tables \
	-Wall -Wextra -Wpedantic -Wshadow -Wdouble-promotion -Wformat=2 \
	-Wmissing-prototypes -Wstrict-prototypes -Wmissing-declarations \
	-Wcast-align -Wcast-qual -Wnull-dereference -Wconversion \
	-Wsign-conversion \
	-Isrc

LDFLAGS := /subsystem:efi_application /entry:efi_main /nodefaultlib

SRCS := \
	src/efi/loader.c \
	src/efi/blockio.c \
	src/kernel/kmain.c \
	src/kernel/alloc.c \
	src/kernel/cpu.c \
	src/kernel/gdt.c \
	src/kernel/idt.c \
	src/kernel/paging.c \
	src/kernel/diag.c \
	src/kernel/acpi.c \
	src/kernel/smp.c \
	src/console/console.c \
	src/console/fmt.c \
	src/console/debugcon.c \
	src/lib/string.c \
	src/lib/sort.c \
	src/brc/worker.c

ASMS := src/kernel/trampoline.S

OBJS := $(SRCS:src/%.c=build/release/%.obj) \
	$(ASMS:src/%.S=build/release/%.obj)

EFI := build/bin/BOOTX64.EFI
ESP := build/esp

OVMF_CODE := /usr/share/edk2-ovmf/OVMF_CODE.fd
OVMF_VARS := /usr/share/edk2-ovmf/OVMF_VARS.fd

# KVM runs the guest's AVX2 natively, so in-guest cycle counts are
# real; TCG software-emulates every instruction and its numbers mean
# nothing. Falls back automatically if /dev/kvm is unavailable.
ACCEL := $(shell test -w /dev/kvm && echo "-enable-kvm -cpu host" \
	|| echo "-cpu max")

QEMU := qemu-system-x86_64
# Overridable per run: make run SMP=16 MEM=20G DATA=/path/to/big.img
SMP ?= 8
MEM ?= 4G
DATA ?= build/data.img

QEMU_FLAGS := -machine q35 $(ACCEL) -smp $(SMP) -m $(MEM) -net none \
	-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	-drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
	-drive format=raw,file=fat:rw:$(ESP) \
	-drive format=raw,file=$(DATA)

MARKER := finished:

.PHONY: all esp run test clean

all: $(EFI) compile_commands.json

$(EFI): $(OBJS)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) /out:$@ $(OBJS)

# -MJ drops a compile-db fragment per object; compile_commands.json is
# assembled from them so clangd sees the real flags. -MMD emits header
# depfiles: without them a struct edit in a shared header rebuilds only
# some objects and the two halves disagree on field offsets.
build/release/%.obj: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(FREESTANDING) -MMD -MF $@.d -MJ $@.json -c -o $@ $<

# The brc/ objects are the only ones allowed to emit AVX2/BMI: they run
# strictly after cpu_enable_simd programs XCR0 on the executing core.
# -Wno-cast-align: intrinsic-heavy code casts char* to vector types
# constantly; the unaligned paths use loadu and the aligned ones hit the
# 64-byte-aligned Entry table, both deliberate.
# -D__MM_MALLOC_H: hosted-mode immintrin.h pulls stdlib.h through
# mm_malloc.h; predefining the guard skips that one header (nothing
# here calls _mm_malloc).
build/release/brc/%.obj: src/brc/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -mavx2 -mbmi -mbmi2 -Wno-cast-align \
		-D__MM_MALLOC_H -MMD -MF $@.d -MJ $@.json -c -o $@ $<

-include $(SRCS:src/%.c=build/release/%.obj.d)

# Exception paths run with possibly-broken SIMD state; keep them off
# the vector registers entirely.
build/release/kernel/idt.obj: src/kernel/idt.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(FREESTANDING) -mgeneral-regs-only \
		-MMD -MF $@.d -MJ $@.json -c -o $@ $<

build/release/%.obj: src/%.S
	@mkdir -p $(@D)
	$(CC) $(TARGET) -c -o $@ $<

# Fragments come from the C objects only (the .S rule emits none), and
# the db rebuilds with every make so clangd never sees stale flags.
CDB_FRAGS := $(SRCS:src/%.c=build/release/%.obj.json)

# The last fragment's trailing comma must go: clangd rejects a database
# containing any entry without a "file" key, so no {} sentinel allowed.
compile_commands.json: $(OBJS)
	@echo '[' > $@
	@cat $(CDB_FRAGS) | sed '$$ s/,[[:space:]]*$$//' >> $@
	@echo ']' >> $@

# Small deterministic dataset for the QEMU loop; real datasets are
# wrapped with --from and dd'd to a partition.
build/data.img: tools/mkdataimg.py
	python3 tools/mkdataimg.py $@ --rows 1000000 --seed 1

build/golden.txt: build/data.img tools/golden.py
	python3 tools/golden.py build/data.img > $@

esp: $(EFI) build/data.img
	@mkdir -p $(ESP)/EFI/BOOT
	cp $(EFI) $(ESP)/EFI/BOOT/BOOTX64.EFI
	@test -f build/OVMF_VARS.fd || cp $(OVMF_VARS) build/OVMF_VARS.fd

run: esp
	$(QEMU) $(QEMU_FLAGS) -debugcon stdio

# Headless boot check: debugcon output to a file, hard timeout, then
# assert the milestone marker made it out.
test: esp
	rm -f build/debugcon.log
	timeout 60 $(QEMU) $(QEMU_FLAGS) \
		-display none -debugcon file:build/debugcon.log & \
	QPID=$$!; \
	for i in $$(seq 1 60); do \
		grep -q "$(MARKER)" build/debugcon.log 2>/dev/null && break; \
		sleep 1; \
	done; \
	kill $$QPID 2>/dev/null; wait $$QPID 2>/dev/null; \
	grep -q "$(MARKER)" build/debugcon.log || \
		{ echo "TEST FAIL (no marker)"; cat build/debugcon.log; exit 1; }; \
	grep -m1 '^{' build/debugcon.log | diff - build/golden.txt \
		>/dev/null && echo "TEST PASS (output matches golden)" || \
		{ echo "TEST FAIL (output differs from golden)"; exit 1; }

test: build/golden.txt

clean:
	rm -rf build
