/* Exception IDT: every vector 0-31 dumps vector/RIP/error code to the
 * console and halts. This is the only debugger bare metal has; without
 * it every kernel bug is a silent triple-fault reset.
 */
#ifndef KERNEL_IDT_H
#define KERNEL_IDT_H

/* Builds the gate table once (idempotent) and loads IDTR on the
 * calling core. Call after gdt_load: gates use GDT_SEL_CODE. */
void idt_load(void);

#endif
