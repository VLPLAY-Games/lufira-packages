// compat.c — весь ОС-специфичный "недостающий ANSI C/POSIX" слой для
// порта doomgeneric: FILE* поверх sys_open/sys_read/sys_write/sys_lseek/
// sys_close (у LufiraOS libc есть только printf() и сырые syscall'ы, см.
// LufiraOS/libc/include/stdio.h), плюс getenv/system-заглушки (нет ни
// переменных окружения, ни шелла для system()), strdup/atoi/atof,
// mkdir()/remove() и урезанный sscanf() (ровно то подмножество форматов,
// что реально встречается в коде Doom — см. compat/stdio.h).
//
// atof() делает настоящую арифметику с double — единственная причина,
// по которой этот файл собирается БЕЗ -mgeneral-regs-only (см.
// games/doom build-правило в lufira-packages/build.py), как и
// g_game.c/i_video.c/m_config.c/v_video.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <lufira/syscall.h>

struct FILE {
    long fd;
    int  eof;
};

// "Консольный" FILE* — используется только для stdout/stderr (Doom
// печатает туда через fprintf(stderr,...) в основном при фатальных
// ошибках, I_Error). В этой ОС нет отдельного fd для stderr — всё идёт
// туда же, что и printf() (fd 1).
static struct FILE std_console = { 1, 0 };
FILE *stdin  = &std_console;
FILE *stdout = &std_console;
FILE *stderr = &std_console;

int errno = 0;

static int translate_mode(const char *mode, int *out_flags) {
    int has_plus = strchr(mode, '+') != NULL;

    if (mode[0] == 'r') {
        *out_flags = has_plus ? O_RDWR : O_RDONLY;
    } else if (mode[0] == 'w') {
        *out_flags = (has_plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        *out_flags = (has_plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    } else {
        return -1;
    }
    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    int flags;
    if (translate_mode(mode, &flags) != 0) return NULL;

    long fd = sys_open(path, flags, 0644);
    if (fd < 0) return NULL;

    FILE *f = malloc(sizeof(FILE));
    if (!f) {
        sys_close((int)fd);
        return NULL;
    }
    f->fd = fd;
    f->eof = 0;
    return f;
}

int fclose(FILE *f) {
    if (!f || f == &std_console) return 0;
    sys_close((int)f->fd);
    free(f);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || size == 0 || nmemb == 0) return 0;

    unsigned long want = (unsigned long)size * (unsigned long)nmemb;
    unsigned long got = 0;
    uint8_t *p = (uint8_t *)ptr;

    while (got < want) {
        long n = sys_read((int)f->fd, p + got, want - got);
        if (n <= 0) {
            if (n == 0) f->eof = 1;
            break;
        }
        got += (unsigned long)n;
    }
    return got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f) return 0;

    unsigned long want = (unsigned long)size * (unsigned long)nmemb;

    if (f == &std_console) {
        sys_write(1, ptr, want);
        return nmemb;
    }

    unsigned long sent = 0;
    const uint8_t *p = (const uint8_t *)ptr;
    while (sent < want) {
        long n = sys_write((int)f->fd, p + sent, want - sent);
        if (n <= 0) break;
        sent += (unsigned long)n;
    }
    return sent / size;
}

int fseek(FILE *f, long offset, int whence) {
    if (!f || f == &std_console) return -1;
    if (sys_lseek((int)f->fd, offset, whence) < 0) return -1;
    f->eof = 0;
    return 0;
}

long ftell(FILE *f) {
    if (!f || f == &std_console) return -1;
    return sys_lseek((int)f->fd, 0, SEEK_CUR);
}

int fflush(FILE *f) {
    (void)f;
    return 0; // не буферизуем — писать нечего
}

int fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    fwrite(&ch, 1, 1, f);
    return c;
}

int fputs(const char *s, FILE *f) {
    return (int)fwrite(s, 1, strlen(s), f);
}

