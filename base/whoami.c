// whoami.c — вынос "whoami" (kernel/shell/commands/users.c,
// command_whoami(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение).
//
// В отличие от useradd/groupadd/passwd, НЕ нужен новый syscall: SYS_GETUID/
// SYS_GETGID уже дают сырые uid/gid, а имя пользователя/группы — это просто
// текстовый /etc/passwd:/etc/group (username:uid:gid:salt_hex:hash_hex:home),
// который можно разобрать прямо отсюда через обычный sys_open/sys_read —
// то же самое, что ядро само делает в users.c, только в userspace. Файл
// всегда в синхроне с тем, что видит ядро: единственный способ что-то в
// него добавить — SYS_USERADD/SYS_GROUPADD, которые пишут и файл, и
// кэш ядра атомарно (см. users_add()/groups_add() в users.c).
#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>

// Нет atoi() в этом freestanding libc (userspace/base/kill.c уже заводит
// такой же локальный хелпер для той же причины).
static long parse_long(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

// Читает файл целиком в buf (ёмкость cap), возвращает длину или -1.
static long read_whole_file(const char *path, char *buf, long cap) {
    long fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    long total = 0;
    for (;;) {
        long n = sys_read((int)fd, buf + total, (unsigned long)(cap - 1 - total));
        if (n <= 0) break;
        total += n;
        if (total >= cap - 1) break;
    }
    sys_close((int)fd);
    buf[total] = '\0';
    return total;
}

// Построчно ищет запись "name:match_field_value:...", у которой числовое
// поле с индексом match_field равно match_value, и копирует поле 0 (имя)
// в out.
static int find_name_by_numeric_field(const char *buf, int match_field, long match_value,
                                       char *out, int out_size) {
    const char *line = buf;
    while (*line) {
        const char *nl = line;
        while (*nl && *nl != '\n') nl++;

        const char *p = line;
        char field[64];
        int idx = 0;
        char name[64] = "";
        while (p < nl) {
            int i = 0;
            while (p < nl && *p != ':' && i < (int)sizeof(field) - 1) field[i++] = *p++;
            field[i] = '\0';
            if (p < nl && *p == ':') p++;

            if (idx == 0) {
                int j = 0;
                while (field[j] && j < (int)sizeof(name) - 1) { name[j] = field[j]; j++; }
                name[j] = '\0';
            }
            if (idx == match_field && parse_long(field) == match_value) {
                int j = 0;
                while (name[j] && j < out_size - 1) { out[j] = name[j]; j++; }
                out[j] = '\0';
                return 0;
            }
            idx++;
        }

        line = (*nl == '\n') ? nl + 1 : nl;
    }
    return -1;
}

int main(void) {
    long uid = sys_getuid();
    long gid = sys_getgid();

    char passwd_buf[4096];
    char username[32] = "";
    if (read_whole_file("/etc/passwd", passwd_buf, sizeof(passwd_buf)) >= 0)
        find_name_by_numeric_field(passwd_buf, 1, uid, username, sizeof(username));

    char group_buf[1024];
    char groupname[32] = "";
    if (read_whole_file("/etc/group", group_buf, sizeof(group_buf)) >= 0)
        find_name_by_numeric_field(group_buf, 1, gid, groupname, sizeof(groupname));

    printf("uid=%ld", uid);
    if (username[0]) printf("(%s)", username);
    printf(" gid=%ld", gid);
    if (groupname[0]) printf("(%s)", groupname);
    printf("\n");
    return 0;
}
