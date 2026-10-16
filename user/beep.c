// beep.c — перенос команды "beep" (kernel/shell/commands/sound.c,
// command_beep(), теперь мёртвый код — см. комментарий в начале
// lufira-packages/shell/shell.c: "Network/sound всё ещё без syscall'а —
// не портированы") в userspace, поверх SYS_SOUND_PLAY_PCM — тон синтезирует
// общий common/sound_tone.h (используется и music.c).
//
// Сборка — как у cpuload.c/free.c.

#include <stdio.h>
#include <lufira/syscall.h>
#include "../common/sound_tone.h"

int main(void) {
    printf("Playing 440 Hz beep...\n");

    struct lufira_sound_info info;
    if (sys_sound_info(&info) != 0 || !info.available) {
        printf("Error: audio is not available.\n");
        return 1;
    }

    if (sound_play_tone_pcm(440, 250) != 0) {
        printf("Error: failed to play tone.\n");
        return 1;
    }
    sys_sound_stop();

    printf("Done.\n");
    return 0;
}
