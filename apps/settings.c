// settings.c — v0.9: системные настройки в одном окне с вкладками (Audio/
// Programs/Autostart/System), тот же стиль, что sysinfo.c/calc.c — plain
// SYS_WIN_*/gui_widgets.h, без отдельного "tabs"-виджета в libc.so (только
// это приложение их использует, заводить разделяемый виджет под единственного
// потребителя смысла не было — четыре gui_button_t в роли заголовков вкладок
// и ручное переключение, что рисовать).
#include <stdio.h>
#include <lufira/syscall.h>
#include <lufira/gui_widgets.h>

#define WIN_W 420
#define WIN_H 380

#define TAB_COUNT 4
#define TAB_AUDIO     0
#define TAB_PROGRAMS  1
#define TAB_AUTOSTART 2
#define TAB_SYSTEM    3

static const char *const TAB_LABELS[TAB_COUNT] = {"Audio", "Programs", "Autostart", "System"};
static gui_button_t g_tab_btn[TAB_COUNT];
static int g_active_tab = TAB_AUDIO;

#define TAB_ACTIVE_BG   0x3a6ea5
#define TAB_INACTIVE_BG 0x25252f
#define PANEL_BG        0x1a1a24
#define TEXT_FG         0xe8e8e8
#define TEXT_DIM        0x8a8a98
#define ACCENT_FG       0x7ef9a4

#define CONTENT_Y 46

/* ===================== мелкие хелперы форматирования =====================
 * Этот freestanding libc не даёт snprintf() (см. stdio.h) — только printf()
 * прямо в stdout, а виджетам нужен готовый char* буфер. Тот же ручной приём,
 * что в sysinfo.c/calc.c, просто один общий набор на все четыре вкладки. */
static void append_str(char *dst, int *pos, const char *src) {
    int i = 0;
    while (src[i]) dst[(*pos)++] = src[i++];
    dst[*pos] = '\0';
}

static void append_ulong(char *dst, int *pos, unsigned long v) {
    char digits[20]; int dn = 0;
    if (v == 0) digits[dn++] = '0';
    while (v > 0) { digits[dn++] = (char)('0' + v % 10); v /= 10; }
    while (dn > 0) dst[(*pos)++] = digits[--dn];
    dst[*pos] = '\0';
}

/* ======================================================================== */
/* Вкладка Audio — SYS_SOUND_INFO/SYS_SOUND_SET_VOLUME                      */
/* ======================================================================== */

static struct lufira_sound_info g_sound;
static gui_button_t g_vol_down_btn, g_vol_up_btn;

static void audio_refresh(void) {
    if (sys_sound_info(&g_sound) != 0) g_sound.available = 0;
}

static void audio_set_volume(int delta) {
    if (!g_sound.available) return;
    int v = (int)g_sound.volume + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    sys_sound_set_volume((unsigned int)v);
    audio_refresh();
}

static void audio_draw(int win) {
    if (!g_sound.available) {
        sys_win_draw_text(win, 16, CONTENT_Y + 10, "No audio device available.", TEXT_DIM);
        return;
    }

    char line[64]; int p;

    p = 0;
    append_str(line, &p, "Device: ");
    append_str(line, &p, g_sound.driver);
    sys_win_draw_text(win, 16, CONTENT_Y + 10, line, TEXT_FG);

    p = 0;
    append_str(line, &p, "Volume: ");
    append_ulong(line, &p, g_sound.volume);
    append_str(line, &p, "%");
    sys_win_draw_text(win, 16, CONTENT_Y + 34, line, TEXT_FG);

    // Полоска громкости — рамка + заполненная часть, пропорциональная
    // проценту (тот же "рамка 1px + внутренняя заливка" приём, что у
    // виджетов в gui_widgets.c). НАЙДЕНО ЖИВЫМ ТЕСТОМ (screendump): bar_y
    // изначально был выставлен ВЫШЕ строки "Device: ..." (56..64px) и
    // перекрывал её — сдвинута под строку "Volume: ..." (80..98px).
    int bar_x = 16, bar_y = CONTENT_Y + 52, bar_w = 300, bar_h = 18;
    sys_win_draw_rect(win, bar_x, bar_y, (unsigned)bar_w, (unsigned)bar_h, 0x4a4a55);
    sys_win_draw_rect(win, bar_x + 1, bar_y + 1, (unsigned)(bar_w - 2), (unsigned)(bar_h - 2), 0x0d0d12);
    int fill_w = (bar_w - 2) * (int)g_sound.volume / 100;
    if (fill_w > 0) sys_win_draw_rect(win, bar_x + 1, bar_y + 1, (unsigned)fill_w, (unsigned)(bar_h - 2), ACCENT_FG);

    gui_button_draw(win, &g_vol_down_btn);
    gui_button_draw(win, &g_vol_up_btn);
}

