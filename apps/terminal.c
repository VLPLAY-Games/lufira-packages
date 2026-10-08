// terminal.c — v0.8 этап 4: настоящий терминал в GUI-окне — не
// переимплементация парсера команд, а fork()+exec() /bin/shell.elf с
// fd 0/1/2 подменёнными (sys_dup2()) на концы двух pipe() — "терминал
// оборачивает shell". Сам terminal.c — эмулятор экрана: сетка g_cols x
// g_rows (подгоняется под окно, см. TERM_MAX_COLS), разбирает ESC-
// последовательность, которую эмитит console.h (ESC 'f'/'b'/'p'/'c'/'l'/
// 'r' — цвет/очистка/позиция курсора), рисует через sys_win_draw_text/rect().
//
// Фон (ESC 'b') не моделируется по ячейкам (почти не меняется в реальном
// выводе) — только цвет текста, достаточно для ls/cpuload/du и т.п.
//
// Ctrl+C не форвардится в pipe: в текстовой консоли это SIGINT в обход
// байтового потока; для процесса за ОБЫЧНЫМ pipe такого пути нет, а
// форвардить сырой 0x05 было бы мусором. Известное ограничение версии.
#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>

// Было: TERM_COLS/TERM_ROWS константы, сетка фиксировалась один раз при
// sys_win_create(), LUFIRA_GUI_EVENT_RESIZE не слушалось — баг:
// "развернуть терминал на весь экран, область отображения всё равно как
// прежде" (WM растягивает пиксельный буфер, но содержимое оставалось
// старого размера в углу). Теперь сетка до TERM_MAX_COLS x TERM_MAX_ROWS
// (запас, без динамического перевыделения), активная часть g_cols/g_rows
// пересчитывается из размера окна при создании и на каждый RESIZE (main()).
#define TERM_MAX_COLS 200
#define TERM_MAX_ROWS 80
#define CHAR_W 8
#define CHAR_H 8
#define MARGIN 6

