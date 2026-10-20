#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Драйвер RTL8139 (Realtek, PCI 0x10EC:0x8139) — порт-I/O BAR0 (не MMIO),
 * без scatter-gather на приём: NIC пишет кадры в один физически непрерывный
 * кольцевой буфер. v0.9.5: перенесён из kernel/drivers/net/rtl8139.c в
 * userspace (netd) — PCI/port-I/O/DMA идут через syscall'ы driver.h вместо
 * прямого доступа к железу.
 *
 * Прерывания контроллера не используются — rtl8139_poll() (вызывается из
 * net_poll(), main.c) сама читает и подтверждает биты ISR; IMR всегда 0.
 * netd однопоточный — в отличие от kernel-side версии, здесь не нужен
 * cli/sti-guard реентерабельности (poll()/send() никогда не вызываются
 * параллельно сами из себя).
 */

void rtl8139_init(void);
int rtl8139_found(void);
const uint8_t *rtl8139_get_mac(void);
int rtl8139_send(const void *frame, uint16_t len);
void rtl8139_poll(void);
