// usbinfo.c — вынос "usbinfo" (kernel/shell/commands/usb.c,
// command_usbinfo(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение). Тонкая обёртка над SYS_USB_COUNT/
// SYS_USB_INFO (xhci_msd_device_count()/xhci_msd_get_info()).
#include <stdio.h>
#include <lufira/syscall.h>

int main(void) {
    long count = sys_usb_count();
    if (count <= 0) {
        printf("No USB mass storage devices found\n");
        return 0;
    }

    for (long i = 0; i < count; i++) {
        struct lufira_usb_info info;
        if (sys_usb_info(i, &info) != 0) continue;

        unsigned long total_kb = ((unsigned long)info.max_lba + 1) * info.block_size / 1024;
        printf("usb%ld: %u blocks x %u bytes = %lu KB\n", i, info.max_lba + 1, info.block_size, total_kb);
    }
    return 0;
}
