// shell.c — userspace-замена kernel-native шелла (kernel/shell/shell.c).
// Ring3-процесс: читает /dev/console блокирующим SYS_READ (console_input_
// read()/console_read(), drivers/input/input.c, fs/vfs/vfs.c), команды —
// fork()+exec()+wait(), Ctrl+C — SYS_SET_FOREGROUND поверх существующего
// process_signal(pid, SIGINT).
//
// Не полная замена: встроенные cd/pwd/exit/help/history/echo/clear/wait/
// su/mount/unmount плюс любая программа из /bin. Network/sound всё ещё
// без syscall'а — не портированы; shell.elf отвечает "unknown command".
//
// argv[]/envp[] для SYS_EXEC — только static, не стек: is_user_range_valid()
// проверяет фиксированные (MAX_EXEC_ARGS+1)*8 байт от начала массива, не
// до NUL — маленький стековый argv[] у верха 16KB стека может не пройти
// эту проверку даже будучи валидным (см. copy_user_string_array(),
// kernel/system/syscall/syscall.c).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lufira/syscall.h>
#include "../common/console.h"

#define LINE_MAX      256
#define MAX_ARGS      32
#define HISTORY_SIZE  16
#define PASSWD_PATH   "/etc/passwd"

static char g_username[32] = "?";
static char g_home[64] = "/";
static int g_have_user = 0;

static char g_history[HISTORY_SIZE][LINE_MAX];
static int g_history_count = 0;

// argv static — см. комментарий сверху файла. MAX_ARGS+1 слотов (терминатор).
static char *g_argv[MAX_ARGS + 1];

/* ===================== мелкие хелперы (тот же стиль, что dlpg.c) ===================== */

static long read_whole(const char *path, char *out, long out_cap) {
    long fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    long size = sys_lseek((int)fd, 0, SEEK_END);
    sys_lseek((int)fd, 0, SEEK_SET);
    if (size < 0 || size >= out_cap) { sys_close((int)fd); return -1; }

    long total = 0;
    while (total < size) {
        long n = sys_read((int)fd, out + total, (unsigned long)(size - total));
        if (n <= 0) break;
        total += n;
    }
    sys_close((int)fd);
    out[total] = '\0';
    return total;
}

// Мутирует line на месте (как split_argv() в kernel/shell/commands/
// filesystem.c) — режет по ':' ровно max раз.
static int split_fields(char *line, char *fields[], int max) {
    int count = 0;
    char *p = line;
    while (count < max) {
        fields[count++] = p;
        char *colon = strchr(p, ':');
        if (!colon) break;
        *colon = '\0';
        p = colon + 1;
    }
    return count;
}

static void lookup_user(void) {
    char buf[2048];
    long len = read_whole(PASSWD_PATH, buf, sizeof(buf));
    if (len < 0) return;

    long uid = sys_getuid();
    char *line = buf;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (*line) {
            char *fields[6];
            char linecopy[256];
            strncpy(linecopy, line, sizeof(linecopy) - 1);
            linecopy[sizeof(linecopy) - 1] = '\0';
            if (split_fields(linecopy, fields, 6) >= 6) {
                long field_uid = 0;
                for (const char *s = fields[1]; *s >= '0' && *s <= '9'; s++)
                    field_uid = field_uid * 10 + (*s - '0');
                if (field_uid == uid) {
                    strncpy(g_username, fields[0], sizeof(g_username) - 1);
                    strncpy(g_home, fields[5], sizeof(g_home) - 1);
                    g_have_user = 1;
                    break;
                }
            }
        }
        line = nl ? nl + 1 : line + strlen(line);
    }
}

/* ===================== запуск команд ===================== */

// Абсолютный путь (легаси "run /bin/foo.elf") — используем как есть, не
// приклеивая "/bin/" снова. Иначе PATH-подобное поведение: сначала
// "/bin/<name>", потом "/bin/<name>.elf" (как в kernel-native шелле).
static int resolve_bin_path(const char *name, char *out, int out_cap) {
    if (name[0] == '/') {
        long fd = sys_open(name, O_RDONLY, 0);
        if (fd < 0) return -1;
        sys_close((int)fd);
        strncpy(out, name, out_cap - 1);
        out[out_cap - 1] = '\0';
        return 0;
    }

    char candidate[128];
    int n = 0;
    const char *prefix = "/bin/";
    while (prefix[n] && n < out_cap - 1) { candidate[n] = prefix[n]; n++; }
    int i = 0;
    while (name[i] && n < out_cap - 1) { candidate[n++] = name[i++]; }
    candidate[n] = '\0';

    long fd = sys_open(candidate, O_RDONLY, 0);
    if (fd >= 0) { sys_close((int)fd); strncpy(out, candidate, out_cap - 1); out[out_cap-1]='\0'; return 0; }

    const char *suffix = ".elf";
    int si = 0;
    while (suffix[si] && n < out_cap - 1) { candidate[n++] = suffix[si++]; }
    candidate[n] = '\0';

    fd = sys_open(candidate, O_RDONLY, 0);
    if (fd >= 0) { sys_close((int)fd); strncpy(out, candidate, out_cap - 1); out[out_cap-1]='\0'; return 0; }

    return -1;
}

