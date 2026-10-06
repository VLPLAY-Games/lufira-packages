// notepad.c — v0.8 (GUI+WM), этап 3 ("первые оконные приложения"):
// настоящий блокнот с реальным чтением/записью файла через VFS (sys_open/
// sys_read/sys_write), заменяет игрушечный text_demo.c (тот остаётся в
// дереве как простой regression-smoke-test виджетов, просто больше не
// кнопка на Desktop). Многострочный, без курсорной навигации по тексту
// (Up/Down/Left/Right не трогают позицию внутри строк) — печать всегда
// идёт в КОНЕЦ последней строки, Enter начинает новую, Backspace в начале
// пустой строки возвращается к предыдущей. Простой текстовый редактор в
// духе первых блокнотов, не vim — соответствует масштабу остальной ОС.
//
// Без аргументов редактирует /notes.txt; с argv[1] — указанный файл (так
// файловый менеджер, files.c, открывает в блокноте то, на что кликнули).
#include <stdio.h>
#include <stdlib.h>
#include <lufira/syscall.h>
#include <lufira/gui_widgets.h>

#define NOTEPAD_MAX_LINES 24
#define NOTEPAD_LINE_MAX  64
#define NOTEPAD_DEFAULT_PATH "/notes.txt"

static char g_path[256];
static char g_lines[NOTEPAD_MAX_LINES][NOTEPAD_LINE_MAX];
static int g_line_count = 1; // всегда хотя бы одна (пустая) строка редактируется
static int g_dirty_flag = 0; // есть несохранённые изменения - влияет только на заголовок окна

static void load_file(void) {
    long fd = sys_open(g_path, O_RDONLY, 0);
    if (fd < 0) return; // файла ещё нет - начинаем с пустого листа

    long size = sys_lseek((int)fd, 0, SEEK_END);
    sys_lseek((int)fd, 0, SEEK_SET);
    if (size <= 0) { sys_close((int)fd); return; }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { sys_close((int)fd); return; }
    long total = 0;
    while (total < size) {
        long n = sys_read((int)fd, buf + total, (unsigned long)(size - total));
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    sys_close((int)fd);

    g_line_count = 0;
    int li = 0, ci = 0;
    for (long i = 0; i < total && g_line_count < NOTEPAD_MAX_LINES - 1; i++) {
        char c = buf[i];
        if (c == '\n') {
            g_lines[li][ci] = '\0';
            li++; ci = 0;
            g_line_count = li;
            continue;
        }
        if (ci < NOTEPAD_LINE_MAX - 1) g_lines[li][ci++] = c;
    }
    g_lines[li][ci] = '\0';
    if (g_line_count <= li) g_line_count = li + 1;
    free(buf);
}

static void save_file(void) {
    long fd = sys_open(g_path, O_CREAT | O_WRONLY | O_TRUNC, 0);
    if (fd < 0) return;
    for (int i = 0; i < g_line_count; i++) {
        long len = 0;
        while (g_lines[i][len]) len++;
        sys_write((int)fd, g_lines[i], (unsigned long)len);
        if (i < g_line_count - 1) sys_write((int)fd, "\n", 1);
    }
    sys_close((int)fd);
    g_dirty_flag = 0;
}

static gui_button_t g_btn_save;

static void redraw(int win) {
    sys_win_fill(win, 0x1a1a24);

    char header[80];
    int n = 0;
    const char *prefix = g_dirty_flag ? "* " : "";
    while (prefix[n]) { header[n] = prefix[n]; n++; }
    int pn = 0;
    while (g_path[pn] && n < (int)sizeof(header) - 1) { header[n++] = g_path[pn++]; }
    header[n] = '\0';
    sys_win_draw_text(win, 10, 8, header, 0xaaaaaa);

    for (int i = 0; i < g_line_count; i++) {
        sys_win_draw_text(win, 10, 24 + i * 11, g_lines[i], 0xe8e8e8);
    }
    // курсор - в конце последней строки
    long last_len = 0;
    while (g_lines[g_line_count - 1][last_len]) last_len++;
    sys_win_draw_rect(win, (int)(10 + last_len * 8), 24 + (g_line_count - 1) * 11,
                      1, 8, 0xe8e8e8);

    gui_button_draw(win, &g_btn_save);
}

int main(int argc, char **argv) {
    int n = 0;
    const char *path = (argc >= 2) ? argv[1] : NOTEPAD_DEFAULT_PATH;
    while (path[n] && n < (int)sizeof(g_path) - 1) { g_path[n] = path[n]; n++; }
    g_path[n] = '\0';

    g_lines[0][0] = '\0';
    load_file();

    int win = sys_win_create(260, 120, 380, 320, "Notepad");
    if (win < 0) { printf("sys_win_create FAIL\n"); return 1; }

    gui_button_init(&g_btn_save, 10, 320 - 36, 90, 26, "Save");
    redraw(win);

    int running = 1;
    while (running) {
        struct lufira_gui_event ev;
        while (sys_win_poll_event(win, &ev)) {
            if (ev.type == LUFIRA_GUI_EVENT_CLOSE) { running = 0; break; }
            if (ev.type == LUFIRA_GUI_EVENT_MOUSE_DOWN) {
                if (gui_button_contains(&g_btn_save, ev.x, ev.y)) {
                    save_file();
                    redraw(win);
                }
            }
            if (ev.type == LUFIRA_GUI_EVENT_KEY) {
                int key = ev.key_or_button;
                int changed = 0;
                int cur = g_line_count - 1;
                long cur_len = 0;
                while (g_lines[cur][cur_len]) cur_len++;

                if (key == '\n') {
                    if (g_line_count < NOTEPAD_MAX_LINES) {
                        g_lines[g_line_count][0] = '\0';
                        g_line_count++;
                        changed = 1;
                    }
                } else if (key == '\b') {
                    if (cur_len > 0) {
                        g_lines[cur][cur_len - 1] = '\0';
                        changed = 1;
                    } else if (g_line_count > 1) {
                        g_line_count--;
                        changed = 1;
                    }
                } else if (key >= 32 && key <= 126) {
                    if (cur_len < NOTEPAD_LINE_MAX - 1) {
                        g_lines[cur][cur_len] = (char)key;
                        g_lines[cur][cur_len + 1] = '\0';
                        changed = 1;
                    }
                }

                if (changed) {
                    g_dirty_flag = 1;
                    redraw(win);
                }
            }
        }
        sys_msleep(16);
    }

    sys_win_destroy(win);
    return 0;
}
