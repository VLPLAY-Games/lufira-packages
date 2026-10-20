// main.c — сетевой демон netd (v0.9.5: сеть вынесена из ядра LufiraOS в
// userspace, тот же принцип, что soundd — см. daemons/soundd.c). Пробует
// RTL8139/E1000/E1000E по очереди (driver.h — PCI/port-I/O/MMIO/DMA вместо
// прямого доступа к железу, которым раньше обладало только ядро),
// регистрируется единственным сетевым демоном (SYS_NETD_REGISTER,
// root-only), пытается получить DHCP-аренду и дальше крутит один цикл:
// неблокирующе проверяет control-plane запросы от ядра (NETD_OP_*,
// netd_protocol.h — релей SYS_NET_FETCH/SYS_PING/SYS_NET_CONFIG_*) и
// дренирует RX-кольцо активной карты (net_poll()) в простое.
//
// Один процесс = одна последовательная очередь запросов — net_lock_acquire()
// (был в kernel-side net.h) здесь не нужен, однопоточность и так сериализует
// tcp.c/icmp.c/dns.c/tls.c (всё состояние статическое "одно соединение
// разом", как и было в ядре).

#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>
#include <lufira/netd_protocol.h>

#include "net.h"
#include "dhcp.h"
#include "icmp.h"
#include "http_client.h"

static void handle_fetch(const struct netd_request *req, struct netd_reply *reply) {
    // URL — отдельная shm-область (одна страница, NUL-терминирована ядром,
    // sys_net_fetch()/syscall.c) — не тесное поле в самом запросе: реальные
    // URL пакетного индекса длиннее любого разумного усечения mailbox'а.
    long url_virt = sys_shm_attach((int32_t)req->url_shm_id);
    if (url_virt < 0) { reply->result = HTTP_FETCH_ENODEV; return; }
    const char *url = (const char *)url_virt;

    void *buf = NULL;
    if (req->cap > 0) {
        long virt = sys_shm_attach((int32_t)req->body_shm_id);
        if (virt < 0) {
            sys_munmap((void *)url_virt, 4096);
            reply->result = HTTP_FETCH_ENODEV;
            return;
        }
        buf = (void *)virt;
    }

    int status = 0;
    int n = http_fetch(url, buf, req->cap, &status);

    sys_munmap((void *)url_virt, 4096);
    if (buf) {
        uint64_t aligned_cap = ((uint64_t)req->cap + 4095u) & ~(uint64_t)4095u;
        sys_munmap(buf, aligned_cap);
    }

    reply->result = n;
    reply->a[0] = status;
}

static void handle_ping(const struct netd_request *req, struct netd_reply *reply) {
    if (!net_found()) { reply->result = -2; return; }

    uint32_t dst_ip = (uint32_t)req->a[0];
    uint16_t id = (uint16_t)req->a[1];
    uint16_t seq = (uint16_t)req->a[2];
    uint32_t timeout_ms = (uint32_t)req->a[3];

    if (icmp_ping_send(dst_ip, id, seq) != 0) { reply->result = -3; return; }

    uint32_t rtt_ms = 0;
    if (icmp_ping_wait(timeout_ms, &rtt_ms) != 0) { reply->result = -1; return; }
    reply->result = (int32_t)rtt_ms;
}

static void handle_config_get(struct netd_reply *reply) {
    reply->result = 0;
    reply->a[0] = net_found() ? 1 : 0;
    if (net_found()) memcpy(reply->mac, net_get_mac(), 6);
    else memset(reply->mac, 0, 6);

    const net_config_t *cfg = net_get_config();
    reply->our_ip = cfg->our_ip;
    reply->netmask = cfg->netmask;
    reply->gateway = cfg->gateway;
    reply->dns_server = cfg->dns_server;
}

static void handle_config_set(const struct netd_request *req, struct netd_reply *reply) {
    net_set_config((uint32_t)req->a[0], (uint32_t)req->a[1], (uint32_t)req->a[2],
                   net_get_config()->dns_server);
    reply->result = 0;
}

static void handle_request(uint32_t sender_pid, const struct netd_request *req) {
    struct netd_reply reply;
    memset(&reply, 0, sizeof(reply));

    switch (req->opcode) {
    case NETD_OP_FETCH:       handle_fetch(req, &reply); break;
    case NETD_OP_PING:        handle_ping(req, &reply); break;
    case NETD_OP_CONFIG_GET:  handle_config_get(&reply); break;
    case NETD_OP_CONFIG_SET:  handle_config_set(req, &reply); break;
    default:                  reply.result = -1; break;
    }

    sys_ipc_send((long)sender_pid, &reply, sizeof(reply));
}

int main(void) {
    net_init();
    if (!net_found()) {
        printf("netd: no network controller found, exiting\n");
        return 1;
    }

    if (dhcp_start(4000) == 0) {
        printf("netd: DHCP lease applied\n");
    } else {
        printf("netd: no DHCP lease, staying on static default\n");
    }

    if (sys_netd_register() != 0) {
        printf("netd: registration failed (already running, or not root?)\n");
        return 1;
    }

    for (;;) {
        struct lufira_ipc_msg msg;
        if (sys_ipc_recv(&msg, 100) == 1) {
            if (msg.len == sizeof(struct netd_request)) {
                handle_request(msg.sender_pid, (const struct netd_request *)msg.data);
            }
        }
        net_poll();
    }
}
