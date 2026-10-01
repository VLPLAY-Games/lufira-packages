// color.c — вынос "color"/"colors" (kernel/shell/commands/colors.c,
// command_color()/command_colors(), мёртвый код) в отдельную userspace-
// программу (v0.7 план, этап 5, продолжение). "color <fg> [bg]" — только
// индексные hex-цвета, см. комментарий в fg.c про RGB. "color reset" —
// то же, что отдельная команда reset.elf (белый на чёрном).

#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>
#include "../common/console.h"

static int parse_hex_byte(const char *s, int *out) {
    int v = 0, n = 0;
    for (; *s; s++, n++) {
        if (n >= 2) return -1;
        char c = *s;
        if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        else return -1;
    }
    if (n == 0) return -1;
    *out = v;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: color <fg> [bg]   (hex 0-FF, or \"color reset\")\n");
        return 1;
    }

    if (strcmp(argv[1], "reset") == 0) {
        con_set_fg(CON_WHITE);
        con_set_bg(CON_BLACK);
        return 0;
    }

    int fg;
    if (parse_hex_byte(argv[1], &fg) != 0 || fg < 0 || fg > 255) {
        printf("color: invalid fg color (expected hex 0-FF)\n");
        return 1;
    }
    con_set_fg(fg);

    if (argc >= 3) {
        int bg;
        if (parse_hex_byte(argv[2], &bg) != 0 || bg < 0 || bg > 255) {
            printf("color: invalid bg color (expected hex 0-FF)\n");
            return 1;
        }
        con_set_bg(bg);
    }
    return 0;
}