static int audio_click(int win, int x, int y) {
    if (gui_button_contains(&g_vol_down_btn, x, y)) { audio_set_volume(-5); return 1; }
    if (gui_button_contains(&g_vol_up_btn, x, y)) { audio_set_volume(5); return 1; }
    (void)win;
    return 0;
}

/* ======================================================================== */
/* Вкладка Programs — /etc/packages/installed (тот же формат, что dlpg.c:   */
/* "name:major.minor.patch:category", построчно)                           */
/* ======================================================================== */

#define INSTALLED_DB "/etc/packages/installed"

static gui_listbox_t g_pkg_list;
static char g_pkg_count_line[32];

static void programs_load(void) {
    gui_listbox_clear(&g_pkg_list);

    long fd = sys_open(INSTALLED_DB, O_RDONLY, 0);
    if (fd < 0) {
        int p = 0;
        append_str(g_pkg_count_line, &p, "0 packages installed");
        return;
    }

    long size = sys_lseek((int)fd, 0, SEEK_END);
    sys_lseek((int)fd, 0, SEEK_SET);
    if (size <= 0) { sys_close((int)fd); return; }

    static char buf[4096];
    long cap = (long)sizeof(buf) - 1;
    long total = 0;
    while (total < size && total < cap) {
        long n = sys_read((int)fd, buf + total, (unsigned long)(size - total < cap - total ? size - total : cap - total));
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    sys_close((int)fd);

    int count = 0;
    char *line = buf;
    while (*line) {
        char *nl = line;
        while (*nl && *nl != '\n') nl++;
        int had_nl = (*nl == '\n');
        if (had_nl) *nl = '\0';

        if (*line) {
            // "name:version:category" -> строка списка "name version"
            // (категория не показываем — Programs не разделяет base/user,
            // в отличие от сборщиковского GUI, который группирует по ней).
            char item[GUI_LISTBOX_ITEM_MAX];
            int ip = 0;
            char *colon1 = line;
            while (*colon1 && *colon1 != ':') colon1++;
            int name_len = (int)(colon1 - line);
            for (int i = 0; i < name_len && ip < GUI_LISTBOX_ITEM_MAX - 2; i++) item[ip++] = line[i];
            if (*colon1 == ':' && ip < GUI_LISTBOX_ITEM_MAX - 2) {
                item[ip++] = ' ';
                char *ver_start = colon1 + 1;
                char *colon2 = ver_start;
                while (*colon2 && *colon2 != ':') colon2++;
                for (char *c = ver_start; c < colon2 && ip < GUI_LISTBOX_ITEM_MAX - 1; c++) item[ip++] = *c;
            }
            item[ip] = '\0';
            gui_listbox_add_item(&g_pkg_list, item);
            count++;
        }

        line = had_nl ? nl + 1 : nl;
    }

    int p = 0;
    append_ulong(g_pkg_count_line, &p, (unsigned long)count);
    append_str(g_pkg_count_line, &p, " package(s) installed");
}

static void programs_draw(int win) {
    sys_win_draw_text(win, 16, CONTENT_Y + 2, g_pkg_count_line, TEXT_DIM);
    gui_listbox_draw(win, &g_pkg_list);
}

static int programs_click(int win, int x, int y) {
    (void)win;
    return gui_listbox_click(&g_pkg_list, x, y) >= 0;
}

/* ======================================================================== */
/* Вкладка Autostart — /etc/wm_autostart, тот же приём "наличие файла =      */
/* флаг включён", что уже использует devmode (/system/devmode.flag,         */
/* devmode_probe_early() в kernel.c) — содержимое файла не важно.           */
/* ======================================================================== */

#define WM_AUTOSTART_PATH "/etc/wm_autostart"

static gui_checkbox_t g_autostart_cb;

static int autostart_flag_present(void) {
    long fd = sys_open(WM_AUTOSTART_PATH, O_RDONLY, 0);
    if (fd < 0) return 0;
    sys_close((int)fd);
    return 1;
}

static void autostart_apply(int enabled) {
    if (enabled) {
        long fd = sys_open(WM_AUTOSTART_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0);
        if (fd >= 0) { sys_write((int)fd, "1\n", 2); sys_close((int)fd); }
    } else {
        sys_unlink(WM_AUTOSTART_PATH);
    }
}

static void autostart_draw(int win) {
    gui_checkbox_draw(win, &g_autostart_cb);
    sys_win_draw_text(win, 16, CONTENT_Y + 30,
                       "When on, /bin/shell.elf launches the", TEXT_DIM);
    sys_win_draw_text(win, 16, CONTENT_Y + 46,
                       "Window Manager in the background on", TEXT_DIM);
    sys_win_draw_text(win, 16, CONTENT_Y + 62,
                       "every new shell session.", TEXT_DIM);
}

static int autostart_click(int win, int x, int y) {
    (void)win;
    if (gui_checkbox_contains(&g_autostart_cb, x, y)) {
        autostart_apply(gui_checkbox_toggle(&g_autostart_cb));
        return 1;
    }
    return 0;
}

/* ======================================================================== */
/* Вкладка System — SYS_MEMINFO/SYS_STATFS/SYS_CPULOAD/SYS_GETTICK/SYS_PSLIST,*/
/* тот же набор, что sysinfo.c, плюс аптайм и число процессов               */
/* ======================================================================== */

static char g_mem_line[64];
static char g_disk_line[64];
static char g_cpu_line[32];
static char g_uptime_line[48];
static char g_proc_line[32];
static gui_button_t g_sys_refresh_btn;

static void system_sample(void) {
    struct lufira_meminfo mem;
    if (sys_meminfo(&mem) == 0) {
        unsigned long total_kb = (unsigned long)mem.total_pages * 4;
        unsigned long used_kb = (unsigned long)mem.used_pages * 4;
        int p = 0;
        append_str(g_mem_line, &p, "RAM: ");
        append_ulong(g_mem_line, &p, used_kb);
        append_str(g_mem_line, &p, " / ");
        append_ulong(g_mem_line, &p, total_kb);
        append_str(g_mem_line, &p, " KB");
    } else {
        g_mem_line[0] = '\0';
    }

    struct lufira_statfs sfs;
    if (sys_statfs(&sfs) == 0) {
        unsigned long used_kb = ((unsigned long)(sfs.total_blocks - sfs.free_blocks) * sfs.block_size) / 1024;
        unsigned long total_kb = ((unsigned long)sfs.total_blocks * sfs.block_size) / 1024;
        int p = 0;
        append_str(g_disk_line, &p, "Disk: ");
        append_ulong(g_disk_line, &p, used_kb);
        append_str(g_disk_line, &p, " / ");
        append_ulong(g_disk_line, &p, total_kb);
        append_str(g_disk_line, &p, " KB");
    } else {
        g_disk_line[0] = '\0';
    }

    struct lufira_cpuload before, after;
    if (sys_cpuload(&before) == 0) {
        sys_msleep(150);
        if (sys_cpuload(&after) == 0) {
            unsigned long total_delta = (unsigned long)(after.total_ticks - before.total_ticks);
            unsigned long idle_delta = (unsigned long)(after.idle_ticks - before.idle_ticks);
            unsigned long pct = total_delta ? (100 - (idle_delta * 100 / total_delta)) : 0;
            int p = 0;
            append_str(g_cpu_line, &p, "CPU: ");
            append_ulong(g_cpu_line, &p, pct);
            append_str(g_cpu_line, &p, "%");
        }
    }

    // SYS_GETTICK — сырые тики PIT (100 Гц, см. pit.c) с момента boot.
    long ticks = sys_gettick();
    if (ticks >= 0) {
        unsigned long secs = (unsigned long)ticks / 100;
        int p = 0;
        append_str(g_uptime_line, &p, "Uptime: ");
        append_ulong(g_uptime_line, &p, secs / 60);
        append_str(g_uptime_line, &p, "m ");
        append_ulong(g_uptime_line, &p, secs % 60);
        append_str(g_uptime_line, &p, "s");
    }

    static struct lufira_ps_entry ps_buf[64];
    long n = sys_pslist(ps_buf, 64);
    if (n >= 0) {
        int p = 0;
        append_str(g_proc_line, &p, "Processes: ");
        append_ulong(g_proc_line, &p, (unsigned long)n);
    }
}

static void system_draw(int win) {
    sys_win_draw_text(win, 16, CONTENT_Y + 6, g_mem_line, TEXT_FG);
    sys_win_draw_text(win, 16, CONTENT_Y + 26, g_disk_line, TEXT_FG);
    sys_win_draw_text(win, 16, CONTENT_Y + 46, g_cpu_line, TEXT_FG);
    sys_win_draw_text(win, 16, CONTENT_Y + 66, g_uptime_line, TEXT_FG);
    sys_win_draw_text(win, 16, CONTENT_Y + 86, g_proc_line, TEXT_FG);
    gui_button_draw(win, &g_sys_refresh_btn);
}

static int system_click(int win, int x, int y) {
    (void)win;
    if (gui_button_contains(&g_sys_refresh_btn, x, y)) { system_sample(); return 1; }
    return 0;
}

/* ======================================================================== */
/* Переключение вкладок + общий цикл окна                                   */
/* ======================================================================== */

static void tabs_init(void) {
    int x = 10;
    for (int i = 0; i < TAB_COUNT; i++) {
        gui_button_init(&g_tab_btn[i], x, 8, 95, 28, TAB_LABELS[i]);
        x += 95 + 5;
    }
}

static void tabs_restyle(void) {
    for (int i = 0; i < TAB_COUNT; i++)
        g_tab_btn[i].bg = (i == g_active_tab) ? TAB_ACTIVE_BG : TAB_INACTIVE_BG;
}

static void redraw(int win) {
    sys_win_fill(win, PANEL_BG);

    tabs_restyle();
    for (int i = 0; i < TAB_COUNT; i++) gui_button_draw(win, &g_tab_btn[i]);
    sys_win_draw_rect(win, 0, CONTENT_Y - 2, WIN_W, 1, 0x33333f);

    switch (g_active_tab) {
        case TAB_AUDIO:     audio_draw(win);     break;
        case TAB_PROGRAMS:  programs_draw(win);  break;
        case TAB_AUTOSTART: autostart_draw(win); break;
        case TAB_SYSTEM:    system_draw(win);    break;
    }
}

static int handle_tab_click(int x, int y) {
    for (int i = 0; i < TAB_COUNT; i++) {
        if (gui_button_contains(&g_tab_btn[i], x, y)) {
            g_active_tab = i;
            return 1;
        }
    }
    return 0;
}

int main(void) {
    int win = sys_win_create(220, 120, WIN_W, WIN_H, "Settings");
    if (win < 0) { printf("sys_win_create FAIL\n"); return 1; }

    tabs_init();

    audio_refresh();
    gui_button_init(&g_vol_down_btn, 16, CONTENT_Y + 80, 40, 30, "-5");
    gui_button_init(&g_vol_up_btn, 64, CONTENT_Y + 80, 40, 30, "+5");

    gui_listbox_init(&g_pkg_list, 16, CONTENT_Y + 18, WIN_W - 32, 260);
    programs_load();

    gui_checkbox_init(&g_autostart_cb, 16, CONTENT_Y + 6, "Launch GUI automatically");
    g_autostart_cb.checked = autostart_flag_present();

    system_sample();
    gui_button_init(&g_sys_refresh_btn, 16, CONTENT_Y + 106, 100, 28, "Refresh");

    redraw(win);

    int running = 1;
    while (running) {
        struct lufira_gui_event ev;
        while (sys_win_poll_event(win, &ev)) {
            if (ev.type == LUFIRA_GUI_EVENT_CLOSE) { running = 0; break; }
            if (ev.type == LUFIRA_GUI_EVENT_MOUSE_DOWN) {
                int changed = handle_tab_click(ev.x, ev.y);
                if (!changed) {
                    switch (g_active_tab) {
                        case TAB_AUDIO:     changed = audio_click(win, ev.x, ev.y);     break;
                        case TAB_PROGRAMS:  changed = programs_click(win, ev.x, ev.y);  break;
                        case TAB_AUTOSTART: changed = autostart_click(win, ev.x, ev.y); break;
                        case TAB_SYSTEM:    changed = system_click(win, ev.x, ev.y);    break;
                    }
                }
                if (changed) redraw(win);
            }
        }
        sys_msleep(16);
    }

    sys_win_destroy(win);
    return 0;
}
