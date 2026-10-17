// ping.c — перенос команды "ping" (kernel/shell/commands/net.c,
// command_ping(), теперь мёртвый код — см. комментарий в начале
// lufira-packages/shell/shell.c: "Network/sound всё ещё без syscall'а —
// не портированы") в userspace, поверх нового SYS_PING (один Echo Request +
// ожидание за вызов — цикл по seq/печать остаются здесь, как и раньше
// делал kernel-native command_ping()). IP-парсер/форматтер — common/ip_addr.h
// (общий и с ifconfig.c/wget.c).
//
// Сборка — как у cpuload.c/free.c.

#include <stdio.h>
#include <lufira/syscall.h>
#include "../common/ip_addr.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: ping <ip> [count]\n");
        return 1;
    }

    unsigned int ip;
    if (ip_addr_parse(argv[1], &ip) != 0) {
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
    ip_addr_to_str(ip, ip_str);
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
