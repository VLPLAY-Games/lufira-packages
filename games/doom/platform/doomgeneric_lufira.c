// doomgeneric_lufira.c — платформенный слой doomgeneric под LufiraOS
// (аналог doomgeneric_sdl.c/doomgeneric_xlib.c у апстрима). Реализует
// ровно 6 функций, которые требует doomgeneric.h: DG_Init/DG_DrawFrame/
// DG_SleepMs/DG_GetTicksMs/DG_GetKey/DG_SetWindowTitle — вся остальная
// игра (d_main.c и т.д.) платформы не знает вообще.
//
// Экран: DOOMGENERIC_RESX/RESY (см. CFLAGS в build.py) == SCREENWIDTH/
// SCREENHEIGHT (320x200) — родной i_video.c тогда работает с fb_scaling=1
// (без собственного масштабирования), а реальное увеличение под
// разрешение экрана (которое заранее не известно — зависит от GOP/VESA
// на конкретной машине) делаем здесь сами: целочисленный масштаб (nearest
// neighbor), отцентрировано, в один РЕАЛЬНОГО размера экрана буфер,
// который целиком идёт в sys_fb_present().
//
// Управление экраном: ОДИН oконный сервер на систему (sys_wm_register()).
// Если его ещё никто не занял (например, запуск doom.elf прямо из шелла,
// без wm.elf) — регистрируемся сами. Если уже кто-то владеет (сценарий
// "полноэкранного захвата": ярлык в wm.c сам себя exec'ает поверх в
// doom.elf, PID не меняется, регистрация остаётся с ним) — повторный
// sys_wm_register() вернёт -1, но это НЕ ошибка: экран и так уже наш
// (тот же самый PID), просто игнорируем результат.
#include "doomgeneric.h"
#include "doomkeys.h"

#include <lufira/syscall.h>
#include <lufira/wm_protocol.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_screen_w = 0, g_screen_h = 0, g_pixel_format = 0;
static uint32_t *g_native_buf = NULL;
static int g_scale = 1;
static int g_off_x = 0, g_off_y = 0;

