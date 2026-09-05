#include "kernel/smp.h"

#include "console/fmt.h"
#include "kernel/acpi.h"
#include "kernel/alloc.h"
#include "kernel/cpu.h"
#include "kernel/diag.h"
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/paging.h"
#include "lib/string.h"

/* Trampoline page layout; must match the .org/.set constants in
 * trampoline.S. */
#define TRAMP_BASE 0x8000ULL
#define TRAMP_SIPI_VECTOR 0x08
#define TRAMP_CR3 (TRAMP_BASE + 0xf00)
#define TRAMP_ENTRY (TRAMP_BASE + 0xf08)
#define TRAMP_STACK (TRAMP_BASE + 0xf10)
#define TRAMP_CPUIDX (TRAMP_BASE + 0xf18)
#define TRAMP_CR0_SEEN (TRAMP_BASE + 0xf20)

#define AP_STACK_BYTES (64 * 1024)

extern const uint8_t trampoline_start[];
extern const uint8_t trampoline_end[];

/* -------- LAPIC, both operating modes -------- */

#define IA32_APIC_BASE 0x1b
#define X2APIC_MODE_BIT (1u << 10)
#define MSR_X2APIC_ICR 0x830

static struct {
    volatile uint32_t *mmio; /* xAPIC; null when in x2APIC mode */
    uint64_t tsc_hz;
    uint32_t online; /* cores checked in, BSP included */
    uint32_t apic_ids[ACPI_MAX_CPUS]; /* by cpu_index, [0] = BSP */
} smp;

uint32_t smp_apic_id(uint32_t cpu_index)
{
    return cpu_index < ACPI_MAX_CPUS ? smp.apic_ids[cpu_index] : 0;
}

static void lapic_init(uint64_t base)
{
    if (rdmsr(IA32_APIC_BASE) & X2APIC_MODE_BIT)
        smp.mmio = 0;
    else
        smp.mmio = (volatile uint32_t *)base;
}

static void lapic_send(uint32_t apic_id, uint32_t icr_low)
{
    if (!smp.mmio) {
        wrmsr(MSR_X2APIC_ICR, ((uint64_t)apic_id << 32) | icr_low);
        return;
    }
    smp.mmio[0x310 / 4] = apic_id << 24;
    smp.mmio[0x300 / 4] = icr_low;
    while (smp.mmio[0x300 / 4] & (1u << 12)) /* delivery pending */
        cpu_pause();
}

static uint32_t bsp_apic_id(void)
{
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(1), "c"(0));
    return b >> 24;
}

/* -------- AP entry and the dispatch barrier -------- */

static struct {
    void (*volatile fn)(uint32_t, void *);
    void *volatile arg;
    volatile uint32_t generation;
    volatile uint32_t done;
    volatile uint32_t ack; /* bringup handshake, one AP at a time */
} bar;

/* APs land here from the trampoline, in long mode on their own stack,
 * interrupts off, and never leave. */
__attribute__((noreturn)) static void ap_entry(uint32_t cpu_index, void *unused)
{
    (void)unused;
    gdt_load();
    idt_load();
    paging_activate();
    cpu_enable_simd();
    cpu_boost();
    diag_ap_sample(cpu_index);
    __atomic_add_fetch(&smp.online, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&bar.ack, 1, __ATOMIC_RELEASE);

    uint32_t seen = 0;
    for (;;) {
        while (__atomic_load_n(&bar.generation, __ATOMIC_ACQUIRE) == seen)
            cpu_pause();
        seen = bar.generation;
        bar.fn(cpu_index, bar.arg);
        __atomic_add_fetch(&bar.done, 1, __ATOMIC_SEQ_CST);
    }
}

void smp_run(smp_fn fn, void *arg)
{
    bar.fn = fn;
    bar.arg = arg;
    __atomic_store_n(&bar.done, 0, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&bar.generation, 1, __ATOMIC_SEQ_CST);

    fn(0, arg); /* BSP is worker 0 */

    uint32_t aps = smp.online - 1;
    while (__atomic_load_n(&bar.done, __ATOMIC_ACQUIRE) != aps)
        cpu_pause();
}

uint32_t smp_cpu_count(void)
{
    return smp.online;
}

/* -------- bringup -------- */

static void patch64(uint64_t addr, uint64_t v)
{
    memcpy((void *)addr, &v, 8);
}

uint32_t smp_start_all(const boot_info *bi)
{
    cpu_topology topo;

    smp.tsc_hz = bi->tsc_hz;
    smp.online = 1;
    if (acpi_enumerate_cpus(bi->acpi_rsdp, &topo) != 0) {
        kprintf("smp: no topology, running BSP only\n");
        return 1;
    }
    lapic_init(topo.lapic_base);

    memcpy((void *)TRAMP_BASE, trampoline_start,
           (size_t)(trampoline_end - trampoline_start));
    /* The trampoline loads CR3 with a 32-bit mov, so APs must hop
     * through the firmware tables (always below 4 GiB) and switch to
     * the kernel tables in ap_entry once in long mode. */
    patch64(TRAMP_CR3, paging_firmware_cr3());
    patch64(TRAMP_ENTRY, (uint64_t)(uintptr_t)ap_entry);

    /* One AP at a time: patch its stack and index, INIT-SIPI-SIPI, and
     * wait for its check-in before reusing the parameter slots. Serial
     * bringup costs ~10 ms per core once, and removes every race from
     * the shared trampoline page. */
    uint32_t self = bsp_apic_id();
    uint32_t idx = 1;
    uint32_t cr0_seen = 0;
    smp.apic_ids[0] = self;
    for (uint32_t i = 0; i < topo.count; i++) {
        uint32_t id = topo.apic_ids[i];
        if (id == self)
            continue;

        uint8_t *stack = kalloc(AP_STACK_BYTES, 4096);
        patch64(TRAMP_STACK, (uint64_t)(uintptr_t)(stack + AP_STACK_BYTES));
        patch64(TRAMP_CPUIDX, idx);
        smp.apic_ids[idx] = id;
        __atomic_store_n(&bar.ack, 0, __ATOMIC_SEQ_CST);

        lapic_send(id, 0x00004500); /* INIT, edge, assert */
        tsc_delay_us(10000, smp.tsc_hz);
        lapic_send(id, 0x00004600 | TRAMP_SIPI_VECTOR);
        tsc_delay_us(300, smp.tsc_hz);
        if (!__atomic_load_n(&bar.ack, __ATOMIC_ACQUIRE))
            lapic_send(id, 0x00004600 | TRAMP_SIPI_VECTOR);

        uint64_t deadline = rdtsc() + smp.tsc_hz; /* 1 s */
        while (!__atomic_load_n(&bar.ack, __ATOMIC_ACQUIRE) &&
               rdtsc() < deadline)
            cpu_pause();
        if (!__atomic_load_n(&bar.ack, __ATOMIC_ACQUIRE)) {
            kprintf("smp: apic %u did not come up\n", id);
            continue;
        }
        cr0_seen |= *(volatile uint32_t *)TRAMP_CR0_SEEN;
        idx++;
    }

    kprintf("smp: %u/%u cores online, wake cr0 %x%s\n", smp.online,
            topo.count, cr0_seen,
            (cr0_seen & (3u << 29)) ? " (CD/NW set: fills were off)" : "");
    return smp.online;
}
