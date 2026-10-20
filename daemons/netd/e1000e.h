#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Драйвер Intel 8257x/8258x ("e1000e") — PCIe-поколение той же регистровой
 * модели, что и классический 8254x (см. e1000.h), другой список PCI device
 * id (82571-82583/ICH8-10). QEMU `-device e1000e` эмулирует 82574L
 * (0x10D3). v0.9.5: перенесён из kernel/drivers/net/e1000e.c в userspace.
 */

void e1000e_init(void);
int e1000e_found(void);
const uint8_t *e1000e_get_mac(void);
int e1000e_send(const void *frame, uint16_t len);
void e1000e_poll(void);
