// sound_tone.h — общий генератор тона для beep/music (user/), которого нет
// ни в libc, ни в ядре как отдельный syscall (SYS_SOUND_PLAY_PCM отдаёт
// только сырой PCM-буфер — см. его комментарий в kernel/system/syscall/
// syscall.h — сам тон раньше синтезировал kernel-native command_beep()/
// command_music(), kernel/shell/commands/sound.c, теперь мёртвый код: та же
// функциональность нужна здесь, в userspace).
//
// Треугольная волна — та же дешёвая целочисленная аппроксимация без libm,
// что уже у ac97_wave()/hda_wave() (kernel/drivers/sound/*.c) — звучит
// идентично тому, что слышно было бы через kernel-native beep.
#pragma once

#include <lufira/syscall.h>

static inline short sound_tone_wave(unsigned int phase) {
    unsigned short p = (unsigned short)(phase >> 16);
    int value;
    if (p < 16384) value = p;
    else if (p < 32768) value = 32768 - p;
    else if (p < 49152) value = -(int)(p - 32768);
    else value = -(int)(65536 - p);

    value *= 2;
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return (short)value;
}

#define SOUND_TONE_SLICE_FRAMES 1024

// Проигрывает один тон длиной duration_ms на частоте frequency через
// SYS_SOUND_PLAY_PCM — слайсами по SOUND_TONE_SLICE_FRAMES, как и
// doomgeneric-порт (games/doom/platform/doomgeneric_sound_lufira.c), только
// без микширования каналов (один-единственный, сам тон). 0 — успех,
// -1 — звук недоступен.
static inline int sound_play_tone_pcm(unsigned int frequency, unsigned int duration_ms) {
    struct lufira_sound_info info;
    if (sys_sound_info(&info) != 0 || !info.available || info.sample_rate == 0) return -1;
    if (frequency == 0 || duration_ms == 0) return 0;

    unsigned int rate = info.sample_rate;
    unsigned int total_frames = (rate * duration_ms) / 1000;
    if (total_frames == 0) total_frames = 1;

    short buf[SOUND_TONE_SLICE_FRAMES * 2];
    unsigned int phase = 0;
    unsigned long long step = ((unsigned long long)frequency << 32) / rate;

    unsigned int produced = 0;
    while (produced < total_frames) {
        unsigned int chunk = total_frames - produced;
        if (chunk > SOUND_TONE_SLICE_FRAMES) chunk = SOUND_TONE_SLICE_FRAMES;

        for (unsigned int i = 0; i < chunk; i++) {
            short s = sound_tone_wave(phase);
            buf[i * 2 + 0] = s;
            buf[i * 2 + 1] = s;
            phase += (unsigned int)step;
        }

        sys_sound_play_pcm(buf, chunk);
        produced += chunk;
    }

    return 0;
}
