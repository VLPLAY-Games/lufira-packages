// SDL_mixer.h — пустая заглушка. src/i_sound.c (вендор, не трогаем)
// подключает её только под "#if defined(FEATURE_SOUND) &&
// !defined(__DJGPP__)", но ничего из неё не использует — ни один Mix_*/
// SDL_*-символ в этом файле не встречается; FEATURE_SOUND там нужен лишь
// ради диспетчеризации на &DG_sound_module/&DG_music_module (см.
// platform/doomgeneric_sound_lufira.c), которые реализованы без SDL.
// compat/ первым в -I (см. его остальные заголовки) — эта заглушка просто
// перехватывает путь вместо падения на "No such file or directory".
#pragma once
