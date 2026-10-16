// assert.h — минимальная замена для порта doomgeneric (нужна только
// sha1.c). Ассерты отключены — это не отладочная сборка.
#pragma once
#define assert(cond) ((void)0)
