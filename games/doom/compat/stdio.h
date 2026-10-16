// stdio.h — порт doomgeneric поверх LufiraOS libc, которая сама по себе
// не предоставляет файловых потоков (см. LufiraOS/libc/include/stdio.h:
// "Никаких файловых потоков/scanf... файлы это sys_open/sys_read/...").
// Doom (m_misc.c/m_config.c/p_saveg.c/w_file_stdc.c) наоборот рассчитывает
// на самый обычный ANSI C FILE* — fopen/fread/fwrite/fseek/ftell/fclose,
// плюс fprintf/vfprintf/fputc/fputs/fgets и sscanf (ограниченное
// подмножество, см. compat.c). Этот заголовок идёт ПЕРВЫМ в -I (раньше
// настоящего libc/include), добавляет всё перечисленное сверху и уже
// ЗАТЕМ подключает настоящий stdio.h (#include_next) за printf()/
// vsnprintf()/snprintf().
#pragma once
#include_next <stdio.h>

#include <stddef.h>
#include <stdarg.h>

typedef struct FILE FILE;

#ifndef EOF
#define EOF (-1)
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
int    fflush(FILE *f);
int    fputc(int c, FILE *f);
int    fputs(const char *s, FILE *f);
char  *fgets(char *s, int size, FILE *f);
int    fprintf(FILE *f, const char *fmt, ...);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    remove(const char *path);
int    rename(const char *oldpath, const char *newpath);
int    putchar(int c);
int    puts(const char *s);

// ТОЛЬКО то подмножество, что реально вызывается в m_misc.c/m_config.c:
// разбор ровно одного целого из начала строки — "[ 0]0x%x", "[ 0]0X%x",
// "[ 0]0%o", "[ 0]%d", "%x", "%i" (см. compat.c). Не настоящий sscanf.
int sscanf(const char *str, const char *fmt, ...);
