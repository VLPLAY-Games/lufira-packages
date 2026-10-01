// bg.c — то же самое, что fg.c, но для фона. См. комментарий там.

#include <stdio.h>
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
        printf("Usage: bg <color 0-FF hex>\n");
        return 1;
    }
    int color;
    if (parse_hex_byte(argv[1], &color) != 0 || color < 0 || color > 255) {
        printf("bg: invalid color (expected hex 0-FF)\n");
        return 1;
    }
    con_set_bg(color);
    return 0;
}
