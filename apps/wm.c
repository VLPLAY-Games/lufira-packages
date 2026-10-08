// wm.c — v0.8 этап 3: оконный сервер как обычный userspace-процесс (раньше
// жил в kernel/system/gui/gui.c, перенесён почти 1:1: таблица окон/z-order/
// drag/фокус/композитинг). Отличия: буферы пикселей — malloc(), не kmalloc();
// клиент шлёт RPC через SYS_IPC_SEND/RECV (протокол в <lufira/wm_protocol.h>,
// зеркало kernel/system/ipc/wm_protocol.h); ядро толкает события клавиатуры/
// мыши в mailbox (sender_pid==0 = WM_SENDER_KERNEL) вместо опроса
// input_mouse_get_*() на PIT-тике; convert_color()/шрифт реплицированы
// локально (недостижимы отсюда); кадр идёт через SYS_FB_PRESENT.
//
// Без иконок (по прямому указанию пользователя) — только примитивы.
//
// Этап 4: рабочий стол (фон+ярлыки+таскбар) рисуется постоянно с момента
// sys_wm_register(), заменяя удалённый GUI-клиент desktop.c — WM сам
// рисует ярлыки и форкает/exec'ает приложение по клику (launch_app()).
// Таскбар получил постоянную кнопку "Exit" — иначе нет пути к консоли.

#include <lufira/syscall.h>
#include <lufira/wm_protocol.h>
#include <stdlib.h>
#include <string.h>

#define WM_MAX_WINDOWS      16
#define WM_TITLE_MAX        64
#define WM_TITLEBAR_HEIGHT  20
#define WM_BORDER           2
#define WM_CLOSE_BTN_SIZE   16
#define WM_CHAR_W           8
#define WM_CHAR_H           8
#define WM_EVENT_QUEUE_SIZE 32

// Запрос пользователя: minimize/maximize/close в титлбаре (порядок как в
// Windows) + ресайз за край/угол окна. Кнопки квадратные, WM_CLOSE_BTN_SIZE
// (имя историческое — теперь размер любой кнопки титлбара), с зазором.
#define WM_TITLEBAR_BTN_GAP 2
// Ширина хватательной зоны у нижнего/правого края окна (клик там запускает
// ресайз вместо mouse-down клиенту) — узкая, чтобы не мешать кликам по
// содержимому у самого края, но достаточно широкая, чтобы в неё попадать.
#define WM_RESIZE_GRIP      8
// Нижний предел размера клиентской области — защищает арифметику
// компоновщика от 0x0 и не даёт окну стать меньше титлбара с кнопками.
#define WM_MIN_WIN_W        120
#define WM_MIN_WIN_H        80

#define DESKTOP_BG        0x2b2b3a
#define TITLEBAR_ACTIVE   0x3a6ea5
#define TITLEBAR_INACTIVE 0x4a4a55
#define TITLEBAR_BTN_BG   0x3a3a52
#define BORDER_COLOR      0x1a1a22
#define TITLE_TEXT_COLOR  0xffffff
#define CLOSE_BTN_COLOR   0xcc4444
#define RESIZE_GRIP_COLOR 0x8888aa

// Таскбар — полоса снизу экрана, кнопка на каждое окно; не отдельное окно,
// а ещё один слой компоновки в g_fb (как рабочий стол/курсор).
#define TASKBAR_HEIGHT    28
#define TASKBAR_BTN_W     160
#define TASKBAR_BG        0x1f1f29
#define TASKBAR_BTN_BG    0x33334a
#define TASKBAR_BTN_ACTIVE 0x3a6ea5
#define TASKBAR_TEXT      0xffffff

// Текстовый конфиг "title=x,y,w,h" построчно, ключ — заголовок окна.
// w/h читаются и пишутся, но при создании окна применяется только x/y:
// подставить сохранённый w/h дал бы клиенту буфер другого размера, чем он
// запросил и под который рассчитал раскладку своих виджетов — а
// sys_win_create() возвращает только id, клиент не узнал бы о подмене.
#define WM_STATE_PATH        "/etc/wm_state.conf"
#define WM_STATE_MAX_ENTRIES 32
#define WM_STATE_LINE_MAX    160

// Ярлыки запуска заменяют удалённый desktop.c: WM сам рисует их в фоновом
// слое рабочего стола (под окнами, над фоном — см. composite_scene()) и
// форкает/exec'ает приложение по клику. Без иконок — подписанные
// прямоугольники, стиль кнопок таскбара.
typedef struct {
    const char *label;
    const char *path;
} desktop_launcher_t;

static const desktop_launcher_t g_launchers[] = {
    {"Terminal", "/bin/terminal.elf"},
    {"Notepad",  "/bin/notepad.elf"},
    {"Files",    "/bin/files.elf"},
    {"Calc",     "/bin/calc.elf"},
    {"Sys Info", "/bin/sysinfo.elf"},
};
#define NUM_LAUNCHERS ((int)(sizeof(g_launchers) / sizeof(g_launchers[0])))

#define LAUNCHER_ICON_W  90
#define LAUNCHER_ICON_H  36
#define LAUNCHER_GAP     8
#define LAUNCHER_START_X 16
#define LAUNCHER_START_Y 16
#define LAUNCHER_BG      0x33334a
#define LAUNCHER_BORDER  0x1a1a22
#define LAUNCHER_TEXT    0xffffff

// Кнопка "Пуск" (запрос: "слева снизу, 3 пункта — exit gui/shutdown/
// reboot, старый exit справа убрать") — левый край таскбара, открывает
// меню из трёх пунктов над собой; заменяет удалённую кнопку "Exit".
#define START_BTN_W       70
#define START_MENU_ITEM_H 26
#define START_MENU_W      150
#define START_MENU_BG     0x2a2a3c

typedef struct {
    const char *label;
} start_menu_item_t;

// Порядок как попросил пользователь (exit gui/shutdown/reboot) —
// do_start_menu_action() индексирует этот массив по номеру пункта.
static const start_menu_item_t g_start_menu_items[] = {
    {"Exit GUI"},
    {"Shutdown"},
    {"Reboot"},
};
#define NUM_START_MENU_ITEMS ((int)(sizeof(g_start_menu_items) / sizeof(g_start_menu_items[0])))

// Рисуется поверх всего (окон, таскбара), как и курсор.
static int g_start_menu_open = 0;

typedef struct {
    int head, tail, count;
    struct lufira_gui_event events[WM_EVENT_QUEUE_SIZE];
} wm_event_queue_t;

typedef struct {
    int in_use;
    uint32_t owner_pid;
    int x, y;             // верхний левый угол ВСЕГО окна (рамка+титлбар)
    int w, h;              // размер КЛИЕНТСКОЙ области
    char title[WM_TITLE_MAX];
    uint32_t *pixels;      // malloc'd, w*h, уже wm_convert_color()'нутые пиксели
    wm_event_queue_t events;
    int minimized;          // скрыто с рабочего стола, но кнопка в таскбаре осталась
    int maximized;           // растянуто на весь рабочий стол (до таскбара)
    int restore_x, restore_y, restore_w, restore_h; // геометрия ДО maximize — для restore; валидны только пока maximized
    uint32_t bg_color;      // последний цвет ПОЛНОЙ заливки (sys_win_fill) — см. комментарий у resize_window_buffer()
} wm_window_t;

static wm_window_t g_windows[WM_MAX_WINDOWS];
static int g_order[WM_MAX_WINDOWS];
static int g_window_count = 0;
static int g_focused = -1;
static int g_dragging = -1;
static int g_drag_off_x = 0, g_drag_off_y = 0;

// Ресайз за край/угол — как g_dragging (один активный, индекс или -1), но
// размер считается от зафиксированного на mouse-down w/h + дельта от
// стартовой позиции мыши, не накопительно — иначе дрейфовал бы от потерь
// событий.
static int g_resizing = -1;
static int g_resize_edge_right = 0, g_resize_edge_bottom = 0;
static int g_resize_start_mx = 0, g_resize_start_my = 0;
static int g_resize_start_w = 0, g_resize_start_h = 0;

// Было: один флаг g_dirty, любое изменение (даже чистое движение курсора)
// гоняло полную composite_and_present() — баг "CPU под 100% при движении
// мыши/печати". Теперь два флага: g_content_dirty (сцена реально
// изменилась) запускает дорогую composite_scene(); g_cursor_moved (только
// мышь) — дешёвый present_frame() (memcpy сцены + силуэт курсора).
static int g_content_dirty = 1;
static int g_cursor_moved = 0;
static int g_mouse_x = 0, g_mouse_y = 0, g_prev_buttons = 0;
// Где курсор рисовался в прошлом present_frame() — на чисто-курсорном
// кадре нужно знать, какую область g_present_fb стереть/восстановить из
// g_fb. -1000: далеко за экраном, безопасно (первый кадр всегда полный).
static int g_last_cursor_x = -1000, g_last_cursor_y = -1000;

