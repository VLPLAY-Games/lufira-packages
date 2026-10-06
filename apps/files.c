// files.c — v0.8 (GUI+WM), этап 3 ("первые оконные приложения"): простой
// файловый менеджер поверх gui_listbox_t. Один клик по каталогу - заходит
// внутрь; один клик по файлу - открывает его в блокноте (fork()+sys_exec()
// /bin/notepad.elf с полным путём в argv[1], тот же паттерн запуска, что у
// desktop.c). Кнопка "Up" - подняться на уровень выше. Без "..", без
// иконок (прямое указание пользователя) - только список имён текстом.
#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>
#include <lufira/gui_widgets.h>

static char g_cwd[256] = "/";
static gui_listbox_t g_list;
static int g_is_dir[GUI_LISTBOX_MAX_ITEMS];
static gui_button_t g_btn_up;

// См. комментарий у g_launch_argv в desktop.c: argv[] ОБЯЗАН жить в
// static/global памяти, не на стеке launch() (вызывается из глубины
// событийного цикла main()) - иначе SYS_EXEC честно откажет с -EFAULT.
static char g_launch_path[256];
static char *g_launch_argv[3];

static void launch_notepad(const char *path) {
    int n = 0;
    while (path[n] && n < (int)sizeof(g_launch_path) - 1) { g_launch_path[n] = path[n]; n++; }
    g_launch_path[n] = '\0';

    long pid = sys_fork();
    if (pid < 0) return;
    if (pid == 0) {
        g_launch_argv[0] = "/bin/notepad.elf";
        g_launch_argv[1] = g_launch_path;
        g_launch_argv[2] = 0;
        sys_exec("/bin/notepad.elf", g_launch_argv, (char **)0);
        sys_exit(127);
    }
}

static void path_join(char *dst, size_t cap, const char *base, const char *name) {
    size_t blen = strlen(base);
    size_t nlen = strlen(name);
    int need_slash = (blen == 0 || base[blen - 1] != '/');
    size_t total = blen + (need_slash ? 1 : 0) + nlen;
    if (total >= cap) { dst[0] = '\0'; return; }
    memcpy(dst, base, blen);
    size_t p = blen;
    if (need_slash) dst[p++] = '/';
    memcpy(dst + p, name, nlen + 1);
}

static void path_to_parent(char *path) {
    size_t len = strlen(path);
    if (len <= 1) return; // уже корень
    size_t i = len - 1;
    if (path[i] == '/') { if (i == 0) return; i--; } // хвостовой '/' не бывает, но на всякий случай
    while (i > 0 && path[i] != '/') i--;
    if (i == 0) { path[1] = '\0'; } else { path[i] = '\0'; }
}

static void refresh_listing(void) {
    gui_listbox_clear(&g_list);
    long fd = sys_open(g_cwd, O_RDONLY, 0);
    if (fd < 0) return;
    struct lufira_dirent ent;
    while (sys_readdir((int)fd, &ent) > 0) {
        // "." и ".." — настоящие записи в каждом каталоге LufiraFS (см.
        // lufirafs.c) — скрываем оба: подняться на уровень выше уже есть
        // отдельная кнопка "Up" (path_to_parent(), не "сырой" ".." в пути).
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) continue;
        int idx = gui_listbox_add_item(&g_list, ent.name);
        if (idx >= 0) g_is_dir[idx] = (ent.type == LUFIRA_FT_DIRECTORY);
    }
    sys_close((int)fd);
}

static void redraw(int win) {
    sys_win_fill(win, 0x1a1a24);
    sys_win_draw_text(win, 10, 8, g_cwd, 0xaaaaaa);
    gui_button_draw(win, &g_btn_up);
    gui_listbox_draw(win, &g_list);
}

int main(void) {
    int win = sys_win_create(320, 100, 340, 300, "Files");
    if (win < 0) { printf("sys_win_create FAIL\n"); return 1; }

    gui_button_init(&g_btn_up, 250, 4, 80, 20, "Up");
    gui_listbox_init(&g_list, 10, 30, 320, 260);
    refresh_listing();
    redraw(win);

    int running = 1;
    while (running) {
        struct lufira_gui_event ev;
        while (sys_win_poll_event(win, &ev)) {
            if (ev.type == LUFIRA_GUI_EVENT_CLOSE) { running = 0; break; }
            if (ev.type == LUFIRA_GUI_EVENT_MOUSE_DOWN) {
                if (gui_button_contains(&g_btn_up, ev.x, ev.y)) {
                    path_to_parent(g_cwd);
                    refresh_listing();
                    redraw(win);
                    continue;
                }
                int idx = gui_listbox_click(&g_list, ev.x, ev.y);
                if (idx >= 0) {
                    if (g_is_dir[idx]) {
                        char next[256];
                        path_join(next, sizeof(next), g_cwd, g_list.items[idx]);
                        if (next[0]) {
                            int n = 0;
                            while (next[n] && n < (int)sizeof(g_cwd) - 1) { g_cwd[n] = next[n]; n++; }
                            g_cwd[n] = '\0';
                            refresh_listing();
                        }
                    } else {
                        char full[256];
                        path_join(full, sizeof(full), g_cwd, g_list.items[idx]);
                        if (full[0]) launch_notepad(full);
                    }
                    redraw(win);
                }
            }
        }
        sys_msleep(16);
    }

    sys_win_destroy(win);
    return 0;
}
