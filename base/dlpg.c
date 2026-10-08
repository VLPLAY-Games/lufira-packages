// dlpg.c — установщик пакетов LufiraOS (v0.7 план, этап 2). Base-пакет
// (сам является userspace ELF), но не требует НИ ОДНОГО нового syscall'а —
// вся логика (парсинг .lpg, проверка зависимостей, распаковка, БД
// установленных пакетов) поверх уже существующих OPEN/READ/WRITE/CLOSE/
// SEEK/CHMOD/MKDIR/UNLINK.
//
// Подкоманды:
//   dlpg install <path.lpg>   — установить новый пакет (отказ, если уже есть)
//   dlpg update  <path.lpg>   — установить/обновить (не отказывает, если уже есть)
//   dlpg list                 — показать установленные пакеты
//   dlpg remove  <name>       — удалить пакет и все его файлы
//   dlpg sync                 — скачать актуальный список пакетов с репозитория
//                                (REMOTE_INDEX_URL) и сохранить локально
//   dlpg upgrade [name]       — обновить один (name) или все установленные
//                                пакеты, у которых в синхронизированном
//                                списке версия новее локальной
//
// sync/upgrade — поверх SYS_NET_FETCH (новый syscall, см. его подробное
// описание в kernel/system/syscall/syscall.h): ОДИН блокирующий вызов
// скачивает URL целиком (DNS + TCP/TLS + разбор HTTP самим ядром,
// kernel/net/http_client.c) — dlpg.c никаких сокетов не открывает сам.
// index.json — НАСТОЯЩИЙ JSON (строится build_index.py в самом
// lufira-packages), но парсер ниже — НЕ общий JSON-парсер: он знает только
// ровно ту форму, которую сам же build_index.py всегда производит (плоский
// массив "packages" из объектов с простыми строковыми полями, без
// вложенных объектов/массивов внутри элемента) — этого достаточно и
// надёжно ровно потому, что формат этого файла контролируется тем же
// репозиторием, а не присылается откуда-то ещё в произвольном виде.
//
// БД установленных пакетов — /etc/packages/installed, построчно
// "имя:major.minor.patch:category" (тот же приём, что уже /etc/passwd,
// /etc/group — kernel/system/users/users.c). Список файлов каждого
// пакета — свой файл /etc/packages/<имя>.files (по одному абсолютному
// пути на строку), нужен только remove'у, чтобы знать, что удалять.
//
// Сборка (отличается от других userspace-программ ОДНИМ дополнительным
// -I — общий формат .lpg лежит в tools/, не в libc/include):
//   gcc $FLAGS -Itools -c userspace/base/dlpg.c -o dlpg.o
//   ld ... -o dlpg.elf crt0.o dlpg.o string.o malloc.o printf.o stdlib.o

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lufira/syscall.h>
#include "lpg_format.h"

#define INSTALLED_DB   "/etc/packages/installed"
#define PACKAGES_DIR   "/etc/packages"
#define MAX_INSTALLED  32

// Репозиторий пакетов — тот же, что у самого lufira-packages (см. его
// README). "refs/heads/main" (а не просто "main") — именно так его
// запросил пользователь; raw.githubusercontent.com понимает оба варианта
// одинаково, но так явнее видно, что это ветка, а не тег/коммит.
#define REMOTE_INDEX_URL \
    "https://raw.githubusercontent.com/VLPLAY-Games/lufira-packages/refs/heads/main/index.json"
#define REMOTE_INDEX_CACHE "/etc/packages/remote_index.json"
// Временный файл для скачанного .lpg перед do_install() — одно имя на всю
// систему достаточно: dlpg не бывает запущен параллельно сам с собой
// (однопользовательская ОС, одна интерактивная сессия шелла за раз).
#define DOWNLOAD_TMP_PATH  "/etc/packages/.download.lpg"
// index.json сегодня — десятки КБ (~40 пакетов), 256КБ — большой запас на
// будущий рост списка, не влияющий на типичную систему (буфер malloc'ится
// только на время самой команды, не держится постоянно).
#define INDEX_FETCH_CAP    (256u * 1024u)
// Крупнейший .lpg на сегодня — около 35КБ (wm.elf внутри); 2МБ — запас с
// большим отрывом под будущие более тяжёлые пакеты.
#define PACKAGE_FETCH_CAP  (2u * 1024u * 1024u)

