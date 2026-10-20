#pragma once

// netd_hw.h — общие для всех трёх NIC-драйверов (rtl8139.c/e1000.c/
// e1000e.c) обёртки над привилегированными syscall'ами driver.h: port I/O
// и MMIO-регистры. Тот же принцип, что io_in*/io_out*/reg_read*/reg_write*
// в daemons/soundd.c, только общий заголовок — здесь несколько файлов, а
// не один.

#include <stdint.h>
#include <lufira/syscall.h>

#define PCI_COMMAND             0x04
#define PCI_COMMAND_IO          (1u << 0)
#define PCI_COMMAND_MEMORY      (1u << 1)
#define PCI_COMMAND_BUS_MASTER  (1u << 2)
#define PCI_COMMAND_INT_DISABLE (1u << 10)

// Читает PCI_COMMAND, ставит biты, пишет обратно — объединяет то, что в
// ядре было тремя отдельными pci_enable_io()/pci_enable_bus_master()/
// pci_disable_interrupts() в один read-modify-write.
void netd_pci_enable(const struct lufira_pci_device *dev, uint32_t bits);

static inline uint8_t io_in8(uint16_t port) { uint32_t v = 0; sys_io_in(port, 1, &v); return (uint8_t)v; }
static inline uint16_t io_in16(uint16_t port) { uint32_t v = 0; sys_io_in(port, 2, &v); return (uint16_t)v; }
static inline uint32_t io_in32(uint16_t port) { uint32_t v = 0; sys_io_in(port, 4, &v); return v; }
static inline void io_out8(uint16_t port, uint8_t v) { sys_io_out(port, 1, v); }
static inline void io_out16(uint16_t port, uint16_t v) { sys_io_out(port, 2, v); }
static inline void io_out32(uint16_t port, uint32_t v) { sys_io_out(port, 4, v); }

// MMIO — обычные волатильные нагрузки/сохранения через указатель,
// возвращённый sys_mmap_mmio(): без лишнего syscall'а на каждый доступ (в
// отличие от port I/O, память просто замаплена в адресное пространство
// процесса).
static inline uint32_t mmio_read32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static inline void mmio_write32(volatile uint8_t *base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}