// Та же RGB-палитра, что ConsoleColor (kernel/lib/colors.h) — ESC 'f'
// передаёт индекс 0..15 в этой же палитре (con_set_fg(), common/console.h).
static const uint32_t PALETTE[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static char g_grid[TERM_MAX_ROWS][TERM_MAX_COLS];
static uint8_t g_fg[TERM_MAX_ROWS][TERM_MAX_COLS];
static int g_row_dirty[TERM_MAX_ROWS];
static int g_cur_x = 0, g_cur_y = 0;
static int g_cur_fg = 15; // CON_WHITE

// Активный размер сетки (<= TERM_MAX_COLS/ROWS), пересчитывается из
// пиксельного размера окна (compute_grid_size()); init в main() до term_clear().
static int g_cols = 64, g_rows = 24;

static void mark_dirty(int y) { if (y >= 0 && y < g_rows) g_row_dirty[y] = 1; }

static void term_clear(void) {
    for (int y = 0; y < g_rows; y++) {
        for (int x = 0; x < g_cols; x++) { g_grid[y][x] = ' '; g_fg[y][x] = (uint8_t)g_cur_fg; }
        mark_dirty(y);
    }
    g_cur_x = 0; g_cur_y = 0;
}

static void term_scroll(void) {
    for (int y = 1; y < g_rows; y++)
        for (int x = 0; x < g_cols; x++) { g_grid[y - 1][x] = g_grid[y][x]; g_fg[y - 1][x] = g_fg[y][x]; }
    for (int x = 0; x < g_cols; x++) { g_grid[g_rows - 1][x] = ' '; g_fg[g_rows - 1][x] = (uint8_t)g_cur_fg; }
    for (int y = 0; y < g_rows; y++) mark_dirty(y);
}

static void term_newline(void) {
    g_cur_x = 0;
    g_cur_y++;
    if (g_cur_y >= g_rows) { term_scroll(); g_cur_y = g_rows - 1; }
}

// win_w/win_h — клиентская область в пикселях (LUFIRA_GUI_EVENT_RESIZE /
// sys_win_create()). Клампим к [1, TERM_MAX_*] — запас покрывает любое
// разумное разрешение (см. TERM_MAX_COLS).
static void compute_grid_size(int win_w, int win_h, int *cols, int *rows) {
    int c = (win_w - 2 * MARGIN) / CHAR_W;
    int r = (win_h - 2 * MARGIN) / CHAR_H;
    if (c < 1) c = 1;
    if (r < 1) r = 1;
    if (c > TERM_MAX_COLS) c = TERM_MAX_COLS;
    if (r > TERM_MAX_ROWS) r = TERM_MAX_ROWS;
    *cols = c; *rows = r;
}

// Обрабатывает LUFIRA_GUI_EVENT_RESIZE: пересчитывает g_cols/g_rows. При
// увеличении новые клетки очищаются пробелом (иначе видны "призрачные"
// символы от прошлого большего размера). При уменьшении курсор поджимается
// внутрь новой границы (как и обычный скролл — курсор всегда внутри сетки).
static void term_resize_grid(int new_cols, int new_rows) {
    if (new_cols == g_cols && new_rows == g_rows) return;

    int old_cols = g_cols, old_rows = g_rows;

    // Новые строки целиком (y >= old_rows) — очищаем по всей новой ширине.
    for (int y = old_rows; y < new_rows; y++) {
        for (int x = 0; x < new_cols; x++) { g_grid[y][x] = ' '; g_fg[y][x] = (uint8_t)g_cur_fg; }
    }
    // Новые столбцы в УЖЕ существовавших строках (x >= old_cols).
    int shared_rows = old_rows < new_rows ? old_rows : new_rows;
    for (int y = 0; y < shared_rows; y++) {
        for (int x = old_cols; x < new_cols; x++) { g_grid[y][x] = ' '; g_fg[y][x] = (uint8_t)g_cur_fg; }
    }

    g_cols = new_cols;
    g_rows = new_rows;
    if (g_cur_x >= g_cols) g_cur_x = g_cols - 1;
    if (g_cur_y >= g_rows) g_cur_y = g_rows - 1;

    for (int y = 0; y < g_rows; y++) mark_dirty(y);
}

// Та же семантика, что put_char() в console.c — '\n'/'\r'/'\b'/'\t'
// разбираются так же, но пишем в свою сетку, не в framebuffer.
static void term_putch(char c) {
    if (c == '\n') { term_newline(); return; }
    if (c == '\r') { g_cur_x = 0; return; }
    if (c == '\b') {
        if (g_cur_x > 0) {
            g_cur_x--;
            g_grid[g_cur_y][g_cur_x] = ' ';
            g_fg[g_cur_y][g_cur_x] = (uint8_t)g_cur_fg;
            mark_dirty(g_cur_y);
        }
        return;
    }
    if (c == '\t') {
        for (int i = 0; i < 4; i++) term_putch(' ');
        return;
    }
    g_grid[g_cur_y][g_cur_x] = c;
    g_fg[g_cur_y][g_cur_x] = (uint8_t)g_cur_fg;
    mark_dirty(g_cur_y);
    g_cur_x++;
    if (g_cur_x >= g_cols) term_newline();
}

// Состояние парсера ESC-последовательности живёт между вызовами — байты
// приходят из pipe кусками, последовательность может разрезаться sys_read().
static int g_esc_state = 0; // 0 - обычный текст, 1 - ждём команду, 2 - собираем доп. байты
static char g_esc_cmd = 0;
static uint8_t g_esc_buf[4];
static int g_esc_need = 0, g_esc_have = 0;

static void term_feed_byte(uint8_t b) {
    int old_y = g_cur_y;

    if (g_esc_state == 0) {
        if (b == 0x1B) { g_esc_state = 1; return; }
        term_putch((char)b);
    } else if (g_esc_state == 1) {
        g_esc_cmd = (char)b;
        if (g_esc_cmd == 'f' || g_esc_cmd == 'b') { g_esc_need = 1; g_esc_have = 0; g_esc_state = 2; }
        else if (g_esc_cmd == 'p') { g_esc_need = 4; g_esc_have = 0; g_esc_state = 2; }
        else {
            if (g_esc_cmd == 'c') term_clear();
            else if (g_esc_cmd == 'l') { if (g_cur_x > 0) g_cur_x--; }
            else if (g_esc_cmd == 'r') { if (g_cur_x < g_cols - 1) g_cur_x++; }
            g_esc_state = 0;
        }
    } else {
        g_esc_buf[g_esc_have++] = b;
        if (g_esc_have >= g_esc_need) {
            if (g_esc_cmd == 'f') g_cur_fg = g_esc_buf[0] & 0x0F;
            else if (g_esc_cmd == 'p') {
                int x = ((int)g_esc_buf[0] << 8) | g_esc_buf[1];
                int y = ((int)g_esc_buf[2] << 8) | g_esc_buf[3];
                if (x >= 0 && x < g_cols) g_cur_x = x;
                if (y >= 0 && y < g_rows) g_cur_y = y;
            }
            // 'b' (фон) — см. шапку файла, сознательно игнорируется.
            g_esc_state = 0;
        }
    }

    mark_dirty(old_y);
    mark_dirty(g_cur_y);
}

static void term_redraw_dirty(int win) {
    for (int y = 0; y < g_rows; y++) {
        if (!g_row_dirty[y]) continue;
        g_row_dirty[y] = 0;

        int py = MARGIN + y * CHAR_H;
        sys_win_draw_rect(win, MARGIN, py, g_cols * CHAR_W, CHAR_H, 0x000000);

        int x = 0;
        while (x < g_cols) {
            uint8_t color = g_fg[y][x];
            int start = x;
            char buf[TERM_MAX_COLS + 1]; int n = 0;
            int has_visible = 0;
            while (x < g_cols && g_fg[y][x] == color) {
                buf[n] = g_grid[y][x];
                if (buf[n] != ' ') has_visible = 1;
                n++; x++;
            }
            buf[n] = '\0';
            if (has_visible) sys_win_draw_text(win, MARGIN + start * CHAR_W, py, buf, PALETTE[color]);
        }

        if (y == g_cur_y)
            sys_win_draw_rect(win, MARGIN + g_cur_x * CHAR_W, py, 1, CHAR_H, 0xffffff);
    }
}

int main(void) {
    int in_pipe[2], out_pipe[2];
    if (sys_pipe(in_pipe) != 0 || sys_pipe(out_pipe) != 0) {
        printf("terminal: sys_pipe FAIL\n");
        return 1;
    }

    long child_pid = sys_fork();
    if (child_pid < 0) { printf("terminal: sys_fork FAIL\n"); return 1; }

    if (child_pid == 0) {
        sys_dup2(in_pipe[0], 0);
        sys_dup2(out_pipe[1], 1);
        sys_dup2(out_pipe[1], 2);
        sys_close(in_pipe[0]);
        sys_close(in_pipe[1]);
        sys_close(out_pipe[0]);
        sys_close(out_pipe[1]);

        static char *argv[2];
        argv[0] = "/bin/shell.elf";
        argv[1] = 0;
        sys_exec("/bin/shell.elf", argv, (char **)0);
        sys_exit(127);
    }

    // Родитель пишет в in_pipe[1], читает из out_pipe[0] — лишние концы
    // закрываем сразу, иначе EOF ребёнка не увидим (reader/writer-счётчики
    // pipe_t, vfs.c, не дойдут до нуля, пока кто-то держит лишний конец).
    sys_close(in_pipe[0]);
    sys_close(out_pipe[1]);

    term_clear();

    int win_w = MARGIN * 2 + g_cols * CHAR_W;
    int win_h = MARGIN * 2 + g_rows * CHAR_H;
    int win = sys_win_create(80, 60, win_w, win_h, "Terminal");
    if (win < 0) { printf("terminal: sys_win_create FAIL\n"); return 1; }

    term_redraw_dirty(win);

    int running = 1;
    while (running) {
        struct lufira_gui_event ev;
        while (sys_win_poll_event(win, &ev)) {
            if (ev.type == LUFIRA_GUI_EVENT_CLOSE) { running = 0; break; }
            if (ev.type == LUFIRA_GUI_EVENT_KEY) {
                int key = ev.key_or_button;
                if (key != 0x05) { // Ctrl+C - см. шапку файла
                    char ch = (char)key;
                    sys_write(in_pipe[1], &ch, 1);
                }
            }
            if (ev.type == LUFIRA_GUI_EVENT_RESIZE) {
                // ev.x/ev.y — НОВЫЙ размер клиентской области в пикселях
                // (см. комментарий у LUFIRA_GUI_EVENT_RESIZE, syscall.h).
                int new_cols, new_rows;
                compute_grid_size(ev.x, ev.y, &new_cols, &new_rows);
                term_resize_grid(new_cols, new_rows);
                term_redraw_dirty(win);
            }
        }

        struct lufira_pollfd pfd = { out_pipe[0], LUFIRA_POLLIN, 0 };
        if (sys_poll(&pfd, 1, 0) > 0) {
            char buf[256];
            long n = sys_read(out_pipe[0], buf, sizeof(buf));
            if (n > 0) {
                for (long i = 0; i < n; i++) term_feed_byte((uint8_t)buf[i]);
                term_redraw_dirty(win);
            }
        }

        sys_msleep(16);
    }

    sys_kill(child_pid, SIGKILL);
    sys_win_destroy(win);
    return 0;
}
