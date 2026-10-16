// mixer.c — перенос команды "mixer" (kernel/shell/commands/sound.c,
// command_mixer(), теперь мёртвый код) в userspace, поверх SYS_SOUND_INFO/
// SYS_SOUND_SET_VOLUME.
//
// Сборка — как у cpuload.c/free.c.

#include <stdio.h>
#include <lufira/syscall.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        struct lufira_sound_info info;
        if (sys_sound_info(&info) != 0 || !info.available) {
            printf("Audio Mixer\n");
            printf("-----------\n");
            printf("Device : none\n");
            printf("State  : offline\n");
            return 0;
        }

        printf("Audio Mixer\n");
        printf("-----------\n");
        printf("Device : %s\n", info.driver);
        printf("State  : ready\n");
        printf("Volume : %u%%\n", info.volume);
        printf("Rate   : %u Hz\n", info.sample_rate);
        return 0;
    }

    unsigned int value = 0;
    const char *p = argv[1];
    while (*p >= '0' && *p <= '9') { value = value * 10 + (unsigned int)(*p - '0'); p++; }

    if (p == argv[1] || *p != '\0') {
        printf("Usage: mixer [0-100]\n");
        return 1;
    }
    if (value > 100) {
        printf("Volume must be between 0 and 100.\n");
        return 1;
    }

    if (sys_sound_set_volume(value) != 0) {
        printf("Error: audio is not available.\n");
        return 1;
    }

    printf("Volume set to %u%%\n", value);
    return 0;
}
