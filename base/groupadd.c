// groupadd.c — вынос "groupadd" (kernel/shell/commands/users.c,
// command_groupadd(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение). Тонкая обёртка над SYS_GROUPADD (см.
// комментарий у useradd.c — та же причина, почему это не просто
// дописывание файла из userspace: нужно обновить и кэш ядра).
#include <stdio.h>
#include <lufira/syscall.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: groupadd <groupname>\n");
        return 1;
    }

    long res = sys_groupadd(argv[1]);
    if (res == 0) {
        printf("groupadd: created group '%s'\n", argv[1]);
        return 0;
    }

    if (res == -EPERM) printf("groupadd: permission denied (root only)\n");
    else printf("groupadd: group '%s' already exists or could not be created\n", argv[1]);
    return 1;
}
