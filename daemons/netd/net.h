#pragma once

#include <stdint.h>
#include <stddef.h>

/*
 * Общие типы/утилиты сетевого стека и точка входа (net_init()/net_poll()) —
 * v0.9.5: перенесено из kernel/net/net.h в userspace-демон netd (daemons/
 * netd/main.c). Стек Ethernet/ARP/IPv4/ICMP/TCP/UDP/DNS/TLS работает поверх
 * одной из rtl8139.c/e1000.c/e1000e.c (см. nic_driver_t ниже) — остальной
 * стек не знает, какая это карта, и видит её только через net_found()/
 * net_get_mac()/net_send_frame()/net_poll().
 *
 * IP-адреса хранятся как uint32_t в ХОСТОВОМ порядке байт
 * (a.b.c.d == (a<<24)|(b<<16)|(c<<8)|d); htonl()/ntohl() — только на
 * границе заголовков.
 *
 * net_lock_acquire()/net_lock_release() (были в kernel-side net.h) здесь не
 * нужны: netd — один процесс с одним циклом обработки (main.c), запросы
 * клиентов (SYS_NET_FETCH/PING/...) и так разбираются строго по одному —
 * сериализация уже даётся самой однопоточностью демона.
 */

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

// Интернет-чексум (16-бит, одно-дополнительная сумма, инвертированная).
// Считает big-endian внутри пары — перед записью в заголовок нужен
// htons() (см. ip.c/icmp.c/tcp.c).
uint16_t net_checksum(const void *data, uint32_t len);

// "a.b.c.d" -> host-order uint32_t. 0 = успех, -1 = неверный формат.
// Останавливается на конце строки или первом пробеле (часто передают
// остаток команды целиком).
int ip_str_to_addr(const char *s, uint32_t *out_ip);

// out должен вмещать минимум 16 байт ("255.255.255.255\0").
void ip_addr_to_str(uint32_t ip, char *out);

// out должен вмещать минимум 18 байт ("xx:xx:xx:xx:xx:xx\0").
void mac_addr_to_str(const uint8_t mac[6], char *out);

typedef struct {
    uint32_t our_ip;
    uint32_t netmask;
    uint32_t gateway;
    // Резолвер для dns_resolve() — обращение по имени хоста, не только по
    // IP. Дефолт — DNS-форвардер QEMU SLIRP (10.0.2.3, см. net.c),
    // заменяется DHCP при успехе (dhcp.c).
    uint32_t dns_server;
} net_config_t;

// Единый интерфейс драйвера карты (см. net_init()). Каждый драйвер
// (rtl8139.c/e1000.c/e1000e.c) даёт такой набор функций под своими
// именами — структура просто таблица указателей на них.
typedef struct {
    const char *name;
    void (*init)(void);
    int (*found)(void);
    const uint8_t *(*get_mac)(void);
    int (*send)(const void *frame, uint16_t len);
    void (*poll)(void);
} nic_driver_t;

// Пробует RTL8139/E1000/E1000E по очереди (см. nic_driver_t), использует
// первую найденную; при успехе ставится статический дефолт под QEMU SLIRP
// (10.0.2.15/24, шлюз 10.0.2.2), который main() (main.c) пытается заменить
// настоящей DHCP-арендой. Безопасно вызывать и без найденной карты.
void net_init(void);

int net_found(void);

// 6-байтный MAC активной карты (валиден только при net_found()).
const uint8_t *net_get_mac(void);

// Отправить готовый Ethernet-кадр через активную карту — см. eth_send().
int net_send_frame(const void *frame, uint16_t len);

// Опрашивает активную NIC и прогоняет пришедшие кадры через стек:
// eth_receive()->arp/ip->icmp/tcp/udp. Вызывается и из главного цикла
// main.c (фоновое дренирование кольца, пока нет запросов от клиентов), и
// изнутри блокирующих ожиданий tcp_connect()/icmp_ping_wait()/arp_resolve()/
// dns_resolve() — тот же приём, что был в kernel-side net_poll().
void net_poll(void);

void net_set_config(uint32_t ip, uint32_t netmask, uint32_t gateway, uint32_t dns_server);
const net_config_t *net_get_config(void);
