// usbwrite.c — вынос "usbwrite" (kernel/shell/commands/usb.c,
// command_usbwrite(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение). Тонкая обёртка над SYS_USB_WRITE (root-only
// — см. комментарий у SYS_USB_WRITE в kernel/system/syscall/syscall.h).
#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>

static long parse_long(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: usbwrite <device> <lba> <text>\n");
        return 1;
    }

    long index = parse_long(argv[1]);
    unsigned long lba = (unsigned long)parse_long(argv[2]);
    const char *text = argv[3];

    struct lufira_usb_info info;
    if (sys_usb_info(index, &info) != 0) {
        printf("usbwrite: no such device: usb%ld\n", index);
        return 1;
    }
    if (lba > info.max_lba) {
        printf("usbwrite: LBA %lu out of range (max %u)\n", lba, info.max_lba);
        return 1;
    }

    unsigned char buf[512];
    if (info.block_size > sizeof(buf)) {
        printf("usbwrite: block size %u too large\n", info.block_size);
        return 1;
    }

    memset(buf, 0, info.block_size);
    size_t len = strlen(text);
    if (len >= info.block_size) len = info.block_size - 1;
    memcpy(buf, text, len);

    long n = sys_usb_write(index, lba, buf, info.block_size);
    if (n < 0) {
        printf("usbwrite: permission denied or write failed\n");
        return 1;
    }

    printf("usbwrite: wrote %ld bytes to usb%ld LBA %lu\n", n, index, lba);
    return 0;
}
