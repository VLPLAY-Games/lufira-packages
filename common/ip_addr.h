// ip_addr.h — общий IPv4-парсер/форматтер для user/ping.c, user/ifconfig.c,
// user/wget.c. Та же кодировка, что ip_str_to_addr()/ip_addr_to_str() в
// kernel/net/net.c (октеты в том же порядке, старший первый) — не вызов
// кода ядра (там не syscall), просто копия той же логики: libc этой ОС
// сетевых адресов не знает вовсе.
#pragma once

static inline int ip_addr_parse(const char *s, unsigned int *out_ip) {
    unsigned int octets[4];
    const char *p = s;

    for (int i = 0; i < 4; i++) {
        if (*p < '0' || *p > '9') return -1;

        unsigned int val = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            val = val * 10 + (unsigned int)(*p - '0');
            p++;
            digits++;
            if (digits > 3 || val > 255) return -1;
        }
        octets[i] = val;

        if (i < 3) {
            if (*p != '.') return -1;
            p++;
        }
    }
    if (*p != '\0') return -1;

    *out_ip = (octets[0] << 24) | (octets[1] << 16) | (octets[2] << 8) | octets[3];
    return 0;
}

static inline void ip_addr_to_str(unsigned int ip, char *out) {
    unsigned char octets[4] = {
        (unsigned char)(ip >> 24), (unsigned char)(ip >> 16),
        (unsigned char)(ip >> 8), (unsigned char)ip
    };
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        unsigned int v = octets[i];
        char tmp[4];
        int tn = 0;
        if (v == 0) tmp[tn++] = '0';
        while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
        while (tn > 0) out[pos++] = tmp[--tn];
        if (i < 3) out[pos++] = '.';
    }
    out[pos] = '\0';
}
