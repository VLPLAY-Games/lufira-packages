// cat.c — вынос "cat" (kernel/shell/commands/filesystem.c, command_cat(),
// мёртвый код) в отдельную userspace-программу (v0.7 план, этап 5,
// продолжение: "как можно больше команд из ядра в пакеты"). Читает файл
// целиком в malloc()'нутый буфер (lseek END/SET — тот же приём, что
// read_whole() в dlpg.c/shell.c) и печатает как есть, без доп. заголовков
// вроде "--- file ---" у старой команды — ближе к обычному Unix cat.

#include <stdio.h>
#include <stdlib.h>
#include <lufira/syscall.h>
#include "../common/pathutil.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: cat <file>\n");
        return 1;
    }

    char abs_path[256];
    if (resolve_path(argv[1], abs_path, sizeof(abs_path)) != 0) {
        printf("cat: %s: path too long\n", argv[1]);
        return 1;
    }

    long fd = sys_open(abs_path, O_RDONLY, 0);
    if (fd < 0) {
        printf("cat: %s: no such file\n", argv[1]);
        return 1;
    }

    long size = sys_lseek((int)fd, 0, SEEK_END);
    sys_lseek((int)fd, 0, SEEK_SET);
    if (size <= 0) { sys_close((int)fd); return 0; }

    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) {
        printf("cat: not enough memory\n");
        sys_close((int)fd);
        return 1;
    }

    long total = 0;
    while (total < size) {
        long n = sys_read((int)fd, buf + total, (unsigned long)(size - total));
        if (n <= 0) break;
        total += n;
    }
    sys_close((int)fd);

    sys_write(1, (char *)buf, (unsigned long)total);
    free(buf);
    return 0;
}