char *fgets(char *s, int size, FILE *f) {
    if (!f || size <= 0) return NULL;

    int i = 0;
    while (i < size - 1) {
        char c;
        long n = sys_read((int)f->fd, &c, 1);
        if (n <= 0) {
            f->eof = 1;
            break;
        }
        s[i++] = c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n < 0) return n;
    size_t len = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
    fwrite(buf, 1, len, f);
    return (int)len;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int remove(const char *path) {
    return (sys_unlink(path) == 0) ? 0 : -1;
}

// Нет sys_rename() в этой ОС — делаем сами через sys_open/sys_read/
// sys_write/sys_unlink (копия + удаление оригинала). Единственный
// вызывающий (g_game.c: атомарная замена файла сохранения через временное
// имя) переживёт потерю атомарности — на единственном локальном процессе,
// пишущем в свой собственный savegame, гонки нет.
int rename(const char *oldpath, const char *newpath) {
    FILE *src = fopen(oldpath, "rb");
    if (!src) return -1;

    FILE *dst = fopen(newpath, "wb");
    if (!dst) {
        fclose(src);
        return -1;
    }

    char buf[4096];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = 0; break; }
    }
    fclose(src);
    fclose(dst);

    if (!ok) return -1;
    sys_unlink(oldpath);
    return 0;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int puts(const char *s) {
    fputs(s, stdout);
    fputc('\n', stdout);
    return 0;
}

int mkdir(const char *path, int mode) {
    return (sys_mkdir(path, mode) == 0) ? 0 : -1;
}

// Нет переменных окружения процесса в этой ОС — всегда "не задано".
// Единственные вызывающие (d_iwad.c: DOOMWADPATH/DOOMWADDIR, m_misc.c:
// TEMP) относятся к НЕОБЯЗАТЕЛЬНЫМ override'ам и благополучно откатываются
// на дефолты при NULL.
char *getenv(const char *name) {
    (void)name;
    return NULL;
}

// Нет шелла, который system() мог бы запустить — всегда "не удалось".
// Единственный вызывающий (i_system.c: проверка наличия "zenity" для
// GUI-попапа с ошибкой) просто тихо откатывается на вывод в консоль.
int system(const char *command) {
    (void)command;
    return -1;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (!copy) return NULL;
    memcpy(copy, s, len);
    return copy;
}

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

int abs(int x) {
    return x < 0 ? -x : x;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size; // вызовы в Doom малы (w_wad.c) — переполнение не грозит
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

// Реализован поверх malloc_usable_size() (LufiraOS libc, libc/src/
// malloc.c) — иначе неоткуда узнать, сколько байт старого блока реально
// копировать в новый (у этой libc нет собственного realloc(), см. его
// stdlib.h). ptr==NULL — то же самое, что malloc(); size==0 — то же,
// что free() с возвратом NULL (как и настоящий realloc()).
void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    size_t old_size = malloc_usable_size(ptr);
    void *new_ptr = malloc(size);
    if (!new_ptr) return NULL;

    size_t copy = (old_size < size) ? old_size : size;
    memcpy(new_ptr, ptr, copy);
    free(ptr);
    return new_ptr;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (a[i] == '\0') return 0;
    }
    return 0;
}

double atof(const char *s) {
    while (*s == ' ' || *s == '\t') s++;

    int neg = 0;
    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }

    double value = 0.0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10.0 + (double)(*s - '0');
        s++;
    }

    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            value += (double)(*s - '0') * frac;
            frac *= 0.1;
            s++;
        }
    }

    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_neg = 0;
        if (*s == '+' || *s == '-') {
            exp_neg = (*s == '-');
            s++;
        }
        int exp = 0;
        while (*s >= '0' && *s <= '9') {
            exp = exp * 10 + (*s - '0');
            s++;
        }
        while (exp-- > 0) value = exp_neg ? value / 10.0 : value * 10.0;
    }

    return neg ? -value : value;
}

// НЕ настоящий sscanf() — ровно то подмножество, что нужно Doom'у (см.
// comment в compat/stdio.h): один %-спецификатор (x/X/o/i/d) в конце
// строки формата, с необязательным литеральным префиксом перед ним
// (пробел в fmt пропускает пробелы/табы в str, остальные символы должны
// совпасть буквально).
int sscanf(const char *str, const char *fmt, ...) {
    while (*fmt && *fmt != '%') {
        if (*fmt == ' ') {
            while (*str == ' ' || *str == '\t') str++;
        } else {
            if (*str != *fmt) return 0;
            str++;
        }
        fmt++;
    }
    if (*fmt != '%') return 0;
    fmt++;

    int base;
    switch (*fmt) {
        case 'x':
        case 'X': base = 16; break;
        case 'o': base = 8; break;
        case 'i': base = 0; break;
        case 'd':
        default:  base = 10; break;
    }

    char *endptr;
    long value = strtol(str, &endptr, base);
    if (endptr == str) return 0;

    va_list ap;
    va_start(ap, fmt);
    int *out = va_arg(ap, int *);
    va_end(ap);

    *out = (int)value;
    return 1;
}
