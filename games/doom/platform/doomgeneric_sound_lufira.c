// doomgeneric_sound_lufira.c — звуковой модуль doomgeneric под LufiraOS
// (аналог i_sdlsound.c/i_sdlmusic.c у апстрима — там SDL_mixer, здесь
// SYS_SOUND_PLAY_PCM). i_sound.c (вендор, см. games/doom/src/) ждёт ровно
// `sound_module_t DG_sound_module` + `music_module_t DG_music_module` при
// определённом FEATURE_SOUND (build.py: -DFEATURE_SOUND, чтобы не трогать
// вендорный doomfeatures.h) — сами DG_*-структуры апстрим НЕ поставляет,
// это платформенный код, как и doomgeneric_lufira.c.
//
// Музыка (MIDI/OPL) НЕ реализована — это отдельный синтезатор (GENMIDI/OPL
// или таймидити), на порядок больше работы, чем PCM-эффекты; DG_music_module
// ниже — честные no-op'ы (Init возвращает true, чтобы engine не ругался,
// но PlaySong ничего не проигрывает). Если понадобится — отдельная задача.
//
// SFX — да, полностью: один аппаратный PCM-канал (SYS_SOUND_PLAY_PCM,
// см. его комментарий в LufiraOS/kernel/system/syscall/syscall.h) не умеет
// проигрывать несколько наложенных друг на друга лампов одновременно сам —
// поэтому микшируем ПРОГРАММНО здесь: до MAX_CHANNELS логических каналов
// (DOOM реально использует snd_channels=8, см. s_sound.c), раз за кадр
// (I_UpdateSound() вызывается из S_UpdateSounds() в d_main.c — один раз на
// отрисованный кадр) считаем один слайс длиной SLICE_MS (28мс — то самое
// snd_maxslicetime_ms, уже объявленное в i_sound.c именно под эту схему:
// "один буфер на тик") суммированием всех активных каналов и одним
// sys_sound_play_pcm() отправляем в драйвер.
//
// На стороне AC'97 (sound_play_pcm_stereo() -> ac97_stream_write(), см.
// ac97.c) это НЕ одноразовый stop/reset/start на каждый слайс (тот давал
// слышимый щелчок на границе каждого кадра — "звук рваный"), а запись в
// непрерывно крутящееся кольцо: канал стартует один раз и больше не
// останавливается, вызов лишь подливает новые данные. Поэтому слайс шлём
// КАЖДЫЙ кадр, даже когда нет ни одного активного канала (g_mix_buf тогда
// просто тишина) — иначе кольцо продолжило бы доигрывать старые страницы
// по кругу вместо тишины.
//
// Ресемплинг: лампы DS* (DMX-формат) приходят каждая со своей частотой в
// заголовке (почти всегда 11025 Гц у doom1.wad) — играем их под частоту,
// которую УЖЕ выбрал активный аудио-драйвер (SYS_SOUND_INFO.sample_rate,
// обычно 48000 у AC'97 по умолчанию, см. ac97_init()), а не наоборот —
// отдельного SYS_SOUND_SET_SAMPLE_RATE не заводим: один syscall меньше,
// и глобальная частота устройства не меняется из-под других возможных
// потребителей звука. Фиксированная точка 16.16, ближайший сосед (не
// линейная интерполяция) — для коротких эффектов на слух неотличимо, а
// кода на порядок меньше.

#include "doomtype.h"
#include "deh_str.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

// i_sound.c::I_BindSoundVariables() ссылается на них под "#ifdef
// FEATURE_SOUND" (M_BindVariable — только персистентность в config-файл,
// сам ресемплинг libsamplerate у нас не используется — свой, см. шапку
// файла) — апстримный i_sdlsound.c определяет их же самым образом.
int use_libsamplerate = 0;
float libsamplerate_scale = 1.0f;

#include <lufira/syscall.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SOUND_CHANNELS 16
#define SLICE_MS 28 // == snd_maxslicetime_ms (i_sound.c)

typedef struct {
    int active;
    short *data;        // свой буфер, моно, 16 бит знаковых, нативная частота лампа
    uint32_t length;    // число сэмплов в data
    uint32_t pos;        // целая часть позиции воспроизведения
    uint32_t pos_frac;   // дробная часть, фиксированная точка 16.16
    uint32_t step;       // (lump_rate << 16) / device_rate
    int left_vol, right_vol; // 0..127
} sound_channel_t;

