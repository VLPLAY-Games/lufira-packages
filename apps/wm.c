// wm.c — v0.8 (GUI+WM), этап 3: оконный сервер как ОБЫЧНЫЙ userspace-
// процесс, не часть ядра. До этого момента (первый срез) вся эта логика
// жила в kernel/system/gui/gui.c — перенесена сюда почти 1:1 (та же
// таблица окон/z-order/drag/фокус/композитинг), только:
//   - пиксельные буферы окон и compositor-буфер — обычный malloc(), не
//     kmalloc() ядра;
//   - вместо прямых кернел-вызовов клиент (sys_win_*(), syscall.c в ядре)
//     теперь шлёт RPC-запрос через generic SYS_IPC_SEND/RECV (mailbox.h) —
//     см. протокол в <lufira/wm_protocol.h> (зеркало kernel/system/ipc/
//     wm_protocol.h);
//   - раньше gui_tick() вызывался каждый PIT-тик из ядра и САМ опрашивал
//     input_mouse_get_x/y/buttons() — теперь ядро толкает события
//     клавиатуры/мыши сюда же, в наш mailbox (sender_pid==0 — сентинел
//     "от ядра", WM_SENDER_KERNEL), и мы просто блокируемся в
//     sys_ipc_recv() между событиями, без опроса;
//   - convert_color()/битмап-шрифт — кернел-функции/static-данные, нам
//     сюда недостижимые: реплицируем byte-swap сами (wm_convert_color(),
//     та же формула, что в console.c) и забираем сами байты шрифта один
//     раз при старте через SYS_FB_FONT;
//   - итоговый кадр уходит на экран через SYS_FB_PRESENT, а не
//     console_mark_dirty()/gfx_present() напрямую (это уже не наше
//     адресное пространство).
//
// Без иконок (прямое указание пользователя) — только примитивы
// (прямоугольники, текст битмап-шрифтом).
//
// Этап 4: рабочий стол (фон + ярлыки запуска + таскбар) рисуется ПОСТОЯННО,
// с самого sys_wm_register() — больше не ждёт первого окна. Ярлыки
// запуска — замена отдельному GUI-клиенту desktop.c (удалён): пользователь
// явно попросил "выбор программ... на экране самом", не в отдельном окне,
// так что сам WM теперь и рисует подписанные прямоугольники прямо на фоне,
// и форкает/exec'ает выбранное приложение по клику (см. launch_app()).
// Раз рабочий стол теперь виден без единого открытого окна, у таскбара
// появилась постоянная кнопка "Exit" (sys_exit() самого WM) — иначе не
// было бы способа вернуться к текстовой консоли вовсе.

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

#define DESKTOP_BG        0x2b2b3a
#define TITLEBAR_ACTIVE   0x3a6ea5
#define TITLEBAR_INACTIVE 0x4a4a55
#define BORDER_COLOR      0x1a1a22
#define TITLE_TEXT_COLOR  0xffffff
#define CLOSE_BTN_COLOR   0xcc4444

// Этап 3, "таскбар" — постоянная полоса снизу экрана, кнопка на каждое
// открытое окно (не отдельное окно само по себе — просто ещё один слой
// компоновки в g_fb, тот же приём, что и рабочий стол/курсор).
#define TASKBAR_HEIGHT    28
#define TASKBAR_BTN_W     160
#define TASKBAR_BG        0x1f1f29
#define TASKBAR_BTN_BG    0x33334a
#define TASKBAR_BTN_ACTIVE 0x3a6ea5
#define TASKBAR_TEXT      0xffffff

// Этап 3, "сохранение позиции/размера окна" — простой текстовый конфиг
// "title=x,y,w,h" построчно, ключ — заголовок окна (на практике
// уникален: "Desktop"/"Counter Demo"/"Notepad"/...). w/h ЧИТАЮТСЯ И
// ПИШУТСЯ в файл (вперёд на будущее, когда появится resize), но пока
// РЕАЛЬНО ПРИМЕНЯЕТСЯ только x/y при создании окна — подставить сюда ещё
// и чужой w/h значило бы дать клиенту пиксельный буфер другого размера,
// чем тот, что он сам запросил и для которого рассчитал раскладку своих
// виджетов (gui_button_t/gui_textbox_t и т.п. считают contains() по
// ЗАПРОШЕННЫМ координатам) — клиент никак не узнал бы о подмене, раз
// sys_win_create() возвращает только id, не фактическую геометрию.
#define WM_STATE_PATH        "/etc/wm_state.conf"
#define WM_STATE_MAX_ENTRIES 32
#define WM_STATE_LINE_MAX    160