static uint32_t g_screen_w = 0, g_screen_h = 0, g_pixel_format = 0;
static uint32_t *g_fb = NULL;         // сцена БЕЗ курсора, malloc'd w*h — актуальна только после composite_scene()
static uint32_t *g_present_fb = NULL; // g_fb + курсор поверх — это и уходит в SYS_FB_PRESENT
static uint32_t *g_draw_target = NULL; // куда сейчас пишут fb_*() ниже — g_fb во время composite_scene(), g_present_fb во время наложения курсора
static uint8_t *g_font = NULL;     // байты глифов (SYS_FB_FONT), 8 байт/глиф
static int g_font_glyphs = 0;      // сколько глифов реально получили (char = 32 + индекс)

typedef struct {
    char title[WM_TITLE_MAX];
    int x, y, w, h;
} wm_saved_pos_t;

static wm_saved_pos_t g_saved_pos[WM_STATE_MAX_ENTRIES];
static int g_saved_pos_count = 0;

// Та же формула, что convert_color() в kernel/drivers/console/console.c
// (недостижима отсюда напрямую — см. SYS_FB_INFO в syscall.h).
static uint32_t wm_convert_color(uint32_t color) {
    if (g_pixel_format == 1) return color;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

// ===== Персистентность позиции/размера окна (/etc/wm_state.conf) =====
// Нет fopen()/atoi()/snprintf() в этой libc — читаем/пишем через сырые
// sys_open/read/write/lseek (как base/cat.c, write.c), числа парсим вручную.

static const char *parse_int(const char *s, int *out) {
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    int v = 0, any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
    if (!any) return NULL;
    *out = neg ? -v : v;
    return s;
}

static int append_int(char *buf, int pos, int value) {
    if (value < 0) { buf[pos++] = '-'; value = -value; }
    char digits[12]; int dn = 0;
    if (value == 0) digits[dn++] = '0';
    while (value > 0) { digits[dn++] = (char)('0' + value % 10); value /= 10; }
    while (dn > 0) buf[pos++] = digits[--dn];
    return pos;
}

static void load_wm_state(void) {
    long fd = sys_open(WM_STATE_PATH, O_RDONLY, 0);
    if (fd < 0) return; // первый запуск / файла ещё нет - не ошибка

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

    char *line = buf;
    while (*line && g_saved_pos_count < WM_STATE_MAX_ENTRIES) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *eq = strchr(line, '=');
        if (eq && eq != line) {
            *eq = '\0';
            int x, y, w, h;
            const char *p = eq + 1;
            p = parse_int(p, &x);
            if (p && *p == ',') { p++; p = parse_int(p, &y); } else p = NULL;
            if (p && *p == ',') { p++; p = parse_int(p, &w); } else p = NULL;
            if (p && *p == ',') { p++; p = parse_int(p, &h); } else p = NULL;
            if (p) {
                wm_saved_pos_t *e = &g_saved_pos[g_saved_pos_count++];
                int n = 0;
                while (line[n] && n < WM_TITLE_MAX - 1) { e->title[n] = line[n]; n++; }
                e->title[n] = '\0';
                e->x = x; e->y = y; e->w = w; e->h = h;
            }
        }

        if (!nl) break;
        line = nl + 1;
    }
    free(buf);
}

static void save_wm_state(void) {
    long fd = sys_open(WM_STATE_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0);
    if (fd < 0) return;

    char line[WM_STATE_LINE_MAX];
    for (int i = 0; i < g_saved_pos_count; i++) {
        wm_saved_pos_t *e = &g_saved_pos[i];
        int p = 0;
        int tn = (int)strlen(e->title);
        if (tn > WM_STATE_LINE_MAX - 48) tn = WM_STATE_LINE_MAX - 48; // запас под 4 числа+разделители
        memcpy(line + p, e->title, (size_t)tn); p += tn;
        line[p++] = '=';
        p = append_int(line, p, e->x); line[p++] = ',';
        p = append_int(line, p, e->y); line[p++] = ',';
        p = append_int(line, p, e->w); line[p++] = ',';
        p = append_int(line, p, e->h);
        line[p++] = '\n';
        sys_write((int)fd, line, (unsigned long)p);
    }
    sys_close((int)fd);
}

static int find_saved_pos(const char *title, int *x, int *y) {
    for (int i = 0; i < g_saved_pos_count; i++) {
        if (strcmp(g_saved_pos[i].title, title) == 0) {
            *x = g_saved_pos[i].x; *y = g_saved_pos[i].y;
            return 1;
        }
    }
    return 0;
}

static void update_saved_pos(const char *title, int x, int y, int w, int h) {
    if (!title[0]) return; // безымянные окна не персистим - нет ключа
    for (int i = 0; i < g_saved_pos_count; i++) {
        if (strcmp(g_saved_pos[i].title, title) == 0) {
            g_saved_pos[i].x = x; g_saved_pos[i].y = y;
            g_saved_pos[i].w = w; g_saved_pos[i].h = h;
            save_wm_state();
            return;
        }
    }
    if (g_saved_pos_count < WM_STATE_MAX_ENTRIES) {
        wm_saved_pos_t *e = &g_saved_pos[g_saved_pos_count++];
        int n = 0;
        while (title[n] && n < WM_TITLE_MAX - 1) { e->title[n] = title[n]; n++; }
        e->title[n] = '\0';
        e->x = x; e->y = y; e->w = w; e->h = h;
        save_wm_state();
    }
}

// ===== Компоновка в g_fb (экранные координаты) =====

static void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > (int)g_screen_w) x1 = (int)g_screen_w;
    int y1 = y + h; if (y1 > (int)g_screen_h) y1 = (int)g_screen_h;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = g_draw_target + (uint32_t)py * g_screen_w + (uint32_t)x0;
        for (int px = x0; px < x1; px++) *row++ = color;
    }
}

static void fb_set(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || (uint32_t)x >= g_screen_w || (uint32_t)y >= g_screen_h) return;
    g_draw_target[(uint32_t)y * g_screen_w + (uint32_t)x] = color;
}

// Глиф в g_fb (экранные координаты), с фоном — для титлбара/кнопки [X].
static void fb_draw_glyph(int x, int y, int c, uint32_t fg, uint32_t bg) {
    if (c < 32 || c - 32 >= g_font_glyphs) c = '?';
    if (c - 32 >= g_font_glyphs) return; // даже '?' недостижим (почти пустой шрифт) — не рисуем вовсе
    const uint8_t *glyph = &g_font[(c - 32) * 8];
    for (int cy = 0; cy < 8; cy++) {
        int py = y + cy;
        if (py < 0 || (uint32_t)py >= g_screen_h) continue;
        for (int cx = 0; cx < 8; cx++) {
            int px = x + cx;
            if (px < 0 || (uint32_t)px >= g_screen_w) continue;
            fb_set(px, py, ((glyph[cy] >> (7 - cx)) & 1) ? fg : bg);
        }
    }
}

// Блит содержимого окна (уже конвертированные пиксели) в g_fb.
static void fb_blit(int dst_x, int dst_y, const uint32_t *src, int sw, int sh) {
    int x0 = dst_x < 0 ? 0 : dst_x;
    int y0 = dst_y < 0 ? 0 : dst_y;
    int x1 = dst_x + sw; if (x1 > (int)g_screen_w) x1 = (int)g_screen_w;
    int y1 = dst_y + sh; if (y1 > (int)g_screen_h) y1 = (int)g_screen_h;
    if (x0 >= x1 || y0 >= y1) return;
    int copy_w = x1 - x0;
    int skip_x = x0 - dst_x, skip_y = y0 - dst_y;
    for (int py = y0; py < y1; py++) {
        const uint32_t *srow = src + (size_t)(py - y0 + skip_y) * sw + skip_x;
        uint32_t *drow = g_draw_target + (size_t)py * g_screen_w + x0;
        memcpy(drow, srow, (size_t)copy_w * sizeof(uint32_t));
    }
}

