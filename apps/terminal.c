// terminal.c — v0.8 (GUI+WM), этап 4: настоящий терминал в GUI-окне —
// НЕ переимплементация парсера команд, а fork()+exec() настоящего
// /bin/shell.elf с fd 0/1/2, подменёнными (sys_dup2(), новый syscall) на
// концы двух pipe() — классический UNIX-приём "терминал оборачивает
// shell". Сам terminal.c — только эмулятор экрана: держит сетку символов
// TERM_COLS x TERM_ROWS, разбирает ту же простую ESC-последовательность,
// что уже понимает console_write() (kernel/fs/vfs/vfs.c) и эмитит
// userspace/common/console.h (ESC 'f'/'b'/'p'/'c'/'l'/'r' — цвет текста,
// очистка экрана, позиция/движение курсора), и рисует получившуюся сетку
// через sys_win_draw_text()/sys_win_draw_rect().
//
// Фон (ESC 'b') сознательно НЕ моделируется по ячейкам (почти никогда не
// меняется в реальном выводе шелла) — только цвет переднего плана, этого
// достаточно для цветного вывода ls/cpuload/du и т.п.
//
// Ctrl+C НЕ форвардится в pipe: в текстовой консоли он доставляется как
// настоящий SIGINT в обход байтового потока (shell_ctrl_c_pending,
// input.c) — для процесса, читающего из ОБЫЧНОГО pipe, такого пути нет,
// а форвардить сырой байт 0x05 в стандартный ввод шелла было бы просто
// мусором. Известное ограничение этой версии.
#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>

#define TERM_COLS 64
#define TERM_ROWS 24
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

static char g_grid[TERM_ROWS][TERM_COLS];
static uint8_t g_fg[TERM_ROWS][TERM_COLS];
static int g_row_dirty[TERM_ROWS];
static int g_cur_x = 0, g_cur_y = 0;
static int g_cur_fg = 15; // CON_WHITE

static void mark_dirty(int y) { if (y >= 0 && y < TERM_ROWS) g_row_dirty[y] = 1; }

static void term_clear(void) {
    for (int y = 0; y < TERM_ROWS; y++) {
        for (int x = 0; x < TERM_COLS; x++) { g_grid[y][x] = ' '; g_fg[y][x] = (uint8_t)g_cur_fg; }
        mark_dirty(y);
    }
    g_cur_x = 0; g_cur_y = 0;
}

static void term_scroll(void) {
    for (int y = 1; y < TERM_ROWS; y++)
        for (int x = 0; x < TERM_COLS; x++) { g_grid[y - 1][x] = g_grid[y][x]; g_fg[y - 1][x] = g_fg[y][x]; }
    for (int x = 0; x < TERM_COLS; x++) { g_grid[TERM_ROWS - 1][x] = ' '; g_fg[TERM_ROWS - 1][x] = (uint8_t)g_cur_fg; }
    for (int y = 0; y < TERM_ROWS; y++) mark_dirty(y);
}

static void term_newline(void) {
    g_cur_x = 0;
    g_cur_y++;
    if (g_cur_y >= TERM_ROWS) { term_scroll(); g_cur_y = TERM_ROWS - 1; }
}

// Та же семантика, что put_char() в kernel/drivers/console/console.c —
// '\n'/'\r'/'\b'/'\t' разбираются так же (терминал обязан вести себя "как
// шелл", см. шапку файла), просто пишем в СВОЮ сетку, а не в framebuffer.
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
    if (g_cur_x >= TERM_COLS) term_newline();
}

// Разбор ESC-последовательности (см. шапку файла) — состояние парсера
// живёт МЕЖДУ вызовами: байты приходят из pipe кусками произвольного
// размера, последовательность может быть разрезана границей sys_read().
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
            else if (g_esc_cmd == 'r') { if (g_cur_x < TERM_COLS - 1) g_cur_x++; }
            g_esc_state = 0;
        }
    } else {
        g_esc_buf[g_esc_have++] = b;
        if (g_esc_have >= g_esc_need) {
            if (g_esc_cmd == 'f') g_cur_fg = g_esc_buf[0] & 0x0F;
            else if (g_esc_cmd == 'p') {
                int x = ((int)g_esc_buf[0] << 8) | g_esc_buf[1];
                int y = ((int)g_esc_buf[2] << 8) | g_esc_buf[3];
                if (x >= 0 && x < TERM_COLS) g_cur_x = x;
                if (y >= 0 && y < TERM_ROWS) g_cur_y = y;
            }
            // 'b' (фон) — см. шапку файла, сознательно игнорируется.
            g_esc_state = 0;
        }
    }

    mark_dirty(old_y);
    mark_dirty(g_cur_y);
}

static void term_redraw_dirty(int win) {
    for (int y = 0; y < TERM_ROWS; y++) {
        if (!g_row_dirty[y]) continue;
        g_row_dirty[y] = 0;

        int py = MARGIN + y * CHAR_H;
        sys_win_draw_rect(win, MARGIN, py, TERM_COLS * CHAR_W, CHAR_H, 0x000000);

        int x = 0;
        while (x < TERM_COLS) {
            uint8_t color = g_fg[y][x];
            int start = x;
            char buf[TERM_COLS + 1]; int n = 0;
            int has_visible = 0;
            while (x < TERM_COLS && g_fg[y][x] == color) {
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

    // Родитель: пишет в in_pipe[1] (клавиатура ребёнка), читает из
    // out_pipe[0] (вывод ребёнка) — лишние концы закрываем сразу, иначе
    // EOF на стороне ребёнка никогда не увидим (reader/writer-счётчики
    // pipe_t, vfs.c, не дойдут до нуля, пока хоть кто-то держит открытым
    // лишний конец).
    sys_close(in_pipe[0]);
    sys_close(out_pipe[1]);

    term_clear();

    int win_w = MARGIN * 2 + TERM_COLS * CHAR_W;
    int win_h = MARGIN * 2 + TERM_ROWS * CHAR_H;
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
