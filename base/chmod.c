// chmod.c — вынос "chmod" (kernel/shell/commands/users.c, command_chmod(),
// мёртвый код) в отдельную userspace-программу (v0.7 план, этап 5,
// продолжение). SYS_CHMOD уже резолвит путь от cwd_inode процесса и сам
// проверяет права (root или владелец) — здесь только парсинг восьмеричного
// режима и вызов.

#include <stdio.h>
#include <lufira/syscall.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: chmod <mode> <path>\n");
        return 1;
    }

    int mode = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '7') {
            printf("chmod: invalid mode (expected octal, e.g. 644)\n");
            return 1;
        }
        mode = mode * 8 + (*p - '0');
    }

    long r = sys_chmod(argv[2], mode & 0777);
    if (r != 0) {
        printf("chmod: cannot change '%s'\n", argv[2]);
        return 1;
    }
    return 0;
}