// Глиф в ПРОИЗВОЛЬНЫЙ буфер окна (клиентские координаты) — только
// foreground, как и font_draw_glyph_to_buffer() в console.c (фон уже
// нарисован отдельным sys_win_fill()).
static void win_draw_glyph(uint32_t *buf, int buf_w, int buf_h, int x, int y, int c, uint32_t fg) {
    if (c < 32 || c - 32 >= g_font_glyphs) return;
    const uint8_t *glyph = &g_font[(c - 32) * 8];
    for (int cy = 0; cy < 8; cy++) {
        int py = y + cy;
        if (py < 0 || py >= buf_h) continue;
        for (int cx = 0; cx < 8; cx++) {
            int px = x + cx;
            if (px < 0 || px >= buf_w) continue;
            if ((glyph[cy] >> (7 - cx)) & 1) buf[py * buf_w + px] = fg;
        }
    }
}

// ===== Ярлыки запуска на рабочем столе =====

// argv[] должен жить в static/global памяти, не на стеке: стековый массив
// рядом с верхом 16KB пользовательского стека не проходит проверку
// диапазона в copy_user_string_array() (kernel/system/syscall/syscall.c).
static char *g_launch_argv[2];

// Временная диагностика (баг: второй запуск из ярлыка не открывался) —
// лог в файл отдельно от GUI/IPC. TODO снять.
static void dbg_log(const char *msg) {
    long fd = sys_open("/wm2_dbg.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    sys_write((int)fd, msg, (unsigned long)strlen(msg));
    sys_close((int)fd);
}

static void dbg_log_num(const char *prefix, long v) {
    char line[64]; int n = 0;
    while (*prefix) line[n++] = *prefix++;
    char tmp[24]; int tn = 0;
    int neg = v < 0; unsigned long uv = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (uv == 0) tmp[tn++] = '0';
    while (uv > 0) { tmp[tn++] = (char)('0' + (uv % 10)); uv /= 10; }
    if (neg) line[n++] = '-';
    while (tn > 0) line[n++] = tmp[--tn];
    line[n++] = '\n';
    line[n] = '\0';
    dbg_log(line);
}

static void launch_app(const char *path) {
    dbg_log("launch_app: "); dbg_log(path); dbg_log("\n");
    long pid = sys_fork();
    dbg_log_num("launch_app: fork()=", pid);
    if (pid < 0) return;
    if (pid == 0) {
        // fork() дублирует весь образ WM — ничего из этого ребёнку не нужно,
        // он сразу exec'ает целевое приложение (тот же паттерн, что в shell.c).
        g_launch_argv[0] = (char *)path;
        g_launch_argv[1] = 0;
        long er = sys_exec(path, g_launch_argv, (char **)0);
        dbg_log_num("launch_app: CHILD exec FAILED er=", er);
        sys_exit(127); // sys_exec не возвращается при успехе
    }
}

static int launcher_rect(int i, int *x, int *y) {
    if (i < 0 || i >= NUM_LAUNCHERS) return 0;
    *x = LAUNCHER_START_X;
    *y = LAUNCHER_START_Y + i * (LAUNCHER_ICON_H + LAUNCHER_GAP);
    return 1;
}

// Клик по рабочему столу мимо всех окон — проверяем ярлыки; возвращает 1
// (и уже запускает приложение), если попали, иначе 0.
static int try_launch_desktop_icon(int mx, int my) {
    for (int i = 0; i < NUM_LAUNCHERS; i++) {
        int x, y;
        launcher_rect(i, &x, &y);
        if (mx >= x && mx < x + LAUNCHER_ICON_W && my >= y && my < y + LAUNCHER_ICON_H) {
            launch_app(g_launchers[i].path);
            return 1;
        }
    }
    return 0;
}

// Кнопка "Пуск" — зафиксирована в левом краю таскбара (всегда видна,
// независимо от числа открытых окон) — см. комментарий у START_BTN_W.
static void start_button_rect(int *x, int *y, int *w, int *h) {
    *w = START_BTN_W - 8;
    *h = TASKBAR_HEIGHT - 4;
    *x = 4;
    *y = (int)g_screen_h - TASKBAR_HEIGHT + 2;
}

// Forward declaration: taskbar_top() определена ниже, start_menu_item_rect()
// использует её раньше в файле (вызываются обе уже после определения).
static int taskbar_top(void);

// Геометрия пункта меню #i (0-based, сверху вниз) — меню растёт ВВЕРХ от
// кнопки "Пуск" (таскбар внизу экрана, под ним рисовать уже негде).
static void start_menu_item_rect(int i, int *x, int *y, int *w, int *h) {
    *w = START_MENU_W;
    *h = START_MENU_ITEM_H;
    *x = 0;
    *y = taskbar_top() - (NUM_START_MENU_ITEMS - i) * START_MENU_ITEM_H;
}

// "Exit GUI" зовёт sys_exit() WM — ядро восстанавливает текстовую консоль
// когда завершается именно зарегистрированный WM pid (process_exit(),
// kernel/system/process/process.c). Shutdown/Reboot — те же привилегированные
// syscall'ы, что у shutdown.elf/reboot.elf (SYS_SHUTDOWN/SYS_REBOOT).
static void do_start_menu_action(int item_index) {
    switch (item_index) {
        case 0: sys_exit(0); break;   // Exit GUI
        case 1: sys_shutdown(); break;
        case 2: sys_reboot(); break;
        default: break;
    }
}

// ===== Таблица окон / z-order (тот же приём, что был в gui.c) =====

static int outer_w(const wm_window_t *w) { return w->w + 2 * WM_BORDER; }
static int outer_h(const wm_window_t *w) { return w->h + WM_TITLEBAR_HEIGHT + WM_BORDER; }

// Реаллоцирует буфер окна под новый размер, копируя пересекающуюся
// область (верх-левый угол); restore после maximize() даёт точно прежний
// вид без отдельного снапшота.
//
// Было: новая площадь всегда чёрная (баг: "в обычных системах фон просто
// расширяется"). Теперь заливается bg_color (последний цвет sys_win_fill())
// — не идеально (приложение сам не перерисует её до следующего redraw, см.
// LUFIRA_GUI_EVENT_RESIZE в syscall.h), но убирает "чёрную дыру" в типичном случае.
static void resize_window_buffer(wm_window_t *w, int new_w, int new_h) {
    if (new_w < 1) new_w = 1;
    if (new_h < 1) new_h = 1;
    if (new_w == w->w && new_h == w->h) return;

    uint32_t *new_pixels = (uint32_t *)malloc((size_t)new_w * (size_t)new_h * sizeof(uint32_t));
    if (!new_pixels) return; // ОЗУ кончилась — оставляем старый размер как есть
    for (size_t i = 0, n = (size_t)new_w * (size_t)new_h; i < n; i++) new_pixels[i] = w->bg_color;

    int copy_w = new_w < w->w ? new_w : w->w;
    int copy_h = new_h < w->h ? new_h : w->h;
    for (int row = 0; row < copy_h; row++) {
        memcpy(new_pixels + (size_t)row * new_w,
               w->pixels + (size_t)row * w->w,
               (size_t)copy_w * sizeof(uint32_t));
    }

    free(w->pixels);
    w->pixels = new_pixels;
    w->w = new_w;
    w->h = new_h;
}

// Правая/нижняя WM_RESIZE_GRIP-полоса внешней рамки (вызывающий уже
// проверил mx/my внутри через find_window_at()). Угол = оба края разом
// (ширина+высота меняются вместе). Недоступно для maximized-окна.
static int in_resize_zone(const wm_window_t *w, int mx, int my, int *edge_right, int *edge_bottom) {
    if (w->maximized) return 0;
    int ow = outer_w(w), oh = outer_h(w);
    int right = (mx >= w->x + ow - WM_RESIZE_GRIP);
    int bottom = (my >= w->y + oh - WM_RESIZE_GRIP);
    if (!right && !bottom) return 0;
    *edge_right = right;
    *edge_bottom = bottom;
    return 1;
}

// Геометрия трёх кнопок титлбара (min/max/close, порядок как в Windows) —
// одна точка правды для рисования и хит-теста, чтобы не разъехались.
static void titlebar_button_rects(const wm_window_t *w, int *min_x, int *max_x, int *close_x,
                                   int *btn_y, int *btn_size) {
    *btn_size = WM_CLOSE_BTN_SIZE;
    *btn_y = w->y + (WM_TITLEBAR_HEIGHT - WM_CLOSE_BTN_SIZE) / 2;
    *close_x = w->x + outer_w(w) - WM_BORDER - WM_CLOSE_BTN_SIZE - 2;
    *max_x = *close_x - WM_CLOSE_BTN_SIZE - WM_TITLEBAR_BTN_GAP;
    *min_x = *max_x - WM_CLOSE_BTN_SIZE - WM_TITLEBAR_BTN_GAP;
}

// Простые векторные иконки (как у курсора — fb_fill_rect() по экранным
// координатам): подчёркивание (minimize), квадрат-контур (maximize), два
// перекрывающихся контура (restore, когда maximized уже 1).
static void draw_titlebar_icon_minimize(int bx, int by, int bs, uint32_t color) {
    int lw = 8;
    int lx = bx + (bs - lw) / 2;
    int ly = by + bs - 5;
    fb_fill_rect(lx, ly, lw, 2, color);
}

static void draw_square_outline(int sx, int sy, int sz, uint32_t color) {
    fb_fill_rect(sx, sy, sz, 2, color);
    fb_fill_rect(sx, sy + sz - 2, sz, 2, color);
    fb_fill_rect(sx, sy, 2, sz, color);
    fb_fill_rect(sx + sz - 2, sy, 2, sz, color);
}

static void draw_titlebar_icon_maxrestore(int bx, int by, int bs, uint32_t color, int maximized) {
    if (!maximized) {
        int sz = 9;
        draw_square_outline(bx + (bs - sz) / 2, by + (bs - sz) / 2, sz, color);
    } else {
        int sz = 7, off = 3;
        int sx = bx + (bs - sz) / 2 - off / 2;
        int sy = by + (bs - sz) / 2 - off / 2;
        draw_square_outline(sx + off, sy, sz, color);
        draw_square_outline(sx, sy + off, sz, color);
    }
}

// Диагональная "ручка" в углу рамки — подсказка про ресайз (зона сама
// невидима, WM_RESIZE_GRIP). Скрыта для maximized (там ресайза нет).
static void draw_resize_grip(const wm_window_t *w) {
    if (w->maximized) return;
    uint32_t c = wm_convert_color(RESIZE_GRIP_COLOR);
    int bx = w->x + outer_w(w) - 2;
    int by = w->y + outer_h(w) - 2;
    for (int d = 2; d <= 8; d += 3) {
        fb_set(bx - d, by, c);
        fb_set(bx, by - d, c);
    }
}

static int find_free_slot(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) if (!g_windows[i].in_use) return i;
    return -1;
}

