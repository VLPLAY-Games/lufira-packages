// ifconfig.c — перенос команды "ifconfig" (kernel/shell/commands/net.c,
// command_ifconfig(), теперь мёртвый код) в userspace, поверх новых
// SYS_NET_CONFIG_GET/SYS_NET_CONFIG_SET. IP-парсер/форматтер —
// common/ip_addr.h (общий и с ping.c/wget.c).
//
// Сборка — как у cpuload.c/free.c.

#include <stdio.h>
#include <lufira/syscall.h>
#include "../common/ip_addr.h"

static void print_mac(const unsigned char mac[6]) {
    static const char hex[] = "0123456789abcdef";
    char buf[18];
    int p = 0;
    for (int i = 0; i < 6; i++) {
        buf[p++] = hex[mac[i] >> 4];
        buf[p++] = hex[mac[i] & 0xF];
        if (i < 5) buf[p++] = ':';
    }
    buf[p] = '\0';
    printf("%s", buf);
}

static int show(void) {
    struct lufira_net_config cfg;
    if (sys_net_config_get(&cfg) != 0) {
        printf("ifconfig: failed to read network config\n");
        return 1;
    }

    if (!cfg.link_up) {
        printf("rtl0: no network device found\n");
        return 0;
    }

    printf("rtl0: link up, mac ");
    print_mac(cfg.mac);
    printf("\n");

    char ip_str[16], mask_str[16], gw_str[16];
    ip_addr_to_str(cfg.our_ip, ip_str);
    ip_addr_to_str(cfg.netmask, mask_str);
    ip_addr_to_str(cfg.gateway, gw_str);
    printf("  inet %s  netmask %s  gateway %s\n", ip_str, mask_str, gw_str);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return show();

    if (argc != 4) {
        printf("Usage: ifconfig [ip netmask gateway]\n");
        return 1;
    }

    unsigned int ip, mask, gw;
    if (ip_addr_parse(argv[1], &ip) != 0 ||
        ip_addr_parse(argv[2], &mask) != 0 ||
        ip_addr_parse(argv[3], &gw) != 0) {
        printf("ifconfig: invalid IP address\n");
        return 1;
    }

    sys_net_config_set(ip, mask, gw);
    printf("ifconfig: updated\n");
    return 0;
}
