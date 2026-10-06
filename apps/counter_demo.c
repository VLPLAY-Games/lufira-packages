// counter_demo.c — демо-приложение v0.8 GUI+WM: кнопка "+1" увеличивает
// счётчик, перерисовывается каждый раз.
#include <stdio.h>
#include <stdlib.h>
#include <lufira/syscall.h>
#include <lufira/gui_widgets.h>

static void redraw(int win, int count, gui_button_t *btn) {
    sys_win_fill(win, 0x202030);
    char buf[32];
    int n = 0;
    buf[n++] = 'C'; buf[n++] = 'o'; buf[n++] = 'u'; buf[n++] = 'n'; buf[n++] = 't';
    buf[n++] = ':'; buf[n++] = ' ';
    if (count == 0) { buf[n++] = '0'; }
    else {
        char digits[12]; int dn = 0; int c = count;
        while (c > 0) { digits[dn++] = (char)('0' + c % 10); c /= 10; }
        while (dn > 0) buf[n++] = digits[--dn];
    }
    buf[n] = '\0';
    sys_win_draw_text(win, 20, 20, buf, 0xffffff);
    gui_button_draw(win, btn);
}

int main(void) {
    int win = sys_win_create(200, 150, 220, 140, "Counter Demo");
    if (win < 0) { printf("sys_win_create FAIL\n"); return 1; }

    gui_button_t btn;
    gui_button_init(&btn, 20, 60, 100, 32, "+1");

    int count = 0;
    redraw(win, count, &btn);

    int running = 1;
    while (running) {
        struct lufira_gui_event ev;
        while (sys_win_poll_event(win, &ev)) {
            if (ev.type == LUFIRA_GUI_EVENT_CLOSE) { running = 0; break; }
            if (ev.type == LUFIRA_GUI_EVENT_MOUSE_DOWN) {
                if (gui_button_contains(&btn, ev.x, ev.y)) {
                    count++;
                    redraw(win, count, &btn);
                }
            }
        }
        sys_msleep(16);
    }

    sys_win_destroy(win);
    return 0;
}