static wm_window_t *get_owned(uint32_t owner_pid, int window_id) {
    if (window_id < 0 || window_id >= WM_MAX_WINDOWS) return NULL;
    wm_window_t *w = &g_windows[window_id];
    if (!w->in_use || w->owner_pid != owner_pid) return NULL;
    return w;
}

static int find_window_at(int px, int py) {
    for (int i = g_window_count - 1; i >= 0; i--) {
        int idx = g_order[i];
        wm_window_t *w = &g_windows[idx];
        if (!w->in_use || w->minimized) continue; // свёрнутое окно не кликабельно на рабочем столе — только через таскбар
        if (px >= w->x && px < w->x + outer_w(w) &&
            py >= w->y && py < w->y + outer_h(w))
            return idx;
    }
    return -1;
}

static void raise_to_top(int idx) {
    int pos = -1;
    for (int i = 0; i < g_window_count; i++) if (g_order[i] == idx) { pos = i; break; }
    if (pos < 0 || pos == g_window_count - 1) return;
    for (int i = pos; i < g_window_count - 1; i++) g_order[i] = g_order[i + 1];
    g_order[g_window_count - 1] = idx;
}

static void set_focus(int idx) {
    if (g_focused == idx) return;
    g_focused = idx;
    g_content_dirty = 1;
}

static void queue_push(wm_window_t *w, int type, int x, int y, int key_or_button) {
    wm_event_queue_t *q = &w->events;
    if (q->count >= WM_EVENT_QUEUE_SIZE) return; // переполнение — молча роняем
    q->events[q->tail].type = type;
    q->events[q->tail].x = x;
    q->events[q->tail].y = y;
    q->events[q->tail].key_or_button = key_or_button;
    q->tail = (q->tail + 1) % WM_EVENT_QUEUE_SIZE;
    q->count++;
}

static int taskbar_top(void) { return (int)g_screen_h - TASKBAR_HEIGHT; }

// Maximize/restore сохраняет/восстанавливает геометрию в самом окне
// (restore_*), без файла — та же идея, что update_saved_pos() на диске.
// Клиент получает LUFIRA_GUI_EVENT_RESIZE (необязателен к обработке, см.
// syscall.h); итоговая геометрия персистится через update_saved_pos().
static void toggle_maximize(wm_window_t *w) {
    if (!w->maximized) {
        w->restore_x = w->x; w->restore_y = w->y;
        w->restore_w = w->w; w->restore_h = w->h;
        w->maximized = 1;
        w->x = 0; w->y = 0;
        int new_w = (int)g_screen_w - 2 * WM_BORDER;
        int new_h = taskbar_top() - WM_TITLEBAR_HEIGHT - WM_BORDER;
        if (new_w < WM_MIN_WIN_W) new_w = WM_MIN_WIN_W;
        if (new_h < WM_MIN_WIN_H) new_h = WM_MIN_WIN_H;
        resize_window_buffer(w, new_w, new_h);
    } else {
        w->maximized = 0;
        w->x = w->restore_x; w->y = w->restore_y;
        resize_window_buffer(w, w->restore_w, w->restore_h);
    }
    queue_push(w, LUFIRA_GUI_EVENT_RESIZE, w->w, w->h, 0);
    update_saved_pos(w->title, w->x, w->y, w->w, w->h);
    g_content_dirty = 1;
}

// Сворачивание не трогает геометрию — окно просто не рисуется/не
// кликается на столе (см. composite_scene()/find_window_at()), но
// остаётся в g_order с кнопкой в таскбаре (единственный путь развернуть
// обратно) — как в Windows.
static void minimize_window(int idx) {
    wm_window_t *w = &g_windows[idx];
    w->minimized = 1;
    if (g_focused == idx) g_focused = -1;
    if (g_dragging == idx) g_dragging = -1;
    if (g_resizing == idx) g_resizing = -1;
    g_content_dirty = 1;
}

