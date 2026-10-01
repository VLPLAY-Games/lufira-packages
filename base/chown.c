// chown.c — вынос "chown" (kernel/shell/commands/users.c, command_chown(),
// мёртвый код) в отдельную userspace-программу (v0.7 план, этап 5,
// продолжение). Упрощение по сравнению со старой версией: та принимала
// "<user>[:group]" и резолвила имя в uid через users_lookup_by_name()
// (кернел-внутренняя функция, не syscall) — здесь напрямую числовые
// uid/gid, как того и ждёт SYS_CHOWN; резолвинг имени пользователя в uid
// из userspace — отдельная, более крупная задача (нужен парсер /etc/passwd
// как в shell.c's lookup_user(), но для ПРОИЗВОЛЬНОГО имени, не только
// своего собственного uid).

#include <stdio.h>
#include <lufira/syscall.h>

static int parse_uint(const char *s) {
    int v = 0;
    for (; *s >= '0' && *s <= '9'; s++) v = v * 10 + (*s - '0');
    return v;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        printf("Usage: chown <uid> <gid> <path>\n");
        return 1;
    }

    int uid = parse_uint(argv[1]);
    int gid = parse_uint(argv[2]);

    long r = sys_chown(argv[3], uid, gid);
    if (r != 0) {
        printf("chown: cannot change '%s'\n", argv[3]);
        return 1;
    }
    return 0;
}