typedef struct {
    char name[LPG_NAME_MAX];
    lpg_version_t version;
    uint8_t category;
} installed_entry_t;

/* ===================== мелкие ручные парсеры =====================
 * В этой freestanding-libc нет ни atoi(), ни sscanf() (см. libc/include/
 * stdlib.h) — те же ручные циклы по цифрам, что уже используются во всём
 * остальном ядре (kernel/lib/string.c: atoi()/hex_to_int()).
 */
static unsigned int parse_uint_local(const char *s, int *consumed) {
    unsigned int v = 0;
    int i = 0;
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (unsigned int)(s[i] - '0'); i++; }
    if (consumed) *consumed = i;
    return v;
}

static lpg_version_t parse_version_str(const char *s) {
    lpg_version_t v = {0, 0, 0};
    int c;
    v.major = (uint16_t)parse_uint_local(s, &c); s += c;
    if (*s == '.') s++;
    v.minor = (uint16_t)parse_uint_local(s, &c); s += c;
    if (*s == '.') s++;
    v.patch = (uint16_t)parse_uint_local(s, &c);
    return v;
}

// Режет line по ':' на месте (как split_fields() в users.c).
static int split_fields(char *line, char *fields[], int max_fields) {
    int count = 0;
    char *p = line;
    fields[count++] = p;
    while (*p && count < max_fields) {
        if (*p == ':') { *p = '\0'; p++; fields[count++] = p; }
        else p++;
    }
    return count;
}

/* ===================== файловые хелперы через syscalls ===================== */

static char *read_whole_file(const char *path, long *out_len) {
    long fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    long size = sys_lseek((int)fd, 0, SEEK_END);
    sys_lseek((int)fd, 0, SEEK_SET);
    if (size < 0) { sys_close((int)fd); return NULL; }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { sys_close((int)fd); return NULL; }

    long total = 0;
    while (total < size) {
        long n = sys_read((int)fd, buf + total, (unsigned long)(size - total));
        if (n <= 0) break;
        total += n;
    }
    sys_close((int)fd);
    buf[total] = '\0';
    if (out_len) *out_len = total;
    return buf;
}

static int write_whole_file(const char *path, const void *data, long len) {
    long fd = sys_open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return -1;
    long pos = 0;
    const char *p = (const char *)data;
    while (pos < len) {
        long n = sys_write((int)fd, p + pos, (unsigned long)(len - pos));
        if (n <= 0) { sys_close((int)fd); return -1; }
        pos += n;
    }
    sys_close((int)fd);
    return 0;
}

/* ===================== БД установленных пакетов ===================== */

// Возвращает число прочитанных записей (0, если файла ещё нет — свежая
// система без единого установленного пакета).
static int load_installed(installed_entry_t *out) {
    long len;
    char *content = read_whole_file(INSTALLED_DB, &len);
    if (!content) return 0;

    int count = 0;
    char *line = content;
    while (*line && count < MAX_INSTALLED) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (*line) {
            char *fields[3];
            if (split_fields(line, fields, 3) == 3) {
                memset(&out[count], 0, sizeof(out[count]));
                strncpy(out[count].name, fields[0], LPG_NAME_MAX - 1);
                out[count].version = parse_version_str(fields[1]);
                out[count].category = (strcmp(fields[2], "base") == 0) ? LPG_CATEGORY_BASE : LPG_CATEGORY_USER;
                count++;
            }
        }
        line = nl ? nl + 1 : line + strlen(line);
    }
    free(content);
    return count;
}