// Этап 4: "выбор программ не в отдельном окне, а на экране самом" — раньше
// был отдельный GUI-клиент desktop.c (окно "Desktop" с кнопками), теперь
// удалён: сам WM рисует эти ярлыки прямо в фоновом слое рабочего стола (под
// окнами, но над заливкой фона, см. composite_scene()) и сам же
// форкает/exec'ает выбранное приложение по клику. По-прежнему БЕЗ ИКОНОК
// (прямое указание пользователя) — просто подписанные прямоугольники, тот
// же стиль, что у кнопок таскбара.
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

#define TASKBAR_EXIT_W   70

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
} wm_window_t;

static wm_window_t g_windows[WM_MAX_WINDOWS];
static int g_order[WM_MAX_WINDOWS];
static int g_window_count = 0;
static int g_focused = -1;
static int g_dragging = -1;
static int g_drag_off_x = 0, g_drag_off_y = 0;

// НАЙДЕННЫЙ БАГ (жалоба пользователя: WM грузит CPU под 100%, особенно
// при движении мыши/печати, кулер шумит) — раньше тут был ОДИН флаг
// g_dirty, и ЛЮБОЕ изменение (в т.ч. чистое движение курсора без единого
// изменения содержимого) запускало composite_and_present(): полная
// заливка всего экрана фоном + перерисовка ВСЕХ ярлыков/окон/таскбара —
// тысячи скалярных записей пикселей на КАЖДОЕ событие мыши (а PS/2/USB
// шлют их десятками в секунду). Разделено на два флага: g_content_dirty
// (сцена реально изменилась - окно создано/сдвинуто/перерисовано,
// таскбар и т.п.) запускает дорогую composite_scene(); g_cursor_moved
// (сдвинулась только мышь) запускает дешёвый present_frame() — memcpy
// уже готовой сцены + маленький силуэт курсора поверх, без полной
// перерисовки. См. composite_scene()/present_frame() ниже.
static int g_content_dirty = 1;
static int g_cursor_moved = 0;
static int g_mouse_x = 0, g_mouse_y = 0, g_prev_buttons = 0;

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

