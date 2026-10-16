// sys/stat.h — минимальная замена для порта doomgeneric: m_misc.c берёт
// mkdir() отсюда (POSIX), для создания каталога сохранений/конфига.
// Обёртка над sys_mkdir() — реализация в compat.c.
#pragma once

int mkdir(const char *path, int mode);