static void handle_mouse_transition(int mx, int my, int buttons) {
    int going_down = (buttons & 1) && !(g_prev_buttons & 1);
    int going_up = !(buttons & 1) && (g_prev_buttons & 1);

    // Меню "Пуск" плавает над всем, проверяется первым, пока открыто. Клик
    // по пункту — выполняет действие и закрывает меню; клик куда угодно
    // ещё тоже закрывает его, но затем разбирается обычным путём ниже
    // (клик по окну под меню и закрывает меню, и фокусирует окно — без
    // двойного клика).
    if (going_down && g_start_menu_open) {
        // Клик по самой кнопке "Пуск" пропускаем сюда нетронутым — его
        // toggle делает блок таскбара ниже; сбросив здесь флаг, мы бы его
        // сразу включили обратно.
        int sx, sy, sw, sh;
        start_button_rect(&sx, &sy, &sw, &sh);
        int on_start_btn = (mx >= sx && mx < sx + sw && my >= sy && my < sy + sh);

        if (!on_start_btn) {
            int handled = 0;
            for (int i = 0; i < NUM_START_MENU_ITEMS; i++) {
                int ix, iy, iw, ih;
                start_menu_item_rect(i, &ix, &iy, &iw, &ih);
                if (mx >= ix && mx < ix + iw && my >= iy && my < iy + ih) {
                    do_start_menu_action(i);
                    handled = 1;
                    break;
                }
            }
            g_start_menu_open = 0;
            g_content_dirty = 1;
            if (handled) return;
        }
    }

    // Таскбар — слой над окнами, его клики проверяются первыми (не идут к
    // window-hit-тесту ниже). Виден постоянно — несёт кнопку "Пуск" (см.
    // START_BTN_W), единственный путь к выключению/консоли.
    if (going_down && my >= taskbar_top()) {
        int sx, sy, sw, sh;
        start_button_rect(&sx, &sy, &sw, &sh);
        if (mx >= sx && mx < sx + sw && my >= sy && my < sy + sh) {
            g_start_menu_open = !g_start_menu_open;
            g_content_dirty = 1;
            return;
        }
        int btn_idx = (mx - START_BTN_W) / TASKBAR_BTN_W;
        if (btn_idx >= 0 && btn_idx < g_window_count) {
            int idx = g_order[btn_idx];
            wm_window_t *tw = &g_windows[idx];
            if (tw->minimized) {
                // Единственный способ развернуть обратно (см. minimize_window()).
                tw->minimized = 0;
                raise_to_top(idx);
                set_focus(idx);
            } else if (g_focused == idx) {
                // Повторный клик на уже активной задаче сворачивает её (как в Windows).
                minimize_window(idx);
            } else {
                raise_to_top(idx);
                set_focus(idx);
            }
            g_content_dirty = 1;
        }
        return;
    }

    if (going_down) {
        int idx = find_window_at(mx, my);
        if (idx >= 0) {
            wm_window_t *w = &g_windows[idx];
            raise_to_top(idx);
            set_focus(idx);
            g_content_dirty = 1;

            int min_x, max_x, close_x, btn_y, btn_size;
            titlebar_button_rects(w, &min_x, &max_x, &close_x, &btn_y, &btn_size);
            int in_titlebar = my < w->y + WM_TITLEBAR_HEIGHT;

            if (in_titlebar && mx >= close_x && mx < close_x + btn_size &&
                my >= btn_y && my < btn_y + btn_size) {
                queue_push(w, LUFIRA_GUI_EVENT_CLOSE, 0, 0, 0);
            } else if (in_titlebar && mx >= max_x && mx < max_x + btn_size &&
                       my >= btn_y && my < btn_y + btn_size) {
                toggle_maximize(w);
            } else if (in_titlebar && mx >= min_x && mx < min_x + btn_size &&
                       my >= btn_y && my < btn_y + btn_size) {
                minimize_window(idx);
            } else if (in_titlebar) {
                // Таскание maximized-окна за титлбар отключено намеренно —
                // сначала restore (кнопка maximize), потом таскать.
                if (!w->maximized) {
                    g_dragging = idx;
                    g_drag_off_x = mx - w->x;
                    g_drag_off_y = my - w->y;
                }
            } else {
                int edge_right = 0, edge_bottom = 0;
                if (in_resize_zone(w, mx, my, &edge_right, &edge_bottom)) {
                    g_resizing = idx;
                    g_resize_edge_right = edge_right;
                    g_resize_edge_bottom = edge_bottom;
                    g_resize_start_mx = mx; g_resize_start_my = my;
                    g_resize_start_w = w->w; g_resize_start_h = w->h;
                } else {
                    int rel_x = mx - (w->x + WM_BORDER);
                    int rel_y = my - (w->y + WM_TITLEBAR_HEIGHT);
                    queue_push(w, LUFIRA_GUI_EVENT_MOUSE_DOWN, rel_x, rel_y, buttons);
                }
            }
        } else if (!try_launch_desktop_icon(mx, my)) {
            set_focus(-1);
        }
    }

    if (going_up) {
        int was_dragging = (g_dragging >= 0);
        if (was_dragging) {
            // Пишем на диск только когда реально отпустили (не на каждый тик драга).
            wm_window_t *w = &g_windows[g_dragging];
            update_saved_pos(w->title, w->x, w->y, w->w, w->h);
        }
        g_dragging = -1;

        // Как у драга — пишем один раз на mouse-up, не на каждый тик ресайза.
        int was_resizing = (g_resizing >= 0);
        if (was_resizing) {
            wm_window_t *w = &g_windows[g_resizing];
            update_saved_pos(w->title, w->x, w->y, w->w, w->h);
        }
        g_resizing = -1;

        if (g_focused >= 0 && g_windows[g_focused].in_use) {
            wm_window_t *w = &g_windows[g_focused];
            int rel_x = mx - (w->x + WM_BORDER);
            int rel_y = my - (w->y + WM_TITLEBAR_HEIGHT);
            queue_push(w, LUFIRA_GUI_EVENT_MOUSE_UP, rel_x, rel_y, buttons);
        }
    }

    if (g_dragging >= 0 && (buttons & 1)) {
        wm_window_t *w = &g_windows[g_dragging];
        int nx = mx - g_drag_off_x;
        int ny = my - g_drag_off_y;
        int max_y = taskbar_top(); // таскбар всегда показан, пока есть окна (т.е. и во время драга)
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx + outer_w(w) > (int)g_screen_w) nx = (int)g_screen_w - outer_w(w);
        if (ny + outer_h(w) > max_y) ny = max_y - outer_h(w);
        if (nx != w->x || ny != w->y) {
            w->x = nx; w->y = ny;
            g_content_dirty = 1;
        }
    }

    if (g_resizing >= 0 && (buttons & 1)) {
        wm_window_t *w = &g_windows[g_resizing];
        int new_w = w->w, new_h = w->h;
        // Дельта от ЗАФИКСИРОВАННОГО на mouse-down размера — см.
        // комментарий у g_resize_start_* выше, не накопительно.
        if (g_resize_edge_right) new_w = g_resize_start_w + (mx - g_resize_start_mx);
        if (g_resize_edge_bottom) new_h = g_resize_start_h + (my - g_resize_start_my);
        if (new_w < WM_MIN_WIN_W) new_w = WM_MIN_WIN_W;
        if (new_h < WM_MIN_WIN_H) new_h = WM_MIN_WIN_H;
        // Внешняя рамка не должна вылезать за экран/под таскбар.
        if (w->x + new_w + 2 * WM_BORDER > (int)g_screen_w)
            new_w = (int)g_screen_w - w->x - 2 * WM_BORDER;
        if (w->y + new_h + WM_TITLEBAR_HEIGHT + WM_BORDER > taskbar_top())
            new_h = taskbar_top() - w->y - WM_TITLEBAR_HEIGHT - WM_BORDER;
        if (new_w < WM_MIN_WIN_W) new_w = WM_MIN_WIN_W;
        if (new_h < WM_MIN_WIN_H) new_h = WM_MIN_WIN_H;

        if (new_w != w->w || new_h != w->h) {
            resize_window_buffer(w, new_w, new_h);
            queue_push(w, LUFIRA_GUI_EVENT_RESIZE, w->w, w->h, 0);
            g_content_dirty = 1;
        }
    }
}

#define CURSOR_H 14
// Bbox всего силуэта курсора (треугольник+пятка+1px обводка, см.
// draw_cursor()) относительно хотспота (x,y): от (x+CURSOR_BBOX_X_OFF,
// y+CURSOR_BBOX_Y_OFF), размер CURSOR_BBOX_W x CURSOR_BBOX_H. present_frame()
// использует это, чтобы на чисто-курсорном кадре трогать только эту
// область, не весь экран (см. разбор задержки курсора там же).
#define CURSOR_BBOX_X_OFF -1
#define CURSOR_BBOX_Y_OFF -1
#define CURSOR_BBOX_W 16
#define CURSOR_BBOX_H 20

// Залитый треугольник-стрелка (остриё в x,y, диагональ вниз-вправо) +
// короткая "пятка" вдоль левого края — силуэт классического курсора, не
// голая диагональная линия.
static void draw_cursor_shape(int x, int y, uint32_t color) {
    for (int row = 0; row < CURSOR_H; row++)
        for (int col = 0; col <= row; col++)
            fb_set(x + col, y + row, color);
    for (int row = CURSOR_H; row < CURSOR_H + 4; row++)
        fb_set(x, y + row, color);
}

// Было: 1px белая диагональ (баг: курсор "бледный, терялся на светлых
// окнах"). Теперь залитый треугольник, CURSOR_H=14 (было 12), с чёрной
// обводкой: силуэт рисуется 9 раз — 8 смещений чёрным по контуру, затем
// сам силуэт белым поверх (приём обводки текста) — виден на любом фоне.
static void draw_cursor(int x, int y) {
    uint32_t white = wm_convert_color(0xffffff);
    uint32_t black = wm_convert_color(0x000000);
    static const int offsets[8][2] = {
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1},
    };
    for (int i = 0; i < 8; i++) draw_cursor_shape(x + offsets[i][0], y + offsets[i][1], black);
    draw_cursor_shape(x, y, white);
}

