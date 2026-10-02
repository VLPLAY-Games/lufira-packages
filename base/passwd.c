// passwd.c — вынос "passwd" (kernel/shell/commands/users.c,
// command_passwd(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение). Видимый ввод пароля (не маскируется) — то
// же сознательное упрощение, что уже принято для su (нет мид-командного
// маскированного ввода вне самого shell.c). Тонкая обёртка над
// SYS_PASSWD.
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: passwd <new-password>\n");
        printf("       passwd -u <username> <new-password>  (root only)\n");
        return 1;
    }

    const char *username = NULL;
    const char *new_password;

    if (strcmp(argv[1], "-u") == 0) {
        if (argc < 4) {
            printf("Usage: passwd -u <username> <new-password>\n");
            return 1;
        }
        username = argv[2];
        new_password = argv[3];
    } else {
        new_password = argv[1];
    }

    long res = sys_passwd(username, new_password);
    if (res == 0) {
        printf("passwd: password updated%s%s\n", username ? " for " : "", username ? username : "");
        return 0;
    }

    if (res == -EPERM) printf("passwd: permission denied (only root can change another user's password)\n");
    else if (res == -ENOENT) printf("passwd: unknown user\n");
    else printf("passwd: failed to update password\n");
    return 1;
}
