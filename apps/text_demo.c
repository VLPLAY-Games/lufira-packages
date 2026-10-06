// text_demo.c — демо-приложение v0.8 GUI+WM: однострочное текстовое поле,
// печать и backspace через события клавиатуры.
#include <stdio.h>
#include <lufira/syscall.h>
#include <lufira/gui_widgets.h>

static void redraw(int win, gui_textbox_t *tb) {
    sys_win_fill(win, 0x202030);
    sys_win_draw_text(win, 20, 20, "Type something:", 0xffffff);
    gui_textbox_draw(win, tb);
}

int main(void) {
    int win = sys_win_create(220, 180, 260, 120, "Text Demo");
    if (win < 0) { printf("sys_win_create FAIL\n"); return 1; }

    gui_textbox_t tb;
    gui_textbox_init(&tb, 20, 50, 220, 28);
    tb.focused = 1; // единственный виджет - фокус всегда на нём

    redraw(win, &tb);

    int running = 1;
    while (running) {
        struct lufira_gui_event ev;
        while (sys_win_poll_event(win, &ev)) {
            if (ev.type == LUFIRA_GUI_EVENT_CLOSE) { running = 0; break; }
            if (ev.type == LUFIRA_GUI_EVENT_KEY) {
                if (gui_textbox_handle_key(&tb, ev.key_or_button)) {
                    redraw(win, &tb);
                }
            }
        }
        sys_msleep(16);
    }

    sys_win_destroy(win);
    return 0;
}
