// stdlib.h — порт doomgeneric: добавляет atoi/atof/getenv/system поверх
// настоящего LufiraOS libc/include/stdlib.h (malloc/free/exit/strtol),
// которые тот сознательно не предоставляет (не нужны самой ОС). Этот
// заголовок идёт ПЕРВЫМ в -I, подключает настоящий stdlib.h через
// #include_next.
#pragma once
#include_next <stdlib.h>

int    atoi(const char *s);
double atof(const char *s);
int    abs(int x);

// calloc()/realloc() — LufiraOS libc сознательно их не даёт (см. его
// stdlib.h), но Doom (w_wad.c/w_checksum.c) их использует — реализованы
// в compat.c поверх malloc()/free()/malloc_usable_size().
void  *calloc(size_t nmemb, size_t size);
void  *realloc(void *ptr, size_t size);

// Всегда возвращает NULL — у этой ОС нет переменных окружения процесса.
// Doom использует getenv() только как необязательный override (DOOMWADDIR
// и т.п.) — NULL означает "не задано", ровно то же, что отсутствие entry
// в окружении на обычной ОС.
char *getenv(const char *name);

// Всегда "не удалось" (ненулевой результат) — у нас нет шелла, который
// system() мог бы запустить. Единственный вызывающий (i_system.c, попытка
// найти "zenity" для GUI-окна с ошибкой) при этом просто тихо переходит
// на вывод сообщения в консоль вместо popup'а.
int system(const char *command);