static int save_installed(installed_entry_t *entries, int count) {
    char buf[MAX_INSTALLED * 64];
    int pos = 0;
    for (int i = 0; i < count; i++) {
        int n = 0;
        // Собираем строку вручную (нет snprintf в этой freestanding libc) —
        // тот же приём, что уже у users.c/build_user_line().
        const char *cat = (entries[i].category == LPG_CATEGORY_BASE) ? "base" : "user";
        char line[128];
        // "name:major.minor.patch:category\n"
        int p2 = 0;
        const char *s = entries[i].name;
        while (*s && p2 < (int)sizeof(line) - 1) line[p2++] = *s++;
        line[p2++] = ':';

        unsigned int parts[3] = {entries[i].version.major, entries[i].version.minor, entries[i].version.patch};
        for (int k = 0; k < 3; k++) {
            char digits[8];
            int dn = 0;
            unsigned int v = parts[k];
            if (v == 0) digits[dn++] = '0';
            while (v > 0) { digits[dn++] = (char)('0' + v % 10); v /= 10; }
            while (dn > 0 && p2 < (int)sizeof(line) - 1) line[p2++] = digits[--dn];
            if (k < 2 && p2 < (int)sizeof(line) - 1) line[p2++] = '.';
        }
        line[p2++] = ':';
        s = cat;
        while (*s && p2 < (int)sizeof(line) - 1) line[p2++] = *s++;
        line[p2++] = '\n';
        line[p2] = '\0';
        n = p2;

        if (pos + n < (int)sizeof(buf)) {
            memcpy(buf + pos, line, (size_t)n);
            pos += n;
        }
    }
    return write_whole_file(INSTALLED_DB, buf, pos);
}

static int find_installed(installed_entry_t *entries, int count, const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) return i;
    }
    return -1;
}

/* ===================== install/update ===================== */