static sound_channel_t g_channels[MAX_SOUND_CHANNELS];

static int g_device_rate = 0;   // 0 — звук недоступен/не инициализирован
static short *g_mix_buf = NULL; // слайс: SLICE_FRAMES стерео-фреймов
static uint32_t g_slice_frames = 0;

// ===== Загрузка/конвертация одного DS*-лампа =====
//
// DMX raw PCM формат (все .lmp после "DS"): uint16 format (всегда 3),
// uint16 sample_rate, uint32 num_samples, затем num_samples байт 8-битного
// БЕЗ ЗНАКА PCM. (Классический DMX ещё добавляет по 16 сэмплов паддинга в
// начале/конце под анти-щелчковый фильтр исходного Allegro-микшера — не
// обрезаем их: на слух почти неразличимо, а код заметно проще.)
static int load_sfx(int lumpnum, short **out_data, uint32_t *out_length, uint32_t *out_rate) {
    if (lumpnum < 0) return 0;

    int lumplen = W_LumpLength((unsigned int)lumpnum);
    if (lumplen < 8) return 0;

    uint8_t *raw = (uint8_t *)W_CacheLumpNum(lumpnum, PU_CACHE);

    uint16_t rate = (uint16_t)(raw[2] | (raw[3] << 8));
    uint32_t nsamples = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8) |
                         ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 24);
    uint32_t max_samples = (uint32_t)(lumplen - 8);
    if (nsamples == 0 || nsamples > max_samples) nsamples = max_samples;
    if (nsamples == 0) return 0;

    short *data = (short *)malloc(nsamples * sizeof(short));
    if (!data) return 0;

    for (uint32_t i = 0; i < nsamples; i++) {
        data[i] = (short)(((int)raw[8 + i] - 128) * 256);
    }

    *out_data = data;
    *out_length = nsamples;
    *out_rate = rate ? rate : 11025;
    return 1;
}

static void free_channel(sound_channel_t *c) {
    if (c->data) free(c->data);
    memset(c, 0, sizeof(*c));
}

// ===== sound_module_t =====

static boolean Sound_Init(boolean use_sfx_prefix) {
    (void)use_sfx_prefix;

    struct lufira_sound_info info;
    if (sys_sound_info(&info) != 0 || !info.available || info.sample_rate == 0) return false;

    g_device_rate = (int)info.sample_rate;
    g_slice_frames = ((uint32_t)g_device_rate * SLICE_MS) / 1000;
    if (g_slice_frames == 0) g_slice_frames = 1;

    g_mix_buf = (short *)malloc((size_t)g_slice_frames * 2 * sizeof(short));
    if (!g_mix_buf) { g_device_rate = 0; return false; }

    memset(g_channels, 0, sizeof(g_channels));
    return true;
}

static void Sound_Shutdown(void) {
    sys_sound_stop(); // иначе AC'97-кольцо (ac97_stream_write(), ac97.c) крутится и после выхода
    for (int i = 0; i < MAX_SOUND_CHANNELS; i++) free_channel(&g_channels[i]);
    if (g_mix_buf) { free(g_mix_buf); g_mix_buf = NULL; }
    g_device_rate = 0;
}

static int Sound_GetSfxLumpNum(sfxinfo_t *sfxinfo) {
    char namebuf[16];
    const char *name = DEH_String(sfxinfo->name);
    namebuf[0] = 'd';
    namebuf[1] = 's';
    size_t i = 0;
    for (; name[i] && i < sizeof(namebuf) - 3; i++) namebuf[2 + i] = name[i];
    namebuf[2 + i] = '\0';
    return W_CheckNumForName(namebuf);
}

