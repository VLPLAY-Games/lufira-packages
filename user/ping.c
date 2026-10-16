// ping.c — перенос команды "ping" (kernel/shell/commands/net.c,
// command_ping(), теперь мёртвый код — см. комментарий в начале
// lufira-packages/shell/shell.c: "Network/sound всё ещё без syscall'а —
// не портированы") в userspace, поверх нового SYS_PING (один Echo Request +
// ожидание за вызов — цикл по seq/печать остаются здесь, как и раньше
// делал kernel-native command_ping()).
//
// IP-парсер/форматтер — свои: это ровно то же, что ip_str_to_addr()/
// ip_addr_to_str() в kernel/net/net.c (октеты в том же порядке, старший
// первый), но там не syscall — повторять незачем, кроме как скопировать:
// libc этой ОС сетевых адресов не знает вовсе.
//
// Сборка — как у cpuload.c/free.c.

#include <stdio.h>
#include <lufira/syscall.h>

static int parse_ip(const char *s, unsigned int *out_ip) {
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

static void ip_to_str(unsigned int ip, char *out) {
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

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: ping <ip> [count]\n");
        return 1;
    }

    unsigned int ip;
    if (parse_ip(argv[1], &ip) != 0) {
        printf("ping: invalid IP address\n");
        return 1;
    }

    int count = 4;
    if (argc >= 3) {
        int n = 0;
        for (const char *s = argv[2]; *s >= '0' && *s <= '9'; s++) n = n * 10 + (*s - '0');
        if (n > 0) count = n;
    }

    char ip_str[16];
    ip_to_str(ip, ip_str);
    printf("PING %s\n", ip_str);

    unsigned int id = (unsigned int)sys_getpid();
    int sent = 0, received = 0;

    for (int seq = 1; seq <= count; seq++) {
        sent++;
        long rc = sys_ping(ip, id, (unsigned int)seq, 1000);

        if (rc == -2) { printf("ping: no network device found\n"); break; }
        if (rc == -4) { printf("ping: network busy, try again\n"); break; }
        if (rc == -3) { printf("icmp_seq=%d: send failed\n", seq); continue; }
        if (rc < 0) { printf("icmp_seq=%d: timeout\n", seq); continue; }

        received++;
        printf("icmp_seq=%d: time=%ldms\n", seq, rc);
    }

    int loss_pct = sent > 0 ? ((sent - received) * 100) / sent : 0;
    printf("\n%d packets transmitted, %d received, %d%% packet loss\n", sent, received, loss_pct);
    return 0;
}