static int do_install(const char *lpg_path, int allow_existing) {
    long len;
    uint8_t *buf = (uint8_t *)read_whole_file(lpg_path, &len);
    if (!buf) { printf("dlpg: cannot open %s\n", lpg_path); return 1; }

    if (len < (long)sizeof(lpg_header_t)) { printf("dlpg: %s is too small to be a .lpg\n", lpg_path); free(buf); return 1; }

    lpg_header_t hdr;
    memcpy(&hdr, buf, sizeof(hdr));
    if (memcmp(hdr.magic, LPG_MAGIC, LPG_MAGIC_LEN) != 0) {
        printf("dlpg: %s is not a valid .lpg (bad magic)\n", lpg_path);
        free(buf);
        return 1;
    }
    if (hdr.dep_count > LPG_MAX_DEPS || hdr.file_count > LPG_MAX_FILES || hdr.file_count == 0) {
        printf("dlpg: %s has an invalid header\n", lpg_path);
        free(buf);
        return 1;
    }

    installed_entry_t installed[MAX_INSTALLED];
    int installed_count = load_installed(installed);

    int existing_idx = find_installed(installed, installed_count, hdr.name);
    if (existing_idx >= 0 && !allow_existing) {
        printf("dlpg: package '%s' is already installed (use 'update' to reinstall)\n", hdr.name);
        free(buf);
        return 1;
    }

    // Проверка зависимостей — ДО того, как что-либо распаковано на диск,
    // чтобы отказ не оставлял систему в частично установленном состоянии.
    lpg_dependency_t *deps = (lpg_dependency_t *)(buf + sizeof(lpg_header_t));
    for (uint32_t i = 0; i < hdr.dep_count; i++) {
        int dep_idx = find_installed(installed, installed_count, deps[i].name);
        if (dep_idx < 0) {
            printf("dlpg: missing dependency '%s'\n", deps[i].name);
            free(buf);
            return 1;
        }
        if (!lpg_version_gte(installed[dep_idx].version, deps[i].min_version)) {
            printf("dlpg: dependency '%s' too old (need >= %u.%u.%u, have %u.%u.%u)\n",
                   deps[i].name, deps[i].min_version.major, deps[i].min_version.minor, deps[i].min_version.patch,
                   installed[dep_idx].version.major, installed[dep_idx].version.minor, installed[dep_idx].version.patch);
            free(buf);
            return 1;
        }
    }

    lpg_file_entry_t *files = (lpg_file_entry_t *)(buf + sizeof(lpg_header_t) + hdr.dep_count * sizeof(lpg_dependency_t));

    // Распаковка + сборка receipt-файла (список установленных путей) для remove().
    char receipt[LPG_MAX_FILES * (LPG_PATH_MAX + 1)];
    int receipt_pos = 0;

    for (uint32_t i = 0; i < hdr.file_count; i++) {
        const uint8_t *data = buf + files[i].offset;
        if (write_whole_file(files[i].path, data, (long)files[i].size) != 0) {
            printf("dlpg: failed to write %s\n", files[i].path);
            free(buf);
            return 1;
        }
        sys_chmod(files[i].path, (int)files[i].mode);

        int plen = (int)strlen(files[i].path);
        if (receipt_pos + plen + 1 < (int)sizeof(receipt)) {
            memcpy(receipt + receipt_pos, files[i].path, (size_t)plen);
            receipt_pos += plen;
            receipt[receipt_pos++] = '\n';
        }
        printf("dlpg: installed %s (%u bytes)\n", files[i].path, files[i].size);
    }

    char receipt_path[LPG_NAME_MAX + 32];
    int rp = 0;
    const char *prefix = PACKAGES_DIR "/";
    while (prefix[rp]) { receipt_path[rp] = prefix[rp]; rp++; }
    int ni = 0;
    while (hdr.name[ni] && rp < (int)sizeof(receipt_path) - 8) receipt_path[rp++] = hdr.name[ni++];
    const char *suffix = ".files";
    int si = 0;
    while (suffix[si]) receipt_path[rp++] = suffix[si++];
    receipt_path[rp] = '\0';
    write_whole_file(receipt_path, receipt, receipt_pos);

    if (existing_idx >= 0) {
        installed[existing_idx].version = hdr.version;
        installed[existing_idx].category = hdr.category;
    } else if (installed_count < MAX_INSTALLED) {
        memset(&installed[installed_count], 0, sizeof(installed[installed_count]));
        strncpy(installed[installed_count].name, hdr.name, LPG_NAME_MAX - 1);
        installed[installed_count].version = hdr.version;
        installed[installed_count].category = hdr.category;
        installed_count++;
    } else {
        printf("dlpg: installed-package table full (max %d)\n", MAX_INSTALLED);
        free(buf);
        return 1;
    }
    save_installed(installed, installed_count);

    printf("dlpg: %s '%s' v%u.%u.%u\n", existing_idx >= 0 ? "updated" : "installed",
           hdr.name, hdr.version.major, hdr.version.minor, hdr.version.patch);
    free(buf);
    return 0;
}

/* ===================== list/remove ===================== */

static int do_list(void) {
    installed_entry_t installed[MAX_INSTALLED];
    int count = load_installed(installed);
    if (count == 0) { printf("No packages installed.\n"); return 0; }

    for (int i = 0; i < count; i++) {
        printf("%s %u.%u.%u (%s)\n", installed[i].name,
               installed[i].version.major, installed[i].version.minor, installed[i].version.patch,
               installed[i].category == LPG_CATEGORY_BASE ? "base" : "user");
    }
    return 0;
}

