// umount.c — пара к mount.c (v0.9, фаза 1). Обёртка над SYS_UNMOUNT,
// которая сама сначала досинкает "грязные" секторы на устройство (см.
// vfs_fat_unmount()/vfs_ext2_unmount()/vfs_exfat_unmount()).
#include <stdio.h>
#include <lufira/syscall.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: umount <path>\n");
        return 1;
    }

    long r = sys_unmount(argv[1]);
    if (r == 0) {
        printf("Unmounted %s\n", argv[1]);
        return 0;
    }
    printf("umount: %s is not mounted\n", argv[1]);
    return 1;
}
