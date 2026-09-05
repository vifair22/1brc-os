/* AP startup trampoline. Copied to physical 0x8000 at runtime; APs are
 * pointed at it by SIPI vector 0x08 and wake here in 16-bit real mode.
 *
 * The path is the AMD-documented direct switch: with CR4.PAE, CR3 and
 * EFER.LME already staged, setting CR0.PG|PE in one write and far
 * jumping lands straight in 64-bit mode, skipping the protected-mode
 * stopover entirely.
 *
 * The parameter block and GDT live at fixed offsets inside this same
 * page (see the .org directives); smp.c patches them before each SIPI.
 * Keep TRAMP_* offsets in sync with the SMP_TRAMP_* defines in smp.c.
 */

.set TRAMP_BASE,   0x8000
.set P_CR3,        TRAMP_BASE + 0xf00
.set P_ENTRY,      TRAMP_BASE + 0xf08
.set P_STACK,      TRAMP_BASE + 0xf10
.set P_CPUIDX,     TRAMP_BASE + 0xf18
.set P_CR0_SEEN,   TRAMP_BASE + 0xf20
.set P_GDT,        TRAMP_BASE + 0xf40
.set P_GDTPTR,     TRAMP_BASE + 0xf60

.section .text
.global trampoline_start
.global trampoline_end

trampoline_start:
.code16
    cli
    cld
    /* DS = 0, so every data reference below uses the full physical
     * offset (P_* constants), all of which sit under 64 KiB. */
    xorw %ax, %ax
    movw %ax, %ds

    lgdtl P_GDTPTR

    /* CR4.PAE */
    movl %cr4, %eax
    orl $(1 << 5), %eax
    movl %eax, %cr4

    movl P_CR3, %eax
    movl %eax, %cr3

    /* EFER: LME + NXE (firmware page tables carry NX bits; without
     * NXE they would be reserved-bit faults). */
    movl $0xc0000080, %ecx
    rdmsr
    orl $((1 << 8) | (1 << 11)), %eax
    wrmsr

    /* CR0: PE and PG together, plus MP/NE; clear EM/TS so SSE decodes,
     * and critically clear CD/NW: an AP wakes from INIT with
     * CR0 = 0x60000010, i.e. CACHES DISABLED, and OR-only logic here
     * once shipped every AP into the benchmark uncached (measured as a
     * 6x per-core parse slowdown on real hardware). The pristine value
     * is stashed first so the kernel can report what firmware left. */
    movl %cr0, %eax
    movl %eax, P_CR0_SEEN
    andl $~((1 << 30) | (1 << 29) | (1 << 2) | (1 << 3)), %eax
    orl $0x80000023, %eax
    movl %eax, %cr0

    /* Far jump into 64-bit code: hand-encoded 66 EA ptr32 sel16. */
    .byte 0x66, 0xea
    .long TRAMP_BASE + (tramp64 - trampoline_start)
    .word 0x08

.code64
tramp64:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    movq P_STACK, %rsp
    /* MS x64 ABI: entered by jmp, so fake the post-call frame shape
     * (rsp % 16 == 8) and leave home space for the callee. */
    subq $40, %rsp
    movq P_CPUIDX, %rcx
    movq P_ENTRY, %rax
    jmpq *%rax

/* -------- patch area (offsets fixed by the P_* constants) -------- */

.org 0xf00
    .quad 0     /* P_CR3      */
    .quad 0     /* P_ENTRY    */
    .quad 0     /* P_STACK    */
    .quad 0     /* P_CPUIDX   */
    .quad 0     /* P_CR0_SEEN */

.org 0xf40
tramp_gdt:
    .quad 0x0000000000000000    /* null */
    .quad 0x00209a0000000000    /* 0x08: 64-bit code */
    .quad 0x0000920000000000    /* 0x10: data */

.org 0xf60
    .word 0x17                  /* limit: 3 descriptors - 1 */
    .quad P_GDT

.org 0xf80
trampoline_end:
