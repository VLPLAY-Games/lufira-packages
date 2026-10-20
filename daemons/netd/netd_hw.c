#include "netd_hw.h"

void netd_pci_enable(const struct lufira_pci_device *dev, uint32_t bits) {
    uint32_t cmd = 0;
    sys_pci_config_read(dev->bus, dev->device, dev->function, PCI_COMMAND, 2, &cmd);
    cmd |= bits;
    sys_pci_config_write(dev->bus, dev->device, dev->function, PCI_COMMAND, 2, cmd);
}
