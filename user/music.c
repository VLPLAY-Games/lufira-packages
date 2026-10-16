// music.c — перенос команды "music" (kernel/shell/commands/sound.c,
// command_music(), test_melody[] — теперь мёртвый код) в userspace, поверх
// SYS_SOUND_PLAY_PCM через общий common/sound_tone.h (используется и
// beep.c). Мелодия — те же ноты/длительности, что у kernel-native версии.
//
// Сборка — как у cpuload.c/free.c.

#include <stdio.h>
#include <lufira/syscall.h>
#include "../common/sound_tone.h"

typedef struct { unsigned int frequency, duration_ms; } sound_tone_t;

static const sound_tone_t test_melody[] = {
    { 262, 180 },   /* C4 */
    { 294, 180 },   /* D4 */
    { 330, 180 },   /* E4 */
    { 262, 180 },   /* C4 */

    { 262, 180 },   /* C4 */
    { 294, 180 },   /* D4 */
    { 330, 180 },   /* E4 */
    { 262, 250 },   /* C4 */

    { 330, 180 },   /* E4 */
    { 349, 180 },   /* F4 */
    { 392, 350 }    /* G4 */
};

int main(void) {
    printf("Playing test melody...\n");

    struct lufira_sound_info info;
    if (sys_sound_info(&info) != 0 || !info.available) {
        printf("Error: audio is not available.\n");
        return 1;
    }

    for (unsigned int i = 0; i < sizeof(test_melody) / sizeof(test_melody[0]); i++) {
        if (sound_play_tone_pcm(test_melody[i].frequency, test_melody[i].duration_ms) != 0) {
            printf("Error: failed at note %u.\n", i);
            sys_sound_stop();
            return 1;
        }
    }
    sys_sound_stop();

    printf("Done.\n");
    return 0;
}
