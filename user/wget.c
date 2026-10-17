// wget.c — перенос команды "wget" (kernel/shell/commands/net.c,
// command_wget(), теперь мёртвый код) в userspace. Раньше это был
// собственный минимальный блокирующий TCP-клиент прямо в ядре (tcp_connect/
// tcp_send/tcp_recv, ручной парсинг заголовков) — здесь всё это уже делает
// SYS_NET_FETCH (sys_net_fetch(), HTTP ЦЕЛИКОМ в ядре, см. его комментарий
// в kernel-side syscall.h): http_client.c сам пробует ip_str_to_addr() на
// хосте ДО dns_resolve() (net.c), так что "wget <ip> <path>" без DNS — то
// самое поведение kernel-native версии — получается БЕСПЛАТНО, просто
// собрав URL "http://<ip><path>" и один раз сходив sys_net_fetch().
//
// Сборка — как у cpuload.c/free.c.

#include <stdio.h>
#include <stdlib.h>
#include <lufira/syscall.h>
#include "../common/ip_addr.h"

// Крупнейшее, что реально качают этим инструментом (страницы/мелкие
// файлы) — тот же порядок, что PACKAGE_FETCH_CAP в dlpg.c.
#define WGET_FETCH_CAP (2u * 1024u * 1024u)

static void print_fetch_error(long code) {
    switch (code) {
        case NET_FETCH_EBADURL:  printf("wget: invalid URL\n"); break;
        case NET_FETCH_EDNS:     printf("wget: DNS resolution failed\n"); break;
        case NET_FETCH_ECONNECT: printf("wget: connection failed\n"); break;
        case NET_FETCH_ETLS:     printf("wget: TLS handshake failed\n"); break;
        case NET_FETCH_EHTTP:    printf("wget: malformed HTTP response\n"); break;
        case NET_FETCH_ENOSPC:   printf("wget: response too large\n"); break;
        case NET_FETCH_ENODEV:   printf("wget: no network device found\n"); break;
        default:                 printf("wget: network error (%ld)\n", code); break;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: wget <ip> <path> [output-filename]\n");
        return 1;
    }

    unsigned int ip;
    if (ip_addr_parse(argv[1], &ip) != 0) {
        printf("wget: invalid IP address\n");
        return 1;
    }

    const char *path = argv[2];

    char out_name[64];
    if (argc >= 4) {
        int n = 0;
        while (argv[3][n] && n < (int)sizeof(out_name) - 1) { out_name[n] = argv[3][n]; n++; }
        out_name[n] = '\0';
    } else {
        const char *last_slash = 0;
        for (const char *s = path; *s; s++) if (*s == '/') last_slash = s;
        const char *leaf = (last_slash && *(last_slash + 1) != '\0') ? last_slash + 1 : "index.html";
        int n = 0;
        while (leaf[n] && n < (int)sizeof(out_name) - 1) { out_name[n] = leaf[n]; n++; }
        out_name[n] = '\0';
    }

    char ip_str[16];
    ip_addr_to_str(ip, ip_str);

    char url[300];
    int p = 0;
    const char *prefix = "http://";
    while (prefix[p]) { url[p] = prefix[p]; p++; }
    int i = 0;
    while (ip_str[i] && p < (int)sizeof(url) - 1) url[p++] = ip_str[i++];
    i = 0;
    while (path[i] && p < (int)sizeof(url) - 1) url[p++] = path[i++];
    url[p] = '\0';

    printf("Connecting to %s:80...\n", ip_str);

    char *buf = (char *)malloc(WGET_FETCH_CAP);
    if (!buf) { printf("wget: out of memory\n"); return 1; }

    int status = 0;
    long n = sys_net_fetch(url, buf, WGET_FETCH_CAP, &status);
    if (n < 0) {
        print_fetch_error(n);
        free(buf);
        return 1;
    }
    printf("HTTP %d, %ld byte(s)\n", status, n);

    if (status != 200) {
        printf("wget: server returned HTTP %d\n", status);
        free(buf);
        return 1;
    }

    long fd = sys_open(out_name, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        printf("wget: cannot create %s\n", out_name);
        free(buf);
        return 1;
    }

    long pos = 0;
    while (pos < n) {
        long written = sys_write((int)fd, buf + pos, (unsigned long)(n - pos));
        if (written <= 0) { printf("wget: write failed\n"); break; }
        pos += written;
    }
    sys_close((int)fd);
    free(buf);

    printf("Saved to %s (%ld bytes)\n", out_name, pos);
    return 0;
}