// Дорогая часть: фон+ярлыки+окна+таскбар — только когда сцена реально
// изменилась (g_content_dirty), не на каждое шевеление мыши. Пишет в
// g_fb (present_frame() ниже выставляет g_draw_target).
static void composite_scene(void) {
    uint32_t desktop_bg = wm_convert_color(DESKTOP_BG);
    uint32_t border_c = wm_convert_color(BORDER_COLOR);
    uint32_t titlebar_active = wm_convert_color(TITLEBAR_ACTIVE);
    uint32_t titlebar_inactive = wm_convert_color(TITLEBAR_INACTIVE);
    uint32_t title_text = wm_convert_color(TITLE_TEXT_COLOR);
    uint32_t close_btn = wm_convert_color(CLOSE_BTN_COLOR);
    uint32_t titlebar_btn_c = wm_convert_color(TITLEBAR_BTN_BG);
    uint32_t white = wm_convert_color(0xffffff);

    fb_fill_rect(0, 0, (int)g_screen_w, (int)g_screen_h, desktop_bg);

    // Ярлыки — фоновый слой рабочего стола (над заливкой фона, под окнами —
    // так их закрывает открытое поверх окно, как в реальной системе).
    uint32_t launcher_bg = wm_convert_color(LAUNCHER_BG);
    uint32_t launcher_border = wm_convert_color(LAUNCHER_BORDER);
    uint32_t launcher_text = wm_convert_color(LAUNCHER_TEXT);
    for (int i = 0; i < NUM_LAUNCHERS; i++) {
        int lx, ly;
        launcher_rect(i, &lx, &ly);
        fb_fill_rect(lx, ly, LAUNCHER_ICON_W, LAUNCHER_ICON_H, launcher_border);
        fb_fill_rect(lx + 1, ly + 1, LAUNCHER_ICON_W - 2, LAUNCHER_ICON_H - 2, launcher_bg);

        const char *label = g_launchers[i].label;
        int label_len = (int)strlen(label);
        int ltx = lx + (LAUNCHER_ICON_W - label_len * WM_CHAR_W) / 2;
        if (ltx < lx + 2) ltx = lx + 2;
        int lty = ly + (LAUNCHER_ICON_H - WM_CHAR_H) / 2;
        for (int c = 0; label[c] && ltx + WM_CHAR_W < lx + LAUNCHER_ICON_W - 2; c++) {
            fb_draw_glyph(ltx, lty, label[c], launcher_text, launcher_bg);
            ltx += WM_CHAR_W;
        }
    }

    for (int i = 0; i < g_window_count; i++) {
        wm_window_t *w = &g_windows[g_order[i]];
        if (!w->in_use || w->minimized) continue; // свёрнутое — только кнопка в таскбаре, см. ниже

        int is_focused = (g_order[i] == g_focused);
        int ow = outer_w(w), oh = outer_h(w);
        uint32_t titlebar_c = is_focused ? titlebar_active : titlebar_inactive;

        fb_fill_rect(w->x, w->y, ow, oh, border_c);
        fb_fill_rect(w->x + WM_BORDER, w->y, w->w, WM_TITLEBAR_HEIGHT, titlebar_c);

        int min_x, max_x, close_x, btn_y, btn_size;
        titlebar_button_rects(w, &min_x, &max_x, &close_x, &btn_y, &btn_size);

        int tx = w->x + WM_BORDER + 4;
        int ty = w->y + (WM_TITLEBAR_HEIGHT - WM_CHAR_H) / 2;
        for (int c = 0; w->title[c] && tx + WM_CHAR_W < min_x - 4; c++) {
            fb_draw_glyph(tx, ty, (unsigned char)w->title[c], title_text, titlebar_c);
            tx += WM_CHAR_W;
        }

        fb_fill_rect(min_x, btn_y, btn_size, btn_size, titlebar_btn_c);
        draw_titlebar_icon_minimize(min_x, btn_y, btn_size, white);
        fb_fill_rect(max_x, btn_y, btn_size, btn_size, titlebar_btn_c);
        draw_titlebar_icon_maxrestore(max_x, btn_y, btn_size, white, w->maximized);
        fb_fill_rect(close_x, btn_y, btn_size, btn_size, close_btn);
        fb_draw_glyph(close_x + 4, btn_y + 4, 'X', white, close_btn);

        fb_blit(w->x + WM_BORDER, w->y + WM_TITLEBAR_HEIGHT, w->pixels, w->w, w->h);
        draw_resize_grip(w);
    }

    // Таскбар рисуется после окон (поверх них), но до курсора — см.
    // TASKBAR_HEIGHT выше и hit-тест в handle_mouse_transition().
    uint32_t taskbar_bg = wm_convert_color(TASKBAR_BG);
    uint32_t taskbar_btn_bg = wm_convert_color(TASKBAR_BTN_BG);
    uint32_t taskbar_btn_active = wm_convert_color(TASKBAR_BTN_ACTIVE);
    uint32_t taskbar_text = wm_convert_color(TASKBAR_TEXT);

    int tb_y = taskbar_top();
    fb_fill_rect(0, tb_y, (int)g_screen_w, TASKBAR_HEIGHT, taskbar_bg);
    // Кнопки окон начинаются после кнопки "Пуск" (см. START_BTN_W) — левый
    // край таскбара уже занят ей (старая кнопка "Exit" справа не мешала).
    for (int i = 0; i < g_window_count; i++) {
        wm_window_t *w = &g_windows[g_order[i]];
        int bx = START_BTN_W + i * TASKBAR_BTN_W;
        if (bx >= (int)g_screen_w) break; // не влезло - дальше рисовать некуда
        int bw = TASKBAR_BTN_W - 4;
        if (bx + TASKBAR_BTN_W > (int)g_screen_w) bw = (int)g_screen_w - bx - 4;
        uint32_t btn_c = (g_order[i] == g_focused) ? taskbar_btn_active : taskbar_btn_bg;
        fb_fill_rect(bx + 2, tb_y + 2, bw, TASKBAR_HEIGHT - 4, btn_c);

        int tx = bx + 8, ty = tb_y + (TASKBAR_HEIGHT - WM_CHAR_H) / 2;
        for (int c = 0; w->title[c] && tx + WM_CHAR_W < bx + TASKBAR_BTN_W - 6; c++) {
            fb_draw_glyph(tx, ty, (unsigned char)w->title[c], taskbar_text, btn_c);
            tx += WM_CHAR_W;
        }
    }

    // Кнопка "Пуск" подсвечена активным цветом, пока меню открыто — обычная
    // обратная связь "нажата". См. START_BTN_W/start_button_rect() выше.
    {
        int sx, sy, sw, sh;
        start_button_rect(&sx, &sy, &sw, &sh);
        uint32_t start_bg = g_start_menu_open ? taskbar_btn_active : taskbar_btn_bg;
        fb_fill_rect(sx, sy, sw, sh, start_bg);
        const char *label = "Start";
        int stx = sx + (sw - 5 * WM_CHAR_W) / 2;
        int sty = sy + (sh - WM_CHAR_H) / 2;
        for (int c = 0; label[c]; c++) {
            fb_draw_glyph(stx, sty, label[c], taskbar_text, start_bg);
            stx += WM_CHAR_W;
        }
    }

    // Меню "Пуск" рисуется последним здесь — поверх окон и таскбара; курсор
    // поверх него рисует present_frame()/draw_cursor() отдельно.
    if (g_start_menu_open) {
        uint32_t menu_bg = wm_convert_color(START_MENU_BG);
        for (int i = 0; i < NUM_START_MENU_ITEMS; i++) {
            int ix, iy, iw, ih;
            start_menu_item_rect(i, &ix, &iy, &iw, &ih);
            fb_fill_rect(ix, iy, iw, ih, menu_bg);
            const char *label = g_start_menu_items[i].label;
            int ltx = ix + 10;
            int lty = iy + (ih - WM_CHAR_H) / 2;
            for (int c = 0; label[c]; c++) {
                fb_draw_glyph(ltx, lty, label[c], taskbar_text, menu_bg);
                ltx += WM_CHAR_W;
            }
            if (i > 0) fb_fill_rect(ix, iy, iw, 1, wm_convert_color(0x1a1a22));
        }
    }
}

static void cursor_bbox(int cx, int cy, int *x0, int *y0, int *x1, int *y1) {
    *x0 = cx + CURSOR_BBOX_X_OFF;
    *y0 = cy + CURSOR_BBOX_Y_OFF;
    *x1 = *x0 + CURSOR_BBOX_W;
    *y1 = *y0 + CURSOR_BBOX_H;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > (int)g_screen_w) *x1 = (int)g_screen_w;
    if (*y1 > (int)g_screen_h) *y1 = (int)g_screen_h;
}

// Копирует [x0,x1)x[y0,y1) (экранные координаты, клипнутые вызывающим) из
// g_fb в буфер презентации построчно — та же идея, что у fb_blit().
static void present_copy_rect(int x0, int y0, int x1, int y1) {
    for (int py = y0; py < y1; py++) {
        memcpy(g_present_fb + (size_t)py * g_screen_w + x0,
               g_fb + (size_t)py * g_screen_w + x0,
               (size_t)(x1 - x0) * sizeof(uint32_t));
    }
}

