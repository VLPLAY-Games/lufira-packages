// write.c — вынос "write" (kernel/shell/commands/filesystem.c,
// command_write(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение). "write <file> <text...>" перезаписывает
// файл целиком текстом из оставшихся argv, склеенных пробелом (как и
// старая версия) — создаёт файл, если его не было.

#include <stdio.h>
#include <lufira/syscall.h>
#include "../common/pathutil.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: write <file> <text>\n");
        printf("Example: write test.txt Hello World\n");
        return 1;
    }

    char abs_path[256];
    if (resolve_path(argv[1], abs_path, sizeof(abs_path)) != 0) {
        printf("write: %s: path too long\n", argv[1]);
        return 1;
    }

    long fd = sys_open(abs_path, O_CREAT | O_WRONLY | O_TRUNC, 0);
    if (fd < 0) {
        printf("write: cannot open '%s'\n", argv[1]);
        return 1;
    }

    long total = 0;
    for (int i = 2; i < argc; i++) {
        if (i > 2) { sys_write((int)fd, " ", 1); total++; }
        long len = 0;
        while (argv[i][len]) len++;
        sys_write((int)fd, argv[i], (unsigned long)len);
        total += len;
    }
    sys_close((int)fd);

    printf("'%s' written (%ld bytes)\n", argv[1], total);
    return 0;
}
