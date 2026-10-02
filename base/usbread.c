// usbread.c — вынос "usbread" (kernel/shell/commands/usb.c,
// command_usbread(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение). Тонкая обёртка над SYS_USB_READ.
#include <stdio.h>
#include <lufira/syscall.h>

// Нет atoi() в этом freestanding libc (base/kill.c уже заводит такой же
// локальный хелпер по той же причине).
static long parse_long(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: usbread <device> <lba>\n");
        return 1;
    }

    long index = parse_long(argv[1]);
    unsigned long lba = (unsigned long)parse_long(argv[2]);

    struct lufira_usb_info info;
    if (sys_usb_info(index, &info) != 0) {
        printf("usbread: no such device: usb%ld\n", index);
        return 1;
    }
    if (lba > info.max_lba) {
        printf("usbread: LBA %lu out of range (max %u)\n", lba, info.max_lba);
        return 1;
    }

    unsigned char buf[512];
    if (info.block_size > sizeof(buf)) {
        printf("usbread: block size %u too large\n", info.block_size);
        return 1;
    }

    long n = sys_usb_read(index, lba, buf, sizeof(buf));
    if (n < 0) {
        printf("usbread: read failed\n");
        return 1;
    }

    printf("--- usb%ld LBA %lu (%u bytes) ---\n", index, lba, info.block_size);
    unsigned int show = info.block_size < 64 ? info.block_size : 64;
    for (unsigned int i = 0; i < show; i++) {
        printf("%x ", buf[i]);
        if ((i % 16) == 15) printf("\n");
    }
    printf("\n--- end ---\n");
    return 0;
}