static long run_child(const char *path, char *const argv[]) {
    long pid = sys_fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        sys_exec(path, argv, (char **)0);
        printf("shell: exec failed: %s\n", path);
        sys_exit(127);
    }
    return pid;
}

static void dispatch(int argc, char *argv[]);

/* ===================== встроенные команды ===================== */

static void builtin_cd(int argc, char *argv[]) {
    const char *target = (argc >= 2) ? argv[1] : g_home;
    if (sys_chdir(target) != 0) {
        printf("cd: no such directory: %s\n", target);
    }
}

static void builtin_pwd(void) {
    char cwd[256];
    if (sys_getcwd(cwd, sizeof(cwd)) >= 0) printf("%s\n", cwd);
}

static void builtin_history(void) {
    for (int i = 0; i < g_history_count; i++) printf("%d  %s\n", i + 1, g_history[i]);
}

static void builtin_help(void) {
    printf("Builtins: cd pwd exit help history echo clear wait su mount unmount\n");
    printf("Packages (any other name tries /bin/<name> then /bin/<name>.elf):\n");
    printf("  du df free cpuload cp mv ls mkdir rm kill ps dlpg\n");
    printf("  cat touch write chmod chown fg bg color reset reboot shutdown devmode\n");
    printf("Append & to run in background (e.g. \"cpuload &\").\n");
}

// mount <usb-index> <prefix> — после успеха cd/ls/cat/cp/mkdir/rm видят
// корень флешки прямо под <prefix> (см. SYS_MOUNT, syscall.h) — отдельные
// команды чтения/записи (старые mountls/mountcat/mountwrite) не нужны.
static void builtin_mount(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: mount <usb-device-index> <prefix>  (e.g. \"mount 0 /mnt/usb0\")\n");
        return;
    }
    long usb_index = 0;
    for (const char *s = argv[1]; *s >= '0' && *s <= '9'; s++) usb_index = usb_index * 10 + (*s - '0');

    long res = sys_mount(argv[2], usb_index);
    if (res >= 0) { printf("Mounted usb%ld at %s\n", usb_index, argv[2]); return; }

    switch (res) {
        case -1: printf("mount: prefix must be an absolute path\n"); break;
        case -2: printf("mount: %s is already mounted\n", argv[2]); break;
        case -3: printf("mount: too many mounts - unmount one first\n"); break;
        case -4: printf("mount: no such usb device: usb%ld\n", usb_index); break;
        case -5: printf("mount: unsupported block size\n"); break;
        case -6: printf("mount: device too large\n"); break;
        case -7: printf("mount: not enough memory\n"); break;
        case -8: printf("mount: read failed\n"); break;
        case -9: printf("mount: not a FAT filesystem\n"); break;
        default: printf("mount: failed (%ld)\n", res); break;
    }
}

static void builtin_unmount(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: unmount <prefix>\n"); return; }
    if (sys_unmount(argv[1]) != 0) printf("unmount: no such mount: %s\n", argv[1]);
}

// Без эха — shell.elf сам отвечает за эхо (см. main()), скрыть пароль —
// просто не делать sys_write(1,&c,1). Backspace молча редактирует буфер.
static void read_hidden_line(char *out, int cap) {
    int len = 0;
    for (;;) {
        uint8_t c;
        long n = sys_read(0, &c, 1);
        if (n <= 0) continue;
        if (c == '\n') break;
        if (c == '\b' || c == 0x7F) {
            if (len > 0) len--;
            continue;
        }
        if (len < cap - 1 && c >= 32 && c < 127) out[len++] = (char)c;
    }
    out[len] = '\0';
    sys_write(1, "\n", 1);
}

