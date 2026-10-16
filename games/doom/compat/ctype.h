// ctype.h — минимальная замена для порта doomgeneric. В ОС её вообще нет
// (LufiraOS/libc не предоставляет ctype.h) — Doom использует только
// isspace/isprint/toupper/tolower, этого достаточно.
#pragma once

static inline int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static inline int isprint(int c) {
    return c >= 0x20 && c < 0x7F;
}

static inline int toupper(int c) {
    return (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
}

static inline int tolower(int c) {
    return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c;
}
