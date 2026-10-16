// string.h — порт doomgeneric: добавляет strdup() (POSIX, не ANSI C —
// отсюда и не из stdlib.h) поверх настоящего LufiraOS libc/include/
// string.h. #include_next подключает настоящий за memcpy/strcpy/strcmp/
// strstr/strrchr и т.д.
#pragma once
#include_next <string.h>

char *strdup(const char *s);
int   strcasecmp(const char *a, const char *b);
int   strncasecmp(const char *a, const char *b, size_t n);