// Та же формула, что convert_color() в kernel/drivers/console/console.c —
// см. комментарий у SYS_FB_INFO (syscall.h) про то, почему её нельзя
// позвать напрямую отсюда.
static uint32_t wm_convert_color(uint32_t color) {
    if (g_pixel_format == 1) return color;
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

// ===== Персистентность позиции/размера окна (/etc/wm_state.conf) =====
// Нет ни fopen() (stdio.h этой libc — только printf()), ни atoi()/
// snprintf() (stdlib.h — только malloc/free/exit) — читаем/пишем файл
// теми же сырыми sys_open/sys_read/sys_write/sys_lseek, что использует
// любая другая программа этой ОС (см. lufira-packages/base/cat.c, write.c),
// и парсим/форматируем числа вручную ниже.

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

// См. комментарий у g_launch_argv в бывшем desktop.c (удалён) — argv[]
// ОБЯЗАН жить в static/global памяти, не на стеке: launch_app() зовётся
// из глубины событийного цикла main(), короткий стековый массив рядом с
// верхом 16KB пользовательского стека не проходит фиксированную проверку
// диапазона в copy_user_string_array() (kernel/system/syscall/syscall.c).
static char *g_launch_argv[2];

static void launch_app(const char *path) {
    long pid = sys_fork();
    if (pid < 0) return;
    if (pid == 0) {
        // fork() дублирует ВЕСЬ образ WM (окна, compositor-буфer, mailbox —
        // process_create() внутри fork() заводит ребёнку свой, пустой, см.
        // process.h) — ничего из этого ребёнку не нужно, он немедленно
        // заменяет себя целевым приложением; тот же paттерн, что уже
        // использовал desktop.c и использует shell.c для любой команды.
        g_launch_argv[0] = (char *)path;
        g_launch_argv[1] = 0;
        sys_exec(path, g_launch_argv, (char **)0);
        sys_exit(127); // sys_exec не возвращается при успехе
    }
}

static int launcher_rect(int i, int *x, int *y) {
    if (i < 0 || i >= NUM_LAUNCHERS) return 0;
    *x = LAUNCHER_START_X;
    *y = LAUNCHER_START_Y + i * (LAUNCHER_ICON_H + LAUNCHER_GAP);
    return 1;
}

// Клик по рабочему столу, который не задел ни одно окно, — проверяем
// ярлыки. Возвращает 1 (и уже запускает приложение), если клик попал в
// ярлык, иначе 0.
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

// Кнопка "Exit" — зафиксирована в правом краю таскбара (всегда видна,
// независимо от числа открытых окон). Без неё, раз рабочий стол теперь
// отрисовывается ПОСТОЯННО (см. main()), не было бы способа вернуться к
// текстовой консоли вообще — ни один настоящий оконный менеджер не
// обходится без способа завершить сессию. Клик зовёт sys_exit() самого
// WM — ядро уже само восстанавливает текстовую консоль, когда
// завершается ИМЕННО зарегистрированный WM pid (см. process_exit(),
// kernel/system/process/process.c).
static void exit_button_rect(int *x, int *y, int *w, int *h) {
    *w = TASKBAR_EXIT_W - 8;
    *h = TASKBAR_HEIGHT - 4;
    *x = (int)g_screen_w - TASKBAR_EXIT_W;
    *y = (int)g_screen_h - TASKBAR_HEIGHT + 2;
}

// ===== Таблица окон / z-order (тот же приём, что был в gui.c) =====

static int outer_w(const wm_window_t *w) { return w->w + 2 * WM_BORDER; }
static int outer_h(const wm_window_t *w) { return w->h + WM_TITLEBAR_HEIGHT + WM_BORDER; }

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
        if (!w->in_use) continue;
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

static void handle_mouse_transition(int mx, int my, int buttons) {
    int going_down = (buttons & 1) && !(g_prev_buttons & 1);
    int going_up = !(buttons & 1) && (g_prev_buttons & 1);

    // Таскбар — отдельный слой НАД окнами (всегда поверх), так что его
    // клики проверяются первыми и дальше не идут к обычному window-hit-
    // тесту ниже. Теперь виден ПОСТОЯННО (не только при открытых окнах) —
    // в нём живёт кнопка "Exit", без которой не было бы способа вернуться
    // к текстовой консоли, раз рабочий стол тоже отрисовывается постоянно
    // (см. main()).
    if (going_down && my >= taskbar_top()) {
        int ex, ey, ew, eh;
        exit_button_rect(&ex, &ey, &ew, &eh);
        if (mx >= ex && mx < ex + ew && my >= ey && my < ey + eh) {
            sys_exit(0);
        }
        int btn_idx = mx / TASKBAR_BTN_W;
        if (btn_idx >= 0 && btn_idx < g_window_count) {
            int idx = g_order[btn_idx];
            raise_to_top(idx);
            set_focus(idx);
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

            int close_x = w->x + outer_w(w) - WM_BORDER - WM_CLOSE_BTN_SIZE - 2;
            int close_y = w->y + (WM_TITLEBAR_HEIGHT - WM_CLOSE_BTN_SIZE) / 2;
            if (mx >= close_x && mx < close_x + WM_CLOSE_BTN_SIZE &&
                my >= close_y && my < close_y + WM_CLOSE_BTN_SIZE) {
                queue_push(w, LUFIRA_GUI_EVENT_CLOSE, 0, 0, 0);
            } else if (my < w->y + WM_TITLEBAR_HEIGHT) {
                g_dragging = idx;
                g_drag_off_x = mx - w->x;
                g_drag_off_y = my - w->y;
            } else {
                int rel_x = mx - (w->x + WM_BORDER);
                int rel_y = my - (w->y + WM_TITLEBAR_HEIGHT);
                queue_push(w, LUFIRA_GUI_EVENT_MOUSE_DOWN, rel_x, rel_y, buttons);
            }
        } else if (!try_launch_desktop_icon(mx, my)) {
            set_focus(-1);
        }
    }

    if (going_up) {
        int was_dragging = (g_dragging >= 0);
        if (was_dragging) {
            // Этап 3, персистентность: сохраняем позицию, когда реально
            // закончили таскать (не на каждый промежуточный тик драга —
            // иначе писали бы файл на диск при каждом движении мыши).
            wm_window_t *w = &g_windows[g_dragging];
            update_saved_pos(w->title, w->x, w->y, w->w, w->h);
        }
        g_dragging = -1;
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
}

#define CURSOR_H 14

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

// НАЙДЕННЫЙ БАГ (жалоба пользователя: курсор "бледный и маленький",
// терялся на светлых окнах) — раньше это была одна белая диагональная
// линия в 1px. Теперь крупнее (CURSOR_H=14 вместо старых 12, да ещё и
// ЗАЛИТЫЙ треугольник, не линия) и с чёрной обводкой — силуэт рисуется
// 9 раз: 8 смещений на 1px по контуру чёрным, затем сам силуэт белым
// поверх (тот же приём, что и у обводки текста в любом растровом
// редакторе) — виден и на тёмном рабочем столе, и на белом/светлом фоне
// окна поверх него.
static void draw_cursor(int x, int y) {
    uint32_t white = wm_convert_color(0xffffff);
    uint32_t black = wm_convert_color(0x000000);
    static const int offsets[8][2] = {
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1},
    };
    for (int i = 0; i < 8; i++) draw_cursor_shape(x + offsets[i][0], y + offsets[i][1], black);
    draw_cursor_shape(x, y, white);
}

// Дорогая часть: фон + ярлыки + все окна + таскбар — ТОЛЬКО когда реально
// что-то изменилось в сцене (g_content_dirty), не на каждое шевеление
// мыши. Пишет в g_fb (g_draw_target выставлен на него вызывающим,
// present_frame() ниже).
static void composite_scene(void) {
    uint32_t desktop_bg = wm_convert_color(DESKTOP_BG);
    uint32_t border_c = wm_convert_color(BORDER_COLOR);
    uint32_t titlebar_active = wm_convert_color(TITLEBAR_ACTIVE);
    uint32_t titlebar_inactive = wm_convert_color(TITLEBAR_INACTIVE);
    uint32_t title_text = wm_convert_color(TITLE_TEXT_COLOR);
    uint32_t close_btn = wm_convert_color(CLOSE_BTN_COLOR);
    uint32_t white = wm_convert_color(0xffffff);

    fb_fill_rect(0, 0, (int)g_screen_w, (int)g_screen_h, desktop_bg);

    // Ярлыки запуска — фоновый слой рабочего стола (НАД заливкой фона, но
    // ПОД окнами — так их и закрывает открытое поверх окно, как в любой
    // настоящей системе). См. "Ярлыки запуска на рабочем столе" выше.
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
        if (!w->in_use) continue;

        int is_focused = (g_order[i] == g_focused);
        int ow = outer_w(w), oh = outer_h(w);
        uint32_t titlebar_c = is_focused ? titlebar_active : titlebar_inactive;

        fb_fill_rect(w->x, w->y, ow, oh, border_c);
        fb_fill_rect(w->x + WM_BORDER, w->y, w->w, WM_TITLEBAR_HEIGHT, titlebar_c);

        int tx = w->x + WM_BORDER + 4;
        int ty = w->y + (WM_TITLEBAR_HEIGHT - WM_CHAR_H) / 2;
        for (int c = 0; w->title[c] && tx + WM_CHAR_W < w->x + ow - WM_CLOSE_BTN_SIZE - 6; c++) {
            fb_draw_glyph(tx, ty, (unsigned char)w->title[c], title_text, titlebar_c);
            tx += WM_CHAR_W;
        }

        int close_x = w->x + ow - WM_BORDER - WM_CLOSE_BTN_SIZE - 2;
        int close_y = w->y + (WM_TITLEBAR_HEIGHT - WM_CLOSE_BTN_SIZE) / 2;
        fb_fill_rect(close_x, close_y, WM_CLOSE_BTN_SIZE, WM_CLOSE_BTN_SIZE, close_btn);
        fb_draw_glyph(close_x + 4, close_y + 4, 'X', white, close_btn);

        fb_blit(w->x + WM_BORDER, w->y + WM_TITLEBAR_HEIGHT, w->pixels, w->w, w->h);
    }

    // Таскбар — см. комментарий у TASKBAR_HEIGHT выше и hit-тест в
    // handle_mouse_transition(). Рисуется ПОСЛЕ окон (поверх них, как и
    // положено панели задач), но ДО курсора.
    uint32_t taskbar_bg = wm_convert_color(TASKBAR_BG);
    uint32_t taskbar_btn_bg = wm_convert_color(TASKBAR_BTN_BG);
    uint32_t taskbar_btn_active = wm_convert_color(TASKBAR_BTN_ACTIVE);
    uint32_t taskbar_text = wm_convert_color(TASKBAR_TEXT);

    int tb_y = taskbar_top();
    fb_fill_rect(0, tb_y, (int)g_screen_w, TASKBAR_HEIGHT, taskbar_bg);
    for (int i = 0; i < g_window_count; i++) {
        wm_window_t *w = &g_windows[g_order[i]];
        int bx = i * TASKBAR_BTN_W;
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

    // Кнопка "Exit" — см. комментарий у exit_button_rect() выше.
    {
        int ex, ey, ew, eh;
        exit_button_rect(&ex, &ey, &ew, &eh);
        uint32_t exit_bg = wm_convert_color(CLOSE_BTN_COLOR);
        fb_fill_rect(ex, ey, ew, eh, exit_bg);
        const char *label = "Exit";
        int etx = ex + (ew - 4 * WM_CHAR_W) / 2;
        int ety = ey + (eh - WM_CHAR_H) / 2;
        for (int c = 0; label[c]; c++) {
            fb_draw_glyph(etx, ety, label[c], taskbar_text, exit_bg);
            etx += WM_CHAR_W;
        }
    }

}

// Дешёвая часть: копирует уже готовую сцену (g_fb) в буфер презентации
// одним memcpy (быстрее любого числа scalar-записей пикселей), рисует
// курсор поверх И ТОЛЬКО ЕГО, затем отправляет кадр в ядро. Вызывается
// на КАЖДЫЙ кадр, который реально нужно показать (и после
// composite_scene(), и при чистом движении курсора) — сама по себе на
// порядки дешевле composite_scene().
static void present_frame(void) {
    memcpy(g_present_fb, g_fb, (size_t)g_screen_w * g_screen_h * sizeof(uint32_t));
    g_draw_target = g_present_fb;
    draw_cursor(g_mouse_x, g_mouse_y);
    sys_fb_present(g_present_fb, g_screen_w, g_screen_h);
    g_draw_target = g_fb; // сбрасываем обратно - composite_scene() снова пишет в g_fb
}

// ===== Обработка клиентских RPC-запросов (WM_OP_WIN_*, wm_protocol.h) =====

static void do_win_create(uint32_t owner_pid, const struct wm_request *req, struct wm_reply *rep) {
    int x = req->a[0], y = req->a[1], w = req->a[2], h = req->a[3];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || g_window_count >= WM_MAX_WINDOWS) {
        rep->result = -1; return;
    }
    int idx = find_free_slot();
    if (idx < 0) { rep->result = -1; return; }

    uint32_t *pixels = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!pixels) { rep->result = -1; return; }
    memset(pixels, 0, (size_t)w * (size_t)h * sizeof(uint32_t));

    wm_window_t *win = &g_windows[idx];
    win->in_use = 1;
    win->owner_pid = owner_pid;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->pixels = pixels;
    win->events.head = win->events.tail = win->events.count = 0;

    int n = 0;
    while (req->str[n] && n < WM_TITLE_MAX - 1) { win->title[n] = req->str[n]; n++; }
    win->title[n] = '\0';

    // Этап 3, персистентность — см. комментарий у WM_STATE_PATH: только
    // x/y, w/h остаются ЗАПРОШЕННЫМИ клиентом. Лёгкий clamp на случай,
    // если файл писался при другом разрешении экрана.
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
}