// full=1 — полный путь: memcpy всей сцены g_fb->g_present_fb, курсор,
// sys_fb_present() целиком. Нужен после composite_scene(), когда неизвестно
// что именно изменилось.
//
// full=0 — лёгкий путь, чистое движение курсора. Было: full-путь
// безусловно на каждое движение мыши — memcpy ~4МБ (1280x800) дважды
// (ещё раз в ядре, MMIO/VRAM дороже RAM, см. console.c gfx_present()) на
// каждое из десятков событий мыши/сек — заметная задержка курсора.
// Теперь трогаем только объединение bbox'ов старой/новой позиции курсора
// (десятки пикселей): восстанавливаем старый силуэт из g_fb, рисуем
// новый, sys_fb_present_rect() копирует в ядре тоже только эту область.
//
// Если старая/новая позиция курсора далеко друг от друга (резкий скачок),
// объединённый bbox может занять почти весь экран — тогда частичный путь
// не дешевле полного. PARTIAL_PRESENT_MAX_AREA — порог с запасом над
// типичным движением, за которым падаем на полный путь.
#define PARTIAL_PRESENT_MAX_AREA (200 * 200)

static void present_frame(int full) {
    if (!full) {
        int ox0, oy0, ox1, oy1, nx0, ny0, nx1, ny1;
        cursor_bbox(g_last_cursor_x, g_last_cursor_y, &ox0, &oy0, &ox1, &oy1);
        cursor_bbox(g_mouse_x, g_mouse_y, &nx0, &ny0, &nx1, &ny1);
        int ux0 = ox0 < nx0 ? ox0 : nx0;
        int uy0 = oy0 < ny0 ? oy0 : ny0;
        int ux1 = ox1 > nx1 ? ox1 : nx1;
        int uy1 = oy1 > ny1 ? oy1 : ny1;
        if (ux0 < ux1 && uy0 < uy1 && (long)(ux1 - ux0) * (long)(uy1 - uy0) <= PARTIAL_PRESENT_MAX_AREA) {
            present_copy_rect(ux0, uy0, ux1, uy1);
            g_draw_target = g_present_fb;
            draw_cursor(g_mouse_x, g_mouse_y);
            sys_fb_present_rect(g_present_fb, g_screen_w, g_screen_h,
                                 ux0, uy0, (unsigned)(ux1 - ux0), (unsigned)(uy1 - uy0));
            g_last_cursor_x = g_mouse_x;
            g_last_cursor_y = g_mouse_y;
            g_draw_target = g_fb;
            return;
        }
        // Скачок слишком большой (или вообще не было прошлой позиции) —
        // падаем обратно на полный путь ниже.
    }

    memcpy(g_present_fb, g_fb, (size_t)g_screen_w * g_screen_h * sizeof(uint32_t));
    g_draw_target = g_present_fb;
    draw_cursor(g_mouse_x, g_mouse_y);
    sys_fb_present(g_present_fb, g_screen_w, g_screen_h);
    g_last_cursor_x = g_mouse_x;
    g_last_cursor_y = g_mouse_y;
    g_draw_target = g_fb; // сбрасываем обратно - composite_scene() снова пишет в g_fb
}

// ===== Обработка клиентских RPC-запросов (WM_OP_WIN_*, wm_protocol.h) =====

static void do_win_create(uint32_t owner_pid, const struct wm_request *req, struct wm_reply *rep) {
    dbg_log_num("do_win_create: owner_pid=", (long)owner_pid);
    int x = req->a[0], y = req->a[1], w = req->a[2], h = req->a[3];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || g_window_count >= WM_MAX_WINDOWS) {
        dbg_log("do_win_create: REJECTED bad size/count\n");
        rep->result = -1; return;
    }
    int idx = find_free_slot();
    if (idx < 0) { dbg_log("do_win_create: REJECTED no free slot\n"); rep->result = -1; return; }

    uint32_t *pixels = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!pixels) { rep->result = -1; return; }
    memset(pixels, 0, (size_t)w * (size_t)h * sizeof(uint32_t));

    wm_window_t *win = &g_windows[idx];
    win->in_use = 1;
    win->owner_pid = owner_pid;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->pixels = pixels;
    win->events.head = win->events.tail = win->events.count = 0;
    // Слот переиспользуется после destroy — он не чистит minimized/maximized,
    // иначе новое окно в старом слоте могло бы родиться уже "свёрнутым".
    win->minimized = 0;
    win->maximized = 0;
    win->bg_color = 0; // чёрный по умолчанию, пока приложение не сделает первый sys_win_fill()

    int n = 0;
    while (req->str[n] && n < WM_TITLE_MAX - 1) { win->title[n] = req->str[n]; n++; }
    win->title[n] = '\0';

    // См. WM_STATE_PATH — только x/y, w/h остаются запрошенными клиентом.
    // Clamp на случай, если файл писался при другом разрешении экрана.
    int saved_x, saved_y;
    if (find_saved_pos(win->title, &saved_x, &saved_y)) {
        win->x = saved_x; win->y = saved_y;
        if (win->x < 0) win->x = 0;
        if (win->y < 0) win->y = 0;
        if (win->x + outer_w(win) > (int)g_screen_w) win->x = (int)g_screen_w - outer_w(win);
        if (win->y + outer_h(win) > (int)g_screen_h) win->y = (int)g_screen_h - outer_h(win);
    }

    g_order[g_window_count++] = idx;
    set_focus(idx);
    g_content_dirty = 1;
    rep->result = idx;
    dbg_log_num("do_win_create: OK idx=", (long)idx);
}

// Общее тело закрытия — используется и WM_OP_WIN_DESTROY, и
// WM_NOTIFY_PROCESS_EXIT (владелец умер без закрытия). window_id уже
// провалидирован вызывающим (get_owned() либо owner_pid-скан).
static void destroy_window_by_index(int window_id) {
    wm_window_t *w = &g_windows[window_id];

    update_saved_pos(w->title, w->x, w->y, w->w, w->h);

    free(w->pixels);
    w->pixels = NULL;
    w->in_use = 0;

    for (int i = 0; i < g_window_count; i++) {
        if (g_order[i] == window_id) {
            for (int j = i; j < g_window_count - 1; j++) g_order[j] = g_order[j + 1];
            g_window_count--;
            break;
        }
    }
    if (g_focused == window_id) g_focused = (g_window_count > 0) ? g_order[g_window_count - 1] : -1;
    if (g_dragging == window_id) g_dragging = -1;
    if (g_resizing == window_id) g_resizing = -1;

    // Рабочий стол рисуется постоянно (см. main()) — закрытие последнего
    // окна просто обнажает пустой стол; возврат к консоли делает кернел
    // (process_exit(), process.c) только когда завершается сам WM.
    g_content_dirty = 1;
}

static void do_win_destroy(uint32_t owner_pid, const struct wm_request *req, struct wm_reply *rep) {
    int window_id = req->a[0];
    wm_window_t *w = get_owned(owner_pid, window_id);
    if (!w) { rep->result = -1; return; }

    destroy_window_by_index(window_id);
    rep->result = 0;
}

// WM_NOTIFY_PROCESS_EXIT: клиент умер без WM_OP_WIN_DESTROY — без этого
// его окна висели бы вечно на несуществующем pid. Снимок id'ов за один
// проход: destroy_window_by_index() сама правит g_order/g_window_count,
// повторный проход по "живому" массиву без снимка сдвигал бы индексы
// под ногами.
static void destroy_windows_owned_by(uint32_t pid) {
    int to_destroy[WM_MAX_WINDOWS];
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS; i++)
        if (g_windows[i].in_use && g_windows[i].owner_pid == pid) to_destroy[n++] = i;
    for (int i = 0; i < n; i++) destroy_window_by_index(to_destroy[i]);
}

static void do_win_fill(uint32_t owner_pid, const struct wm_request *req, struct wm_reply *rep) {
    wm_window_t *w = get_owned(owner_pid, req->a[0]);
    if (!w) { rep->result = -1; return; }
    uint32_t c = wm_convert_color((uint32_t)req->a[1]);
    for (int i = 0; i < w->w * w->h; i++) w->pixels[i] = c;
    // Красим окно целиком — почти всегда первая строка redraw() приложения,
    // так что bg_color — лучшее доступное приближение "фона окна" для
    // будущего ресайза (resize_window_buffer() выше).
    w->bg_color = c;
    g_content_dirty = 1;
    rep->result = 0;
}

