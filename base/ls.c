// ls.c — вынос "ls" (kernel/shell/commands/filesystem.c, command_ls()) в
// отдельную userspace-программу (v0.7 план, этап 5, под-этап 1).
//
// Сознательно НЕ порт 1:1: kernel-native версия поддерживает "-l" (права/
// владелец/размер прямо из lufirafs_inode_t) — у ABI этого ядра всё ещё
// нет stat()-примитива (см. план фундамента v0.7), так что "-l" тут
// по-прежнему недоступен. Раскраска, впервые отложенная по той же
// причине ("нет syscall'а на цвет консоли"), с этапа 5 под-этапа 6 уже
// ДОСТУПНА через userspace/common/console.h (con_set_fg(), тот же, что
// уже используют color.c/fg.c/bg.c) — просто ещё не была сюда перенесена.
// "Исполняемый" определяется так же, как в kernel-native версии
// (is_executable_name() в filesystem.c): суффикс ".elf", раз нет прав
// доступа через syscall-ABI, чтобы проверить реальный бит исполнения.
//
// SYS_OPEN резолвит путь только от корня (userspace/common/pathutil.h) —
// без этого "ls" внутри /etc среагировал бы на голое "ls" листингом корня,
// а не /etc.

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>
#include "../common/pathutil.h"
#include "../common/console.h"

static int is_executable_name(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".elf") == 0;
}

int main(int argc, char **argv) {
    const char *path = (argc >= 2) ? argv[1] : NULL;

    char abs_path[256];
    if (resolve_path(path ? path : ".", abs_path, sizeof(abs_path)) < 0) {
        printf("ls: cannot resolve current directory\n");
        return 1;
    }

    long fd = sys_open(abs_path, O_RDONLY, 0);
    if (fd < 0) {
        printf("ls: cannot access '%s': No such file or directory\n", path ? path : ".");
        return 1;
    }

    // "." — настоящая запись в каждом каталоге LufiraFS, так что успешный
    // первый sys_readdir() отличает каталог от обычного файла (тот же приём,
    // что уже у du.c) без отдельного stat()-примитива, которого нет.
    struct lufira_dirent probe;
    int is_dir = sys_readdir((int)fd, &probe) > 0;
    sys_close((int)fd);

    if (!is_dir) {
        printf("%s\n", path ? path : ".");
        return 0;
    }

    fd = sys_open(abs_path, O_RDONLY, 0);
    if (fd < 0) {
        printf("ls: cannot access '%s': No such file or directory\n", path ? path : ".");
        return 1;
    }

    int count = 0;
    struct lufira_dirent ent;
    while (sys_readdir((int)fd, &ent) > 0) {
        int entry_is_dir = (ent.type == LUFIRA_FT_DIRECTORY);
        int entry_is_exec = !entry_is_dir && is_executable_name(ent.name);

        con_set_fg(entry_is_dir ? CON_LIGHT_BLUE : (entry_is_exec ? CON_LIGHT_GREEN : CON_WHITE));
        printf("%s  ", ent.name);
        con_set_fg(CON_WHITE);

        if (++count % 4 == 0) printf("\n");
    }
    sys_close((int)fd);
    if (count % 4 != 0) printf("\n");
    return 0;
}
