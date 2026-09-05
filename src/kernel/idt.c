/* Compiled with -mgeneral-regs-only: these paths run before or during
 * broken SIMD state and must not touch vector registers.
 */
#include "kernel/idt.h"

#include <stdint.h>

#include "console/fmt.h"
#include "kernel/gdt.h"

typedef struct {
    uint16_t off0;
    uint16_t sel;
    uint8_t ist;
    uint8_t type;
    uint16_t off1;
    uint32_t off2;
    uint32_t zero;
} __attribute__((packed)) idt_gate;

static idt_gate idt[32] __attribute__((aligned(16)));

/* First faulting core wins the console; the rest halt quietly instead
 * of interleaving glyphs into its dump. */
static volatile int dump_lock;

__attribute__((noreturn)) void isr_dump(uint64_t vec, uint64_t rip,
                                        uint64_t err);
__attribute__((noreturn)) void isr_dump(uint64_t vec, uint64_t rip,
                                        uint64_t err)
{
    if (!__atomic_exchange_n(&dump_lock, 1, __ATOMIC_ACQUIRE)) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("\nEXCEPTION vec %llu err %llx rip %llx cr2 %llx\n", vec,
                err, rip, cr2);
    }
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* Stubs: the CPU pushed [err?][rip][cs][rflags][rsp][ss]. Terminal
 * handlers never iretq, so each stub just marshals vector, rip and
 * error code into the MS-ABI argument registers and calls the dump
 * (with the 40-byte shadow+alignment adjustment a call site owes). */
#define ISR_NOERR(n)                                                       \
    __attribute__((naked)) static void isr##n(void)                        \
    {                                                                      \
        __asm__ volatile("movq (%%rsp), %%rdx\n\t"                         \
                         "movl $" #n ", %%ecx\n\t"                         \
                         "xorl %%r8d, %%r8d\n\t"                           \
                         "subq $40, %%rsp\n\t"                             \
                         "call isr_dump" ::);                              \
    }

#define ISR_ERR(n)                                                         \
    __attribute__((naked)) static void isr##n(void)                        \
    {                                                                      \
        __asm__ volatile("movq 8(%%rsp), %%rdx\n\t"                        \
                         "movq (%%rsp), %%r8\n\t"                          \
                         "movl $" #n ", %%ecx\n\t"                         \
                         "subq $40, %%rsp\n\t"                             \
                         "call isr_dump" ::);                              \
    }

ISR_NOERR(0) ISR_NOERR(1) ISR_NOERR(2) ISR_NOERR(3)
ISR_NOERR(4) ISR_NOERR(5) ISR_NOERR(6) ISR_NOERR(7)
ISR_ERR(8) ISR_NOERR(9) ISR_ERR(10) ISR_ERR(11)
ISR_ERR(12) ISR_ERR(13) ISR_ERR(14) ISR_NOERR(15)
ISR_NOERR(16) ISR_ERR(17) ISR_NOERR(18) ISR_NOERR(19)
ISR_NOERR(20) ISR_ERR(21) ISR_NOERR(22) ISR_NOERR(23)
ISR_NOERR(24) ISR_NOERR(25) ISR_NOERR(26) ISR_NOERR(27)
ISR_NOERR(28) ISR_ERR(29) ISR_ERR(30) ISR_NOERR(31)

static void (*const stubs[32])(void) = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7,
    isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

void idt_load(void)
{
    if (!idt[0].sel) {
        for (int i = 0; i < 32; i++) {
            uint64_t a = (uint64_t)(uintptr_t)stubs[i];
            idt[i].off0 = (uint16_t)a;
            idt[i].sel = GDT_SEL_CODE;
            idt[i].ist = 0;
            idt[i].type = 0x8e; /* present, interrupt gate */
            idt[i].off1 = (uint16_t)(a >> 16);
            idt[i].off2 = (uint32_t)(a >> 32);
            idt[i].zero = 0;
        }
    }

    struct __attribute__((packed)) {
        uint16_t limit;
        uint64_t base;
    } idtr = { sizeof(idt) - 1, (uint64_t)(uintptr_t)idt };
    __asm__ volatile("lidt %0" : : "m"(idtr));
}
