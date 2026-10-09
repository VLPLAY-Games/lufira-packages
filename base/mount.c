// mount.c — v0.9, фаза 1: обёртка над SYS_MOUNT, которой раньше не было
// нигде в userspace (sys_mount()/sys_unmount() уже существовали в libc —
// см. libc/include/lufira/syscall.h — но ни одна программа их не звала;
// единственный способ монтирования был мёртвый kernel-native
// command_mount(), оставшийся в kernel/shell/commands/mount.c с v0.6).
// Формат USB-устройства определяется автоматически в самом ядре (FAT,
// затем ext2, затем exFAT — см. sys_mount() в syscall.c) — эта программа
// не выбирает и не знает формат.
#include <stdio.h>
#include <lufira/syscall.h>

// Нет atoi() в этом freestanding libc — тот же локальный хелпер, что уже
// заводят usbread.c/kill.c по той же причине.
static long parse_long(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: mount <usb-device-index> <path>\n");
        printf("Example: mount 0 /mnt/usb0\n");
        return 1;
    }

    long index = parse_long(argv[1]);
    const char *path = argv[2];

    long r = sys_mount(path, index);
    if (r >= 0) {
        printf("Mounted usb%ld at %s\n", index, path);
        return 0;
    }

    switch (r) {
        case -2: printf("mount: %s is already mounted\n", path); break;
        case -3: printf("mount: no free mount slots\n"); break;
        case -4: printf("mount: no such device: usb%ld\n", index); break;
        case -5: printf("mount: unsupported block size\n"); break;
        case -6: printf("mount: device too large (8 MB cap)\n"); break;
        case -7: printf("mount: out of memory\n"); break;
        case -8: printf("mount: read error\n"); break;
        case -9: printf("mount: unrecognized filesystem (tried FAT, ext2, exFAT)\n"); break;
        default: printf("mount: failed (%ld)\n", r); break;
    }
    return 1;
}