// Один слайс: суммируем активные каналы, продвигаем позиции, гасим
// дошедшие до конца — и шлём результат одним sys_sound_play_pcm().
static void Sound_Update(void) {
    if (g_device_rate == 0 || !g_mix_buf) return;

    memset(g_mix_buf, 0, (size_t)g_slice_frames * 2 * sizeof(short));

    for (int ci = 0; ci < MAX_SOUND_CHANNELS; ci++) {
        sound_channel_t *c = &g_channels[ci];
        if (!c->active) continue;

        for (uint32_t f = 0; f < g_slice_frames; f++) {
            if (c->pos >= c->length) { free_channel(c); break; }

            int32_t s = c->data[c->pos];
            int32_t l = g_mix_buf[f * 2 + 0] + (s * c->left_vol) / 127;
            int32_t r = g_mix_buf[f * 2 + 1] + (s * c->right_vol) / 127;
            if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
            if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
            g_mix_buf[f * 2 + 0] = (short)l;
            g_mix_buf[f * 2 + 1] = (short)r;

            c->pos_frac += c->step;
            c->pos += c->pos_frac >> 16;
            c->pos_frac &= 0xFFFF;
        }
    }

    sys_sound_play_pcm(g_mix_buf, g_slice_frames);
}

static void Sound_UpdateSoundParams(int channel, int vol, int sep) {
    if (channel < 0 || channel >= MAX_SOUND_CHANNELS) return;
    sound_channel_t *c = &g_channels[channel];
    if (!c->active) return;
    c->left_vol = (vol * (254 - sep)) / 254;
    c->right_vol = (vol * sep) / 254;
}

static int Sound_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    if (g_device_rate == 0) return -1;
    if (channel < 0 || channel >= MAX_SOUND_CHANNELS) return -1;

    sound_channel_t *c = &g_channels[channel];
    free_channel(c); // этот логический канал переиспользуется — старый буфер долой

    short *data;
    uint32_t length, lump_rate;
    if (!load_sfx(sfxinfo->lumpnum, &data, &length, &lump_rate)) return -1;

    c->data = data;
    c->length = length;
    c->pos = 0;
    c->pos_frac = 0;
    c->step = (uint32_t)(((uint64_t)lump_rate << 16) / (uint32_t)g_device_rate);
    c->left_vol = (vol * (254 - sep)) / 254;
    c->right_vol = (vol * sep) / 254;
    c->active = 1;

    return channel;
}

static void Sound_StopSound(int channel) {
    if (channel < 0 || channel >= MAX_SOUND_CHANNELS) return;
    free_channel(&g_channels[channel]);
}

static boolean Sound_SoundIsPlaying(int channel) {
    if (channel < 0 || channel >= MAX_SOUND_CHANNELS) return false;
    return g_channels[channel].active ? true : false;
}

// Доступны почти на всех "классических" звуковых путях Doom —
// SndDeviceInList() (i_sound.c) требует совпадения с snd_sfxdevice, а
// явного выбора устройства у нас нет (нет ни конфига, ни командной
// строки под это) — тот же набор, что у апстримного i_sdlsound.c.
static snddevice_t g_sound_devices[] = {
    SNDDEVICE_SB, SNDDEVICE_ADLIB, SNDDEVICE_PAS, SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_GENMIDI, SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    g_sound_devices,
    sizeof(g_sound_devices) / sizeof(g_sound_devices[0]),
    Sound_Init,
    Sound_Shutdown,
    Sound_GetSfxLumpNum,
    Sound_Update,
    Sound_UpdateSoundParams,
    Sound_StartSound,
    Sound_StopSound,
    Sound_SoundIsPlaying,
    NULL, // CacheSounds — не нужно, лампы грузятся лениво в load_sfx()
};

// ===== music_module_t — честные no-op'ы, см. шапку файла =====

static boolean Music_Init(void) { return true; }
static void Music_Shutdown(void) {}
static void Music_SetVolume(int volume) { (void)volume; }
static void Music_Pause(void) {}
static void Music_Resume(void) {}
static void *Music_RegisterSong(void *data, int len) { (void)data; (void)len; return (void *)1; }
static void Music_UnRegisterSong(void *handle) { (void)handle; }
static void Music_PlaySong(void *handle, boolean looping) { (void)handle; (void)looping; }
static void Music_StopSong(void) {}
static boolean Music_IsPlaying(void) { return false; }

music_module_t DG_music_module = {
    g_sound_devices,
    sizeof(g_sound_devices) / sizeof(g_sound_devices[0]),
    Music_Init,
    Music_Shutdown,
    Music_SetVolume,
    Music_Pause,
    Music_Resume,
    Music_RegisterSong,
    Music_UnRegisterSong,
    Music_PlaySong,
    Music_StopSong,
    Music_IsPlaying,
    NULL, // Poll — проверяется на NULL перед вызовом (i_sound.c), не нужен
};
