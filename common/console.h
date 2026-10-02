// console.h — конструирует управляющие последовательности /dev/console
// (см. console_write() в kernel/fs/vfs/vfs.c) — v0.7 план, этап 5,
// под-этап 6. НЕ настоящий ANSI (эта ОС не обещает совместимости с
// внешними терминалами) — свой простой формат: ESC (0x1B) + однобайтовая
// команда. Каждая функция здесь делает ОДИН sys_write() — ядро не хранит
// состояние парсера между вызовами, так что последовательность обязана
// целиком укладываться в один write().
#pragma once

#include <lufira/syscall.h>

static inline void con_set_fg(int color) {
    char seq[3] = {0x1B, 'f', (char)color};
    sys_write(1, seq, 3);
}

static inline void con_set_bg(int color) {
    char seq[3] = {0x1B, 'b', (char)color};
    sys_write(1, seq, 3);
}

static inline void con_set_cursor(unsigned int x, unsigned int y) {
    char seq[6] = {0x1B, 'p',
                   (char)((x >> 8) & 0xFF), (char)(x & 0xFF),
                   (char)((y >> 8) & 0xFF), (char)(y & 0xFF)};
    sys_write(1, seq, 6);
}

static inline void con_clear(void) {
    char seq[2] = {0x1B, 'c'};
    sys_write(1, seq, 2);
}

// Двигают сам курсор-подчёркивание на одну позицию, НЕ трогая текст на
// экране (в отличие от raw '\b', который console_putchar() в kernel/fs/
// vfs/vfs.c трактует как "стереть символ слева" — тот путь годится для
// Backspace, но не для стрелок влево/вправо).
static inline void con_cursor_left(void) {
    char seq[2] = {0x1B, 'l'};
    sys_write(1, seq, 2);
}

static inline void con_cursor_right(void) {
    char seq[2] = {0x1B, 'r'};
    sys_write(1, seq, 2);
}

// Индексы палитры (16 цветов) — те же значения, что ConsoleColor
// (kernel/lib/colors.h), для программ, которым не хочется тащить весь
// заголовок ядра только за одним enum'ом.
#define CON_BLACK         0
#define CON_BLUE          1
#define CON_GREEN         2
#define CON_CYAN          3
#define CON_RED           4
#define CON_MAGENTA       5
#define CON_BROWN         6
#define CON_LIGHT_GRAY    7
#define CON_DARK_GRAY     8
#define CON_LIGHT_BLUE    9
#define CON_LIGHT_GREEN   10
#define CON_LIGHT_CYAN    11
#define CON_LIGHT_RED     12
#define CON_LIGHT_MAGENTA 13
#define CON_YELLOW        14
#define CON_WHITE         15
