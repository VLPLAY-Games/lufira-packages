// useradd.c — вынос "useradd" (kernel/shell/commands/users.c,
// command_useradd(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение). В отличие от whoami/chmod/chown, это НЕ
// просто чтение файла — нужно и дописать /etc/passwd, и обновить кэш
// пользователей ядра (g_users[]) атомарно, иначе su/whoami не увидят
// нового пользователя до перезагрузки (см. users_add() в users.c), так
// что реальная работа (groups_next_free_gid()/ensure_home_dir()/
// users_add()) осталась в ядре — SYS_USERADD (см. комментарий в
// kernel/system/syscall/syscall.h).
#include <stddef.h>
#include <stdio.h>
#include <lufira/syscall.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: useradd <username> <password> [group]\n");
        return 1;
    }

    const char *group = (argc >= 4) ? argv[3] : NULL;
    long res = sys_useradd(argv[1], argv[2], group);
    if (res == 0) {
        printf("useradd: created user '%s'\n", argv[1]);
        return 0;
    }

    if (res == -EPERM) printf("useradd: permission denied (root only)\n");
    else if (res == -ENOENT) printf("useradd: unknown group: %s\n", group ? group : "?");
    else printf("useradd: user '%s' already exists or could not be created\n", argv[1]);
    return 1;
}
