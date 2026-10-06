// sysinfo.c — v0.8 (GUI+WM), этап 4: окно с информацией о системе (память,
// диск, загрузка CPU) поверх уже существующих SYS_MEMINFO/SYS_STATFS/
// SYS_CPULOAD — тот же набор данных, что у userspace/user/free.c, df.c,
// cpuload.c, просто в окне вместо текстового вывода. "Refresh" берёт новый
// снимок (CPU% требует двух снимков с паузой между ними — sys_cpuload()
// сэмплируется заново при каждом нажатии, тот же приём, что у cpuload.c).
#include <stdio.h>
#include <lufira/syscall.h>
#include <lufira/gui_widgets.h>

static gui_button_t g_btn_refresh;

static char g_mem_line[64];
static char g_disk_line[64];
static char g_cpu_line[32];

static void format_ulong(char *buf, unsigned long v) {
    int n = 0;
    char digits[20]; int dn = 0;
    if (v == 0) digits[dn++] = '0';
    while (v > 0) { digits[dn++] = (char)('0' + v % 10); v /= 10; }
    while (dn > 0) buf[n++] = digits[--dn];
    buf[n] = '\0';
}

static void append_str(char *dst, int *pos, const char *src) {
    int i = 0;
    while (src[i]) dst[(*pos)++] = src[i++];
    dst[*pos] = '\0';
}

static void append_ulong(char *dst, int *pos, unsigned long v) {
    char tmp[20];
    format_ulong(tmp, v);
    append_str(dst, pos, tmp);
}

static void sample(void) {
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
        sys_msleep(200);
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
}

static void redraw(int win) {
    sys_win_fill(win, 0x1a1a24);
    sys_win_draw_text(win, 12, 16, g_mem_line, 0xe8e8e8);
    sys_win_draw_text(win, 12, 36, g_disk_line, 0xe8e8e8);
    sys_win_draw_text(win, 12, 56, g_cpu_line, 0xe8e8e8);
    gui_button_draw(win, &g_btn_refresh);
}

int main(void) {
    int win = sys_win_create(260, 200, 260, 130, "Sys Info");
    if (win < 0) { printf("sys_win_create FAIL\n"); return 1; }

    gui_button_init(&g_btn_refresh, 12, 86, 100, 28, "Refresh");
    sample();
    redraw(win);

    int running = 1;
    while (running) {
        struct lufira_gui_event ev;
        while (sys_win_poll_event(win, &ev)) {
            if (ev.type == LUFIRA_GUI_EVENT_CLOSE) { running = 0; break; }
            if (ev.type == LUFIRA_GUI_EVENT_MOUSE_DOWN) {
                if (gui_button_contains(&g_btn_refresh, ev.x, ev.y)) {
                    sample();
                    redraw(win);
                }
            }
        }
        sys_msleep(16);
    }

    sys_win_destroy(win);
    return 0;
}
