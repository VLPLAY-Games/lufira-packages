// touch.c — вынос "touch" (kernel/shell/commands/filesystem.c,
// command_touch(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение). Создаёт пустой файл через SYS_OPEN с
// O_CREAT — если файл уже есть, open() просто открывает его (как
// настоящий Unix touch — не ошибка), в отличие от старой кернел-native
// версии, которая печатала "already exists".

#include <stdio.h>
#include <lufira/syscall.h>
#include "../common/pathutil.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: touch <file>\n");
        return 1;
    }

    char abs_path[256];
    if (resolve_path(argv[1], abs_path, sizeof(abs_path)) != 0) {
        printf("touch: %s: path too long\n", argv[1]);
        return 1;
    }

    long fd = sys_open(abs_path, O_CREAT | O_RDONLY, 0);
    if (fd < 0) {
        printf("touch: cannot create '%s'\n", argv[1]);
        return 1;
    }
    sys_close((int)fd);
    return 0;
}