// Общее тело закрытия окна — переиспользуется и штатным WM_OP_WIN_DESTROY
// (владелец сам закрыл), и WM_NOTIFY_PROCESS_EXIT (владелец умер, не
// успев закрыть сам — см. handle_kernel_input()). window_id предполагается
// уже провалидированным вызывающим (get_owned() для первого случая,
// прямой owner_pid-скан для второго).
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

    // Этап 4: рабочий стол (фон + ярлыки + таскбар) теперь отрисовывается
    // ПОСТОЯННО, не только пока есть окна (см. main()) — закрытие
    // последнего окна просто обнажает пустой рабочий стол, как и
    // закрытие любого другого; отдельного возврата к текстовой консоли
    // тут больше не нужно (её восстанавливает сам кернел — process_exit(),
    // process.c — когда завершается САМ WM, см. кнопку "Exit" ниже).
    g_content_dirty = 1;
}

static void do_win_destroy(uint32_t owner_pid, const struct wm_request *req, struct wm_reply *rep) {
    int window_id = req->a[0];
    wm_window_t *w = get_owned(owner_pid, window_id);
    if (!w) { rep->result = -1; return; }

    destroy_window_by_index(window_id);
    rep->result = 0;
}

// WM_NOTIFY_PROCESS_EXIT (wm_protocol.h): клиент умер (краш/kill), не
// успев позвать WM_OP_WIN_DESTROY сам — без этого его окна висели бы на
// экране вечно, принадлежа уже не существующему pid (тот же случай, что
// в первом срезе закрывал gui_destroy_windows_owned_by() прямым кернел-
// вызовом; теперь WM не в ядре, так что вместо прямого вызова нас просто
// уведомляют, см. process.c). Снимок id'ов за один проход —
// destroy_window_by_index() сама правит g_order/g_window_count по ходу,
// повторный проход по "живому" массиву без снимка сдвигал бы индексы под
// ногами (тот же приём, что и в исходном gui_destroy_windows_owned_by()).
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
        // Этап 4: рабочий стол теперь отрисовывается постоянно (см.
        // main()) — ядру больше незачем решать, вернуть ли байт в
        // текстовую консоль (SYS_CONSOLE_INJECT, прежний приём первого
        // среза): раз экран всегда принадлежит WM, клавиша либо идёт
        // сфокусированному окну, либо, если фокуса нет, просто
        // проглатывается — ровно как в любой настоящей оконной системе
        // (клик по пустому рабочему столу снимает фокус, и печать после
        // этого никуда не идёт, а не "утекает" куда-то ещё).
        if (g_focused >= 0 && g_windows[g_focused].in_use)
            queue_push(&g_windows[g_focused], LUFIRA_GUI_EVENT_KEY, 0, 0, req->a[0]);
        return;
    }
    if (req->opcode == WM_INPUT_MOUSE) {
        int mx = req->a[0], my = req->a[1], buttons = req->a[2];
        if (buttons != g_prev_buttons || g_dragging >= 0) handle_mouse_transition(mx, my, buttons);
        g_prev_buttons = buttons;
        // Тот же найденный в первом срезе баг/фикс: курсор обязан
        // перерисовываться и на ЧИСТОЕ движение мыши, без смены кнопок —
        // но ТОЛЬКО курсор (g_cursor_moved), не вся сцена заново, см.
        // комментарий у g_content_dirty выше.
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

// Один логический "redraw()" клиента (gui_button_draw() и т.п.) — это
// обычно НЕСКОЛЬКО последовательных sys_win_draw_*()/sys_win_fill()
// вызовов подряд (каждый — свой синхронный RPC: клиент шлёт, блокируется,
// получает ответ, шлёт следующий). Раньше composite_scene() запускалась
// после КАЖДОГО такого вызова по отдельности — простая перерисовка
// текстового поля в несколько вызовов пересобирала весь экран несколько
// раз подряд (НАЙДЕННЫЙ БАГ: жалоба пользователя на загрузку CPU при
// печати). Недолго ждём (COALESCE_WAIT_MS) следующее сообщение той же
// пачки вместо того, чтобы пересобирать сцену на каждое из них.
#define COALESCE_WAIT_MS 20

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

    // Этап 4: рабочий стол (фон + ярлыки + таскбар) виден СРАЗУ, ещё до
    // первого sys_win_create() — это и есть то, через что теперь
    // запускается первое приложение (клик по ярлыку, см. "Ярлыки запуска
    // на рабочем столе" выше), а не набор команды в текстовой консоли.
    composite_scene();
    present_frame();
    g_content_dirty = 0;
    g_cursor_moved = 0;

    for (;;) {
        struct lufira_ipc_msg msg;
        if (sys_ipc_recv(&msg, -1) != 1) continue; // -1 — ждать первое сообщение пачки неограниченно
        dispatch_message(&msg);

        // Коалесцируем: пока сцена дирти и кто-то продолжает слать нам
        // сообщения почти сразу (COALESCE_WAIT_MS) — это, скорее всего,
        // следующий вызов ТОЙ ЖЕ клиентской перерисовки, обрабатываем его
        // тоже и откладываем пересборку сцены дальше. Как только пачка
        // иссякла (таймаут) или сцена не менялась (чистое движение мыши
        // между кликами) — выходим и, если нужно, рисуем один кадр.
        while (g_content_dirty && sys_ipc_recv(&msg, COALESCE_WAIT_MS) == 1)
            dispatch_message(&msg);

        if (g_content_dirty) {
            composite_scene();
            g_content_dirty = 0;
            present_frame();
        } else if (g_cursor_moved) {
            present_frame();
        }
        g_cursor_moved = 0;
    }
}