// Та же формула, что convert_color() в LufiraOS/kernel/drivers/console/
// console.c и wm_convert_color() в lufira-packages/apps/wm.c — обе эти
// копии живут порознь по той же причине, что и здесь: ядро отдаёт
// pixel_format через SYS_FB_INFO, но саму конвертацию делает каждый
// презентующий процесс сам (см. комментарий у convert_color() в console.c).
static uint32_t convert_color(uint32_t color) {
    if (g_pixel_format == 1) return color;
    uint8_t r = (uint8_t)(color >> 16);
    uint8_t g = (uint8_t)(color >> 8);
    uint8_t b = (uint8_t)color;
    return ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

// ===== Очередь клавиш =====
// DG_GetKey() вызывается из I_GetEvent() (i_video.c) в цикле "пока
// возвращает 1" РАЗ В ТИК — сама же очередь наполняется из poll_input(),
// который мы зовём из DG_DrawFrame() (тоже раз в тик, см. I_FinishUpdate()
// в i_video.c — она сама вызывает DG_DrawFrame() в конце).
#define KEYQ_SIZE 64
typedef struct { unsigned char key; int pressed; } keyq_entry_t;
static keyq_entry_t g_keyq[KEYQ_SIZE];
static int g_keyq_head = 0, g_keyq_tail = 0;

static void keyq_push(unsigned char key, int pressed) {
    int next = (g_keyq_tail + 1) % KEYQ_SIZE;
    if (next == g_keyq_head) return; // переполнение — молча роняем
    g_keyq[g_keyq_tail].key = key;
    g_keyq[g_keyq_tail].pressed = pressed;
    g_keyq_tail = next;
}

// Переводит код клавиши LufiraOS (ASCII для печатных + KEY_LEFT_ARROW=1/
// RIGHT=2/UP=3/DOWN=4/CTRL_C=5, см. kernel/drivers/keyboard/keyboard.h)
// в код, который ждёт doomkeys.h. Модификаторы (Shift/Ctrl/Alt сами по
// себе, без другой клавиши) эта ОС в принципе не доставляет как отдельное
// событие (см. keyboard.c — они только меняют внутреннее состояние) —
// поэтому "огонь"/"использовать" у нас НЕ Ctrl/Space, как в ванильном
// Doom, а Space/Enter: единственные гарантированно доставляемые клавиши,
// не занятые под движение. Возвращает 0, если клавиша не нужна игре.
static int translate_key(int raw, unsigned char *out) {
    switch (raw) {
        case 1: *out = KEY_LEFTARROW;  return 1;
        case 2: *out = KEY_RIGHTARROW; return 1;
        case 3: *out = KEY_UPARROW;    return 1;
        case 4: *out = KEY_DOWNARROW;  return 1;
        case 5: return 0; // Ctrl+C — не часть игрового ввода
        case 27: *out = KEY_ESCAPE;    return 1;
        case '\n': *out = KEY_ENTER;    return 1; // доставляется '\n'(10), Doom ждёт 13
        case '\t': *out = KEY_TAB;      return 1;
        case '\b': *out = KEY_BACKSPACE; return 1; // доставляется 8, Doom ждёт 0x7f
        case ' ':  *out = KEY_FIRE;     return 1; // см. комментарий выше
        default:
            if (raw >= 1 && raw <= 255) { *out = (unsigned char)raw; return 1; }
            return 0;
    }
}

static void poll_input(void) {
    struct lufira_ipc_msg msg;
    while (sys_ipc_recv(&msg, 0) == 1) {
        if (msg.sender_pid != WM_SENDER_KERNEL) continue; // клиентских окон у нас нет
        const struct wm_request *req = (const struct wm_request *)msg.data;
        if (req->opcode != WM_INPUT_KEY) continue; // мышь/WM_NOTIFY_PROCESS_EXIT — не нужны

        unsigned char doom_key;
        if (translate_key(req->a[0], &doom_key)) {
            keyq_push(doom_key, req->a[1]);
        }
    }
}

void DG_Init(void) {
    sys_wm_register(); // см. комментарий в шапке файла — ошибку игнорируем специально

    struct lufira_fb_info info;
    if (sys_fb_info(&info) == 0 && info.width > 0 && info.height > 0) {
        g_screen_w = info.width;
        g_screen_h = info.height;
        g_pixel_format = info.pixel_format;
    } else {
        // Крайний случай (SYS_FB_INFO не удался) — хотя бы нативное 1:1.
        g_screen_w = DOOMGENERIC_RESX;
        g_screen_h = DOOMGENERIC_RESY;
        g_pixel_format = 1;
    }

    g_scale = (int)(g_screen_w / DOOMGENERIC_RESX);
    int scale_y = (int)(g_screen_h / DOOMGENERIC_RESY);
    if (scale_y < g_scale) g_scale = scale_y;
    if (g_scale < 1) g_scale = 1;

    g_off_x = (int)(g_screen_w - (uint32_t)(DOOMGENERIC_RESX * g_scale)) / 2;
    g_off_y = (int)(g_screen_h - (uint32_t)(DOOMGENERIC_RESY * g_scale)) / 2;
    if (g_off_x < 0) g_off_x = 0;
    if (g_off_y < 0) g_off_y = 0;

    g_native_buf = (uint32_t *)malloc((size_t)g_screen_w * g_screen_h * 4);
    if (g_native_buf) {
        memset(g_native_buf, 0, (size_t)g_screen_w * g_screen_h * 4);
    }
}

void DG_DrawFrame(void) {
    if (!DG_ScreenBuffer || !g_native_buf) return;

    for (int y = 0; y < DOOMGENERIC_RESY; y++) {
        const uint32_t *src_row = &DG_ScreenBuffer[y * DOOMGENERIC_RESX];
        for (int sy = 0; sy < g_scale; sy++) {
            uint32_t *dst_row = g_native_buf +
                (size_t)(g_off_y + y * g_scale + sy) * g_screen_w + (size_t)g_off_x;
            for (int x = 0; x < DOOMGENERIC_RESX; x++) {
                uint32_t c = convert_color(src_row[x]);
                int base = x * g_scale;
                for (int sx = 0; sx < g_scale; sx++) {
                    dst_row[base + sx] = c;
                }
            }
        }
    }

    sys_fb_present(g_native_buf, g_screen_w, g_screen_h);

    poll_input();
}

void DG_SleepMs(uint32_t ms) {
    sys_msleep(ms);
}

// PIT — 100 Гц (kernel/system/timer/pit.h, PIT_FREQUENCY=100) — 1 тик = 10мс.
uint32_t DG_GetTicksMs(void) {
    return (uint32_t)(sys_gettick() * 10);
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    if (g_keyq_head == g_keyq_tail) return 0;
    *pressed = g_keyq[g_keyq_head].pressed;
    *doomKey = g_keyq[g_keyq_head].key;
    g_keyq_head = (g_keyq_head + 1) % KEYQ_SIZE;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    (void)title; // нет оконного заголовка — полноэкранный захват
}

int main(void) {
    // -iwad с абсолютным путём — D_FindWADByName() (d_iwad.c) проверяет
    // M_FileExists(name) ПЕРВЫМ делом и возвращает его как есть при успехе,
    // так и не доходя до автопоиска по DOOMWADPATH/DOOMWADDIR/getenv() —
    // ни того, ни другого у этой ОС всё равно нет.
    static char *doom_argv[] = { "doom", "-iwad", "/usr/share/doom1.wad", NULL };
    doomgeneric_Create(3, doom_argv);

    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