static int do_remove(const char *name) {
    installed_entry_t installed[MAX_INSTALLED];
    int count = load_installed(installed);

    int idx = find_installed(installed, count, name);
    if (idx < 0) { printf("dlpg: package '%s' is not installed\n", name); return 1; }

    char receipt_path[LPG_NAME_MAX + 32];
    int rp = 0;
    const char *prefix = PACKAGES_DIR "/";
    while (prefix[rp]) { receipt_path[rp] = prefix[rp]; rp++; }
    int ni = 0;
    while (name[ni] && rp < (int)sizeof(receipt_path) - 8) receipt_path[rp++] = name[ni++];
    const char *suffix = ".files";
    int si = 0;
    while (suffix[si]) receipt_path[rp++] = suffix[si++];
    receipt_path[rp] = '\0';

    long len;
    char *content = read_whole_file(receipt_path, &len);
    if (content) {
        char *line = content;
        while (*line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (*line) {
                sys_unlink(line);
                printf("dlpg: removed %s\n", line);
            }
            line = nl ? nl + 1 : line + strlen(line);
        }
        free(content);
    }
    sys_unlink(receipt_path);

    for (int i = idx; i < count - 1; i++) installed[i] = installed[i + 1];
    count--;
    save_installed(installed, count);

    printf("dlpg: removed package '%s'\n", name);
    return 0;
}

/* ===================== минимальный JSON для index.json =====================
 * См. комментарий в шапке файла — НЕ общий JSON-парсер, понимает только ту
 * конкретную плоскую форму, которую всегда производит build_index.py.
 */

