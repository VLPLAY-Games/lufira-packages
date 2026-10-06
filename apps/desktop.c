// desktop.c — демо-лаунчер v0.8 GUI+WM: окно с кнопками, запускающими
// другие GUI-приложения через fork()+sys_exec() (тот же паттерн, что
// run_child() в shell.c). Предполагает, что /bin/wm.elf уже запущен (см.
// его же комментарий) — иначе sys_win_create() вернёт -1.
#include <stdio.h>
#include <lufira/syscall.h>
#include <lufira/gui_widgets.h>

// НАЙДЕННЫЙ БАГ (живое тестирование: sys_exec() возвращал -EFAULT): argv[]
// обязан жить в static/global памяти, не на стеке — см. подробный
// комментарий у copy_user_string_array() (kernel/system/syscall/
// syscall.c): та проверяет ФИКСИРОВАННЫЙ диапазон (MAX_EXEC_ARGS+1)*8
// байт от переданного указателя (дешевле, чем сперва безопасно узнавать
// настоящую длину), и если короткий argv[] лежит близко к ВЕРХУ 16KB
// пользовательского стека (как тут - launch() вызывается из глубины
// main()'ова цикла обработки событий, кадр уже не у самого низа стека),
// этот фиксированный диапазон высовывается за пределы замапленной
// страницы и is_user_range_valid() честно отказывает - даже при
// абсолютно валидном, коротком argv[]. shell.c уже следует этому
// правилу (его argv - статический g_argv); launch() ниже теперь тоже.
static char *g_launch_argv[2];

static void launch(const char *path) {
    long pid = sys_fork();
    if (pid < 0) return;
    if (pid == 0) {
        g_launch_argv[0] = (char *)path;
        g_launch_argv[1] = 0;
        sys_exec(path, g_launch_argv, (char **)0);
        sys_exit(127); // sys_exec не возвращается при успехе
    }
}

int main(void) {
    // Этап 3: "Text Demo" заменён на "Notepad" (настоящий блокнот с
    // файловым I/O, notepad.c) + добавлен "Files" (файловый менеджер,
    // files.c) — см. план, Фаза 2, пункт 4. Окно расширено под третью
    // кнопку.
    int win = sys_win_create(40, 20, 450, 70, "Desktop");
    if (win < 0) { printf("sys_win_create FAIL (wm.elf запущен?)\n"); return 1; }

    gui_button_t btn_counter, btn_notepad, btn_files;
    gui_button_init(&btn_counter, 20, 20, 130, 32, "Counter Demo");
    gui_button_init(&btn_notepad, 160, 20, 130, 32, "Notepad");
    gui_button_init(&btn_files, 300, 20, 130, 32, "Files");

    sys_win_fill(win, 0x181822);
    gui_button_draw(win, &btn_counter);
    gui_button_draw(win, &btn_notepad);
    gui_button_draw(win, &btn_files);

    for (;;) {
        struct lufira_gui_event ev;
        while (sys_win_poll_event(win, &ev)) {
            if (ev.type == LUFIRA_GUI_EVENT_CLOSE) { sys_win_destroy(win); return 0; }
            if (ev.type == LUFIRA_GUI_EVENT_MOUSE_DOWN) {
                if (gui_button_contains(&btn_counter, ev.x, ev.y)) launch("/bin/counter_demo.elf");
                else if (gui_button_contains(&btn_notepad, ev.x, ev.y)) launch("/bin/notepad.elf");
                else if (gui_button_contains(&btn_files, ev.x, ev.y)) launch("/bin/files.elf");
            }
        }
        sys_msleep(16);
    }
}
