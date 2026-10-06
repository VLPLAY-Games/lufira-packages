// calc.c — v0.8 (GUI+WM), этап 4: простой калькулятор (целые числа —
// -mgeneral-regs-only в CC_FLAGS, build.py, запрещает FPU-инструкции,
// значит и float/double в userspace-программах этой ОС; "/" — целочисленное
// деление). Классическая модель "аккумулятор + отложенная операция", как у
// простого карманного калькулятора — не полноценный парсер выражений.
#include <stdio.h>
#include <lufira/syscall.h>
#include <lufira/gui_widgets.h>

#define CALC_COLS 4
#define CALC_ROWS 4
#define CALC_BTN_W 56
#define CALC_BTN_H 44
#define CALC_GAP   6
#define CALC_GRID_X 10
#define CALC_GRID_Y 56

typedef struct { const char *label; char code; } calc_key_t;

static const calc_key_t KEYS[CALC_ROWS][CALC_COLS] = {
    {{"7",'7'}, {"8",'8'}, {"9",'9'}, {"/",'/'}},
    {{"4",'4'}, {"5",'5'}, {"6",'6'}, {"*",'*'}},
    {{"1",'1'}, {"2",'2'}, {"3",'3'}, {"-",'-'}},
    {{"0",'0'}, {"C",'C'}, {"=",'='}, {"+",'+'}},
};

static gui_button_t g_btns[CALC_ROWS][CALC_COLS];

static long g_accum = 0;
static long g_current = 0;
static char g_pending_op = 0;
static int g_entering = 0; // печатаем новое число (а не показываем результат)

static long apply_op(long a, char op, long b) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b != 0 ? a / b : 0;
        default:  return b;
    }
}

static void format_long(char *buf, long v) {
    int n = 0;
    if (v < 0) { buf[n++] = '-'; v = -v; }
    char digits[20]; int dn = 0;
    if (v == 0) digits[dn++] = '0';
    while (v > 0) { digits[dn++] = (char)('0' + v % 10); v /= 10; }
    while (dn > 0) buf[n++] = digits[--dn];
    buf[n] = '\0';
}

static void handle_key(char code) {
    if (code >= '0' && code <= '9') {
        if (!g_entering) { g_current = 0; g_entering = 1; }
        // Простая защита от переполнения отображения - дальше 9 цифр не растим.
        if (g_current < 999999999L) g_current = g_current * 10 + (code - '0');
        return;
    }
    if (code == 'C') {
        g_accum = 0; g_current = 0; g_pending_op = 0; g_entering = 0;
        return;
    }
    if (code == '=') {
        if (g_pending_op) {
            g_accum = apply_op(g_accum, g_pending_op, g_current);
            g_pending_op = 0;
        } else {
            g_accum = g_current;
        }
        g_entering = 0;
        return;
    }
    // +, -, *, /
    if (g_pending_op) g_accum = apply_op(g_accum, g_pending_op, g_current);
    else g_accum = g_entering ? g_current : g_accum;
    g_pending_op = code;
    g_entering = 0;
}

static void redraw(int win) {
    sys_win_fill(win, 0x1a1a24);

    char buf[24];
    format_long(buf, g_entering ? g_current : g_accum);
    int display_w = CALC_COLS * CALC_BTN_W + (CALC_COLS - 1) * CALC_GAP;
    sys_win_draw_rect(win, 10, 10, display_w, 36, 0x0d0d12);
    sys_win_draw_text(win, 16, 20, buf, 0x7ef9a4);

    for (int r = 0; r < CALC_ROWS; r++)
        for (int c = 0; c < CALC_COLS; c++)
            gui_button_draw(win, &g_btns[r][c]);
}

int main(void) {
    int win_w = CALC_GRID_X * 2 + CALC_COLS * CALC_BTN_W + (CALC_COLS - 1) * CALC_GAP;
    int win_h = CALC_GRID_Y + CALC_ROWS * CALC_BTN_H + (CALC_ROWS - 1) * CALC_GAP + 10;

    int win = sys_win_create(300, 140, win_w, win_h, "Calc");
    if (win < 0) { printf("sys_win_create FAIL\n"); return 1; }

    for (int r = 0; r < CALC_ROWS; r++) {
        for (int c = 0; c < CALC_COLS; c++) {
            int x = CALC_GRID_X + c * (CALC_BTN_W + CALC_GAP);
            int y = CALC_GRID_Y + r * (CALC_BTN_H + CALC_GAP);
            gui_button_init(&g_btns[r][c], x, y, CALC_BTN_W, CALC_BTN_H, KEYS[r][c].label);
        }
    }

    redraw(win);

    int running = 1;
    while (running) {
        struct lufira_gui_event ev;
        while (sys_win_poll_event(win, &ev)) {
            if (ev.type == LUFIRA_GUI_EVENT_CLOSE) { running = 0; break; }
            if (ev.type == LUFIRA_GUI_EVENT_MOUSE_DOWN) {
                for (int r = 0; r < CALC_ROWS && running; r++) {
                    for (int c = 0; c < CALC_COLS; c++) {
                        if (gui_button_contains(&g_btns[r][c], ev.x, ev.y)) {
                            handle_key(KEYS[r][c].code);
                            redraw(win);
                            break;
                        }
                    }
                }
            }
        }
        sys_msleep(16);
    }

    sys_win_destroy(win);
    return 0;
}
