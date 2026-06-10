#ifndef DEVICES_IOAPIC_H_
#define DEVICES_IOAPIC_H_

#include <stdint.h>

void ioapic_init (void);
void ioapic_enable (int irq, int cpu);
void ioapic_set_base_address (void *vaddr);
void ioapic_set_id (uint8_t id);


#endif /* DEVICES_IOAPIC_H_ */