// su [username] — пароль не аргументом командной строки (старый
// command_su так брал, но он был виден в строке ввода) — просим скрыто,
// проверяет ядро в sys_su(). После успеха — re-lookup identity (uid уже
// сменён) и cd в домашнюю папку.
static void builtin_su(int argc, char *argv[]) {
    const char *username = (argc >= 2) ? argv[1] : "root";

    printf("Password: ");
    char password[64];
    read_hidden_line(password, sizeof(password));

    if (sys_su(username, password) != 0) {
        printf("su: Authentication failure\n");
        return;
    }

    lookup_user();
    if (sys_chdir(g_home) != 0) {
        printf("su: warning: could not cd to home directory %s\n", g_home);
    }
}

/* ===================== главный цикл ===================== */

static void print_prompt(void) {
    printf("\n");
    con_set_fg(CON_LIGHT_CYAN);
    printf("[%s@lufiraos]", g_have_user ? g_username : "?");
    con_set_fg(CON_WHITE);

    char cwd[256];
    if (sys_getcwd(cwd, sizeof(cwd)) < 0) { cwd[0] = '/'; cwd[1] = '\0'; }

    int home_len = (int)strlen(g_home);
    if (g_have_user && home_len > 1 && strncmp(cwd, g_home, home_len) == 0 &&
        (cwd[home_len] == '\0' || cwd[home_len] == '/')) {
        printf(" ~%s $ ", cwd + home_len);
    } else {
        printf(" %s $ ", cwd);
    }
}

// Перерисовывает строку ввода целиком после любого изменения — не
// надеемся, что инкрементальное эхо (sys_write на символ/backspace)
// остаётся синхронным с буфером вечно. Работает чисто относительно (без
// абсолютных координат курсора, которых userspace не знает): инвариант —
// курсор стоит в old_cursor символах от начала строки, функция сама же
// его поддерживает (оставляет в new_cursor), так что следующий вызов
// может доверять этому инварианту.
static void redraw_line(const char *line, int old_len, int old_cursor, int new_len, int new_cursor) {
    for (int i = 0; i < old_cursor; i++) sys_write(1, "\b", 1);
    if (new_len > 0) sys_write(1, line, (unsigned long)new_len);
    if (old_len > new_len) {
        for (int i = 0; i < old_len - new_len; i++) sys_write(1, " ", 1);
        for (int i = 0; i < old_len - new_len; i++) sys_write(1, "\b", 1);
    }
    // Последний шаг: экран уже содержит правильный текст, курсор просто
    // двигаем к new_cursor без его перерисовки. Баг: raw '\b' тут стирал
    // бы только что написанные символы (вставка в середину "abcdef" ->
    // 'X' стирала хвост "def") — та же ошибка, что у стрелок до con_cursor_left().
    for (int i = 0; i < new_len - new_cursor; i++) con_cursor_left();
}

int main(void) {
    lookup_user();

    char line[LINE_MAX];

    for (;;) {
        print_prompt();

        int len = 0;
        int cursor = 0;
        int hist_browse = g_history_count;

        for (;;) {
            uint8_t c;
            long n = sys_read(0, &c, 1);
            if (n <= 0) continue;

            if (c == '\n') {
                sys_write(1, "\n", 1);
                break;
            }
            if (c == '\b' || c == 0x7F) {
                if (cursor > 0) {
                    int old_len = len, old_cursor = cursor;
                    for (int i = cursor - 1; i < len - 1; i++) line[i] = line[i + 1];
                    len--; cursor--;
                    redraw_line(line, old_len, old_cursor, len, cursor);
                }
                continue;
            }
            if (c == 0x01) { // стрелка влево — просто двигаем курсор, текст не меняется
                if (cursor > 0) { con_cursor_left(); cursor--; }
                continue;
            }
            if (c == 0x02) { // стрелка вправо — con_cursor_right() двигает курсор явно
                // (раньше тут перепечатывался line[cursor], что лишь выглядело как
                // перемещение, пока символ не менялся)
                if (cursor < len) { con_cursor_right(); cursor++; }
                continue;
            }
            if (c == 0x03 || c == 0x04) { // up/down arrow -> история
                int dir = (c == 0x03) ? -1 : 1;
                int new_pos = hist_browse + dir;
                if (new_pos < 0 || new_pos > g_history_count) continue;
                hist_browse = new_pos;
                int old_len = len, old_cursor = cursor;
                if (hist_browse < g_history_count) {
                    strncpy(line, g_history[hist_browse], LINE_MAX - 1);
                    line[LINE_MAX - 1] = '\0';
                    len = (int)strlen(line);
                } else {
                    len = 0;
                }
                cursor = len;
                redraw_line(line, old_len, old_cursor, len, cursor);
                continue;
            }
            if (c == '\t') {
                continue; // автодополнение — всё ещё не реализовано в этой версии
            }
            if (len < LINE_MAX - 1 && c >= 32 && c < 127) {
                int old_len = len, old_cursor = cursor;
                for (int i = len; i > cursor; i--) line[i] = line[i - 1];
                line[cursor] = (char)c;
                len++; cursor++;
                redraw_line(line, old_len, old_cursor, len, cursor);
            }
        }
        line[len] = '\0';

        if (len == 0) continue;

        if (g_history_count == 0 || strcmp(g_history[g_history_count - 1], line) != 0) {
            if (g_history_count < HISTORY_SIZE) {
                strncpy(g_history[g_history_count], line, LINE_MAX - 1);
                g_history_count++;
            } else {
                for (int i = 1; i < HISTORY_SIZE; i++) strcpy(g_history[i - 1], g_history[i]);
                strncpy(g_history[HISTORY_SIZE - 1], line, LINE_MAX - 1);
            }
        }

        int argc = 0;
        char *p = line;
        while (*p && argc < MAX_ARGS) {
            while (*p == ' ') p++;
            if (!*p) break;
            g_argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) { *p = '\0'; p++; }
        }
        g_argv[argc] = NULL;
        if (argc == 0) continue;

        dispatch(argc, g_argv);
    }

    return 0;
}

