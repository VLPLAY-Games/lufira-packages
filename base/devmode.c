// devmode.c — вынос "devmode" (kernel/shell/commands/system.c,
// command_devmode(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение). Тонкая обёртка над SYS_DEVMODE.

#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        long on = sys_devmode(0);
        printf("Developer mode: %s\n", on ? "ON" : "OFF");
        printf("Usage: devmode <on|off>\n");
        return 0;
    }

    if (strcmp(argv[1], "on") == 0) {
        if (sys_devmode(1) == 0) { printf("Developer mode: ON\n"); return 0; }
        printf("Failed to enable developer mode\n");
        return 1;
    }
    if (strcmp(argv[1], "off") == 0) {
        if (sys_devmode(2) == 0) { printf("Developer mode: OFF\n"); return 0; }
        printf("Failed to disable developer mode\n");
        return 1;
    }

    printf("Usage: devmode <on|off>\n");
    return 1;
}