// Первое вхождение needle в [hay, hay_end) (hay_end==NULL — до NUL). NULL,
// если не найдено. Нужен вместо strstr() — её нет в этой freestanding libc
// (string.h, см. libc/include/string.h).
static const char *find_sub(const char *hay, const char *hay_end, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return hay;
    const char *end = hay_end ? hay_end : hay + strlen(hay);
    for (const char *p = hay; p + nlen <= end; p++) {
        if (strncmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

// Указатель на символ сразу ПОСЛЕ открывающей '[' массива "packages", или
// NULL, если такого ключа в документе нет вовсе (битый/пустой индекс).
static const char *json_packages_array_start(const char *json) {
    const char *key = find_sub(json, NULL, "\"packages\"");
    if (!key) return NULL;
    const char *bracket = strchr(key, '[');
    return bracket ? bracket + 1 : NULL;
}

// *cursor — где-то внутри массива объектов (сразу после '[' или после
// предыдущего вызова). Находит следующий '{'...'}' (считает вложенность —
// на случай, если build_index.py когда-нибудь добавит вложенные поля),
// отдаёт его диапазон через obj_start/obj_end и продвигает *cursor за
// закрывающую '}'. Возвращает 0 на конце массива (встретили ']' раньше
// '{') или при явно битом JSON (не нашли закрывающую скобку вовсе).
static int json_next_object(const char **cursor, const char **obj_start, const char **obj_end) {
    const char *p = *cursor;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
    if (*p != '{') return 0;

    const char *start = p;
    int depth = 0;
    for (;; p++) {
        if (*p == '\0') return 0;
        if (*p == '{') depth++;
        else if (*p == '}') { depth--; if (depth == 0) { p++; break; } }
    }

    *obj_start = start;
    *obj_end = p;
    *cursor = p;
    return 1;
}

// Ищет строковое поле "key":"value" внутри [obj_start,obj_end) и копирует
// value в out (до out_cap-1 байт, NUL-terminated). Разворачивает только
// "\"" и "\\" — больше ничего в value этого индекса никогда не бывает
// (имена пакетов/версии/URL, см. build_index.py). 0 = успех, -1 = поле не
// найдено/значение не строка.
static int json_get_string(const char *obj_start, const char *obj_end, const char *key,
                            char *out, int out_cap) {
    char quoted_key[40];
    int qk = 0;
    quoted_key[qk++] = '"';
    for (const char *k = key; *k && qk < (int)sizeof(quoted_key) - 2; k++) quoted_key[qk++] = *k;
    quoted_key[qk++] = '"';
    quoted_key[qk] = '\0';

    const char *key_pos = find_sub(obj_start, obj_end, quoted_key);
    if (!key_pos) return -1;

    const char *p = key_pos + qk;
    while (p < obj_end && (*p == ' ' || *p == '\t')) p++;
    if (p >= obj_end || *p != ':') return -1;
    p++;
    while (p < obj_end && (*p == ' ' || *p == '\t')) p++;
    if (p >= obj_end || *p != '"') return -1;
    p++;

    int n = 0;
    while (p < obj_end && *p != '"') {
        char c = *p;
        if (c == '\\' && p + 1 < obj_end) { p++; c = *p; }
        if (n < out_cap - 1) out[n++] = c;
        p++;
    }
    out[n] = '\0';
    return (p < obj_end && *p == '"') ? 0 : -1;
}

/* ===================== sync/upgrade (SYS_NET_FETCH) ===================== */

static void print_fetch_error(const char *prefix, long code) {
    switch (code) {
        case NET_FETCH_EBADURL:  printf("%s: invalid URL\n", prefix); break;
        case NET_FETCH_EDNS:     printf("%s: DNS resolution failed\n", prefix); break;
        case NET_FETCH_ECONNECT: printf("%s: connection failed\n", prefix); break;
        case NET_FETCH_ETLS:     printf("%s: TLS handshake failed\n", prefix); break;
        case NET_FETCH_EHTTP:    printf("%s: malformed HTTP response\n", prefix); break;
        case NET_FETCH_ENOSPC:   printf("%s: response too large\n", prefix); break;
        case NET_FETCH_ENODEV:   printf("%s: no network device found\n", prefix); break;
        default:                 printf("%s: network error (%ld)\n", prefix, code); break;
    }
}

static int do_sync(void) {
    char *buf = (char *)malloc(INDEX_FETCH_CAP);
    if (!buf) { printf("dlpg: out of memory\n"); return 1; }

    int status = 0;
    // CAP-1 — оставляем место под собственный NUL-терминатор ниже
    // (sys_net_fetch() не NUL-terminate'ит сам — это сырые байты тела).
    long n = sys_net_fetch(REMOTE_INDEX_URL, buf, INDEX_FETCH_CAP - 1, &status);
    if (n < 0) {
        print_fetch_error("dlpg: sync", n);
        free(buf);
        return 1;
    }
    buf[n] = '\0';

    if (status != 200) {
        printf("dlpg: sync: server returned HTTP %d\n", status);
        free(buf);
        return 1;
    }

    if (write_whole_file(REMOTE_INDEX_CACHE, buf, n) != 0) {
        printf("dlpg: sync: failed to save %s\n", REMOTE_INDEX_CACHE);
        free(buf);
        return 1;
    }

    const char *cursor = json_packages_array_start(buf);
    int count = 0;
    if (cursor) {
        const char *os, *oe;
        while (json_next_object(&cursor, &os, &oe)) count++;
    }

    printf("dlpg: synced package index (%d package(s) available)\n", count);
    free(buf);
    return 0;
}

// only_name — NULL для "обновить всё установленное", иначе конкретный
// пакет. upgrade НИКОГДА не ставит пакет, которого ещё нет локально (это
// дело install/sync — см. комментарий в шапке файла) — только обновляет
// уже установленные.
static int do_upgrade(const char *only_name) {
    long idx_len;
    char *idx = read_whole_file(REMOTE_INDEX_CACHE, &idx_len);
    if (!idx) {
        printf("dlpg: no local package index — run 'dlpg sync' first\n");
        return 1;
    }

    installed_entry_t installed[MAX_INSTALLED];
    int installed_count = load_installed(installed);
    if (installed_count == 0) {
        printf("No packages installed.\n");
        free(idx);
        return 0;
    }

    const char *cursor = json_packages_array_start(idx);
    if (!cursor) {
        printf("dlpg: remote index malformed (no 'packages' array) — try 'dlpg sync' again\n");
        free(idx);
        return 1;
    }

    int checked = 0, upgraded = 0, failed = 0;
    const char *obj_start, *obj_end;
    while (json_next_object(&cursor, &obj_start, &obj_end)) {
        char name[LPG_NAME_MAX], version_str[32], lpg_url[256];
        if (json_get_string(obj_start, obj_end, "name", name, sizeof(name)) != 0) continue;
        if (only_name && strcmp(name, only_name) != 0) continue;
        if (json_get_string(obj_start, obj_end, "version", version_str, sizeof(version_str)) != 0) continue;
        if (json_get_string(obj_start, obj_end, "lpg", lpg_url, sizeof(lpg_url)) != 0) continue;

        int local_idx = find_installed(installed, installed_count, name);
        if (local_idx < 0) continue; // не установлен — не дело upgrade (см. комментарий выше)

        checked++;
        lpg_version_t remote_v = parse_version_str(version_str);
        if (!lpg_version_gt(remote_v, installed[local_idx].version)) continue; // уже актуален

        printf("dlpg: upgrading '%s' %u.%u.%u -> %u.%u.%u\n", name,
               installed[local_idx].version.major, installed[local_idx].version.minor,
               installed[local_idx].version.patch,
               remote_v.major, remote_v.minor, remote_v.patch);

        uint8_t *pkgbuf = (uint8_t *)malloc(PACKAGE_FETCH_CAP);
        if (!pkgbuf) { printf("dlpg: out of memory, skipping '%s'\n", name); failed++; continue; }

        int status = 0;
        long n = sys_net_fetch(lpg_url, pkgbuf, PACKAGE_FETCH_CAP, &status);
        if (n < 0) {
            print_fetch_error("dlpg: upgrade", n);
            free(pkgbuf);
            failed++;
            continue;
        }
        if (status != 200) {
            printf("dlpg: upgrade '%s': server returned HTTP %d\n", name, status);
            free(pkgbuf);
            failed++;
            continue;
        }

        if (write_whole_file(DOWNLOAD_TMP_PATH, pkgbuf, n) != 0) {
            printf("dlpg: upgrade '%s': failed to stage download\n", name);
            free(pkgbuf);
            failed++;
            continue;
        }
        free(pkgbuf);

        if (do_install(DOWNLOAD_TMP_PATH, 1) == 0) upgraded++; else failed++;
        sys_unlink(DOWNLOAD_TMP_PATH);

        // do_install() сам перечитал installed/installed_count с диска
        // внутри себя и сохранил обновлённую запись — наша локальная копия
        // installed[] выше теперь устарела для ЭТОГО пакета (версия), но
        // find_installed()/остальные ЕЩЁ не проверенные записи в ней не
        // трогаются этим изменением, так что перечитывать всю таблицу
        // заново посреди цикла не нужно.
    }

    free(idx);
    printf("dlpg: upgrade complete: %d checked, %d upgraded, %d failed\n", checked, upgraded, failed);
    return failed > 0 ? 1 : 0;
}

/* ===================== main ===================== */

static void print_usage(void) {
    printf("Usage: dlpg install <path.lpg>\n");
    printf("       dlpg update  <path.lpg>\n");
    printf("       dlpg list\n");
    printf("       dlpg remove  <name>\n");
    printf("       dlpg sync\n");
    printf("       dlpg upgrade [name]\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(); return 1; }

    // Игнорируем результат — успех, если директории ещё не было и она
    // создалась, успех и если она уже была (sys_mkdir() тогда вернёт
    // ошибку "уже существует", которую здесь незачем отдельно различать).
    sys_mkdir(PACKAGES_DIR, 0755);

    const char *subcmd = argv[1];
    if (strcmp(subcmd, "install") == 0 || strcmp(subcmd, "update") == 0) {
        if (argc < 3) { print_usage(); return 1; }
        return do_install(argv[2], strcmp(subcmd, "update") == 0);
    } else if (strcmp(subcmd, "list") == 0) {
        return do_list();
    } else if (strcmp(subcmd, "remove") == 0) {
        if (argc < 3) { print_usage(); return 1; }
        return do_remove(argv[2]);
    } else if (strcmp(subcmd, "sync") == 0) {
        return do_sync();
    } else if (strcmp(subcmd, "upgrade") == 0) {
        return do_upgrade(argc >= 3 ? argv[2] : NULL);
    }

    print_usage();
    return 1;
}