static void dispatch(int argc, char *argv[]) {
    int background = 0;
    if (argc > 0 && strcmp(argv[argc - 1], "&") == 0) {
        background = 1;
        argc--;
        argv[argc] = NULL;
    }
    if (argc == 0) return;

    if (strcmp(argv[0], "cd") == 0) { builtin_cd(argc, argv); return; }
    if (strcmp(argv[0], "pwd") == 0) { builtin_pwd(); return; }
    if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "logout") == 0) { sys_exit(0); }
    if (strcmp(argv[0], "help") == 0) { builtin_help(); return; }
    if (strcmp(argv[0], "history") == 0) { builtin_history(); return; }
    if (strcmp(argv[0], "clear") == 0) { con_clear(); return; }
    if (strcmp(argv[0], "su") == 0) { builtin_su(argc, argv); return; }
    if (strcmp(argv[0], "mount") == 0) { builtin_mount(argc, argv); return; }
    if (strcmp(argv[0], "unmount") == 0) { builtin_unmount(argc, argv); return; }
    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) printf("%s%s", argv[i], (i + 1 < argc) ? " " : "");
        printf("\n");
        return;
    }
    if (strcmp(argv[0], "wait") == 0) {
        if (argc < 2) { printf("Usage: wait <pid>\n"); return; }
        long pid = 0;
        for (const char *s = argv[1]; *s >= '0' && *s <= '9'; s++) pid = pid * 10 + (*s - '0');
        int status = 0;
        long w = sys_wait(pid, &status, 0);
        if (w < 0) printf("wait: no such child\n");
        else printf("PID %ld exited with code %d\n", w, status);
        return;
    }

    // "run"/"runbg"/"exec" — legacy-совместимые синонимы поверх той же
    // логики, что и голый вызов имени команды.
    const char *cmd = argv[0];
    char **run_argv = argv;
    int force_bg = background;
    int force_replace = 0;

    if (strcmp(cmd, "run") == 0 || strcmp(cmd, "runbg") == 0 || strcmp(cmd, "exec") == 0) {
        if (argc < 2) { printf("Usage: %s <program> [args...]\n", cmd); return; }
        if (strcmp(cmd, "runbg") == 0) force_bg = 1;
        if (strcmp(cmd, "exec") == 0) force_replace = 1;
        run_argv = argv + 1;
        cmd = run_argv[0];
    }

    char path[128];
    if (resolve_bin_path(cmd, path, sizeof(path)) != 0) {
        printf("Unknown command: %s\n", cmd);
        return;
    }

    if (force_replace) {
        sys_exec(path, run_argv, (char **)0);
        printf("shell: exec failed: %s\n", path);
        return;
    }

    long pid = run_child(path, run_argv);
    if (pid < 0) { printf("shell: fork failed\n"); return; }

    if (force_bg) {
        printf("Started background process PID %ld\n", pid);
        return;
    }

    sys_set_foreground(pid);
    int status = 0;
    sys_wait(pid, &status, 0);
    sys_set_foreground(0);
}