static void do_win_draw_rect(uint32_t owner_pid, const struct wm_request *req, struct wm_reply *rep) {
    wm_window_t *w = get_owned(owner_pid, req->a[0]);
    if (!w) { rep->result = -1; return; }
    int x = req->a[1], y = req->a[2], rw = req->a[3], rh = req->a[4];
    uint32_t c = wm_convert_color((uint32_t)req->a[5]);

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + rw; if (x1 > w->w) x1 = w->w;
    int y1 = y + rh; if (y1 > w->h) y1 = w->h;

    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++)
            w->pixels[py * w->w + px] = c;

    g_content_dirty = 1;
    rep->result = 0;
}

static void do_win_draw_text(uint32_t owner_pid, const struct wm_request *req, struct wm_reply *rep) {
    wm_window_t *w = get_owned(owner_pid, req->a[0]);
    if (!w) { rep->result = -1; return; }
    uint32_t c = wm_convert_color((uint32_t)req->a[3]);
    int x = req->a[1], y = req->a[2];

    for (int i = 0; req->str[i]; i++)
        win_draw_glyph(w->pixels, w->w, w->h, x + i * WM_CHAR_W, y, (unsigned char)req->str[i], c);

    g_content_dirty = 1;
    rep->result = 0;
}

static void do_win_move(uint32_t owner_pid, const struct wm_request *req, struct wm_reply *rep) {
    wm_window_t *w = get_owned(owner_pid, req->a[0]);
    if (!w) { rep->result = -1; return; }
    w->x = req->a[1]; w->y = req->a[2];
    g_content_dirty = 1;
    rep->result = 0;
}

static void do_win_poll_event(uint32_t owner_pid, const struct wm_request *req, struct wm_reply *rep) {
    wm_window_t *w = get_owned(owner_pid, req->a[0]);
    if (!w) { rep->result = 0; return; }
    wm_event_queue_t *q = &w->events;
    if (q->count == 0) { rep->result = 0; return; }
    struct lufira_gui_event *ev = &q->events[q->head];
    q->head = (q->head + 1) % WM_EVENT_QUEUE_SIZE;
    q->count--;
    rep->result = 1;
    rep->ev_type = ev->type;
    rep->ev_x = ev->x;
    rep->ev_y = ev->y;
    rep->ev_key = ev->key_or_button;
}

static void handle_client_request(uint32_t sender_pid, const struct wm_request *req) {
    struct wm_reply rep;
    memset(&rep, 0, sizeof(rep));

    switch (req->opcode) {
        case WM_OP_WIN_CREATE:     do_win_create(sender_pid, req, &rep); break;
        case WM_OP_WIN_DESTROY:    do_win_destroy(sender_pid, req, &rep); break;
        case WM_OP_WIN_FILL:       do_win_fill(sender_pid, req, &rep); break;
        case WM_OP_WIN_DRAW_RECT:  do_win_draw_rect(sender_pid, req, &rep); break;
        case WM_OP_WIN_DRAW_TEXT:  do_win_draw_text(sender_pid, req, &rep); break;
        case WM_OP_WIN_POLL_EVENT: do_win_poll_event(sender_pid, req, &rep); break;
        case WM_OP_WIN_MOVE:       do_win_move(sender_pid, req, &rep); break;
        default: rep.result = -1; break;
    }

    sys_ipc_send((long)sender_pid, &rep, sizeof(rep));
}

static void handle_kernel_input(const struct wm_request *req) {
    if (req->opcode == WM_INPUT_KEY) {
        // Экран всегда принадлежит WM — клавиша идёт сфокусированному окну,
        // либо, без фокуса, просто проглатывается (как в любой оконной системе:
        // клик по пустому столу снимает фокус, печать после этого никуда не идёт).
        if (g_focused >= 0 && g_windows[g_focused].in_use)
            queue_push(&g_windows[g_focused], LUFIRA_GUI_EVENT_KEY, 0, 0, req->a[0]);
        return;
    }
    if (req->opcode == WM_INPUT_MOUSE) {
        int mx = req->a[0], my = req->a[1], buttons = req->a[2];
        if (buttons != g_prev_buttons || g_dragging >= 0 || g_resizing >= 0) handle_mouse_transition(mx, my, buttons);
        g_prev_buttons = buttons;
        // Курсор должен перерисовываться и на чистое движение мыши (без смены
        // кнопок), но только курсор (g_cursor_moved) — см. g_content_dirty выше.
        if (mx != g_mouse_x || my != g_mouse_y) g_cursor_moved = 1;
        g_mouse_x = mx; g_mouse_y = my;
        return;
    }
    if (req->opcode == WM_NOTIFY_PROCESS_EXIT) {
        destroy_windows_owned_by((uint32_t)req->a[0]);
        return;
    }
}

static void dispatch_message(const struct lufira_ipc_msg *msg) {
    const struct wm_request *req = (const struct wm_request *)msg->data;
    if (msg->sender_pid == WM_SENDER_KERNEL) handle_kernel_input(req);
    else handle_client_request(msg->sender_pid, req);
}

// Один redraw() клиента — это обычно несколько sys_win_draw_*()/
// sys_win_fill() RPC подряд. Было: composite_scene() запускалась после
// каждого отдельно (баг: CPU при печати). Теперь недолго ждём
// (COALESCE_WAIT_MS) следующее сообщение той же пачки вместо пересборки
// сцены на каждое.
#define COALESCE_WAIT_MS 20

// Баг: "команда в терминале в gui — всё зависает". Коалесцирование ниже
// раньше не имело потолка по суммарному времени — terminal.c шлёт
// draw-RPC каждые ~16мс (короче 20мс порога COALESCE_WAIT_MS), так что
// во время долгого вывода (ls с большим листингом) WM бесконечно крутился
// в while() и ни разу не презентовал кадр — ни текст, ни даже курсор не
// двигались. Теперь жёсткий потолок на батч (COALESCE_MAX_TICKS,
// sys_gettick() — 10мс/тик): после него презентуем кадр, следующий батч
// подхватит остаток.
#define COALESCE_MAX_TICKS 3 // 30мс — ниже кадра терминала (16мс), хватает для одного redraw()

int main(void) {
    if (sys_wm_register() != 0) return 1; // уже есть другой WM — ровно один на систему

    struct lufira_fb_info fb_info;
    if (sys_fb_info(&fb_info) != 0) return 1;
    g_screen_w = fb_info.width;
    g_screen_h = fb_info.height;
    g_pixel_format = fb_info.pixel_format;

    g_fb = (uint32_t *)malloc((size_t)g_screen_w * g_screen_h * sizeof(uint32_t));
    g_present_fb = (uint32_t *)malloc((size_t)g_screen_w * g_screen_h * sizeof(uint32_t));
    if (!g_fb || !g_present_fb) return 1;
    g_draw_target = g_fb;

    long font_size = sys_fb_font(NULL, 0); // max_bytes=0 — просто узнать реальный размер
    if (font_size <= 0) return 1;
    g_font = (uint8_t *)malloc((size_t)font_size);
    if (!g_font) return 1;
    sys_fb_font(g_font, (unsigned long)font_size);
    g_font_glyphs = (int)(font_size / 8);

    for (int i = 0; i < WM_MAX_WINDOWS; i++) { g_windows[i].in_use = 0; g_order[i] = -1; }

    load_wm_state(); // этап 3, персистентность — см. WM_STATE_PATH выше

    // Рабочий стол виден сразу, ещё до первого sys_win_create() — через
    // него запускается первое приложение (клик по ярлыку), а не из консоли.
    composite_scene();
    present_frame(1);
    g_content_dirty = 0;
    g_cursor_moved = 0;

    for (;;) {
        struct lufira_ipc_msg msg;
        if (sys_ipc_recv(&msg, -1) != 1) continue; // -1 — ждать первое сообщение пачки неограниченно
        dispatch_message(&msg);

        // Пока сцена дирти и сообщения продолжают приходить быстрее
        // COALESCE_WAIT_MS — вероятно та же клиентская перерисовка, обрабатываем
        // и откладываем пересборку. batch_deadline (COALESCE_MAX_TICKS) не даёт
        // источнику, шлющему непрерывно (терминал во время вывода), держать нас
        // в этом while() вечно без единого кадра.
        long batch_deadline = sys_gettick() + COALESCE_MAX_TICKS;
        while (g_content_dirty && sys_gettick() < batch_deadline &&
               sys_ipc_recv(&msg, COALESCE_WAIT_MS) == 1)
            dispatch_message(&msg);

        if (g_content_dirty) {
            composite_scene();
            g_content_dirty = 0;
            present_frame(1);
        } else if (g_cursor_moved) {
            present_frame(0);
        }
        g_cursor_moved = 0;
    }
}
