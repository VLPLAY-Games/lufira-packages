#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Драйвер Intel 8254x ("e1000", 82540EM и близкие 82544/82545/82546),
 * PCI vendor 0x8086, MMIO BAR0, дескрипторные кольца TX/RX (16-байтные
 * "legacy" дескрипторы). QEMU `-device e1000` эмулирует 82540EM (0x100E).
 * v0.9.5: перенесён из kernel/drivers/net/e1000.c в userspace (netd).
 *
 * Прерывания не используются: IMS всегда 0, e1000_poll() читает бит DD
 * (Descriptor Done) дескриптора напрямую. netd однопоточный — в отличие от
 * kernel-side версии, здесь не нужен cli/sti-guard реентерабельности.
 */

void e1000_init(void);
int e1000_found(void);
const uint8_t *e1000_get_mac(void);
int e1000_send(const void *frame, uint16_t len);
void e1000_poll(void);
