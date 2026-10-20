#include "rtl8139.h"
#include <stdio.h>
#include "eth.h"

#include <string.h>
#include <lufira/syscall.h>
#include "netd_hw.h"

/* ======================================================================== */
/* Регистры (смещения от base I/O)                                          */
/* ======================================================================== */

#define RTL_REG_MAC0      0x00
#define RTL_REG_TSD0      0x10
#define RTL_REG_TSAD0     0x20
#define RTL_REG_RBSTART   0x30
#define RTL_REG_CR        0x37
#define RTL_REG_CAPR      0x38
#define RTL_REG_IMR       0x3C
#define RTL_REG_ISR       0x3E
#define RTL_REG_TCR       0x40
#define RTL_REG_RCR       0x44
#define RTL_REG_CONFIG1   0x52

#define RTL_CR_BUFE  (1u << 0)
#define RTL_CR_TE    (1u << 2)
#define RTL_CR_RE    (1u << 3)
#define RTL_CR_RST   (1u << 4)

#define RTL_RCR_APM        (1u << 1)
#define RTL_RCR_AB         (1u << 3)
#define RTL_RCR_WRAP       (1u << 7)
#define RTL_RCR_MXDMA_UNLIMITED (7u << 8)
#define RTL_RCR_RBLEN_8K   (0u << 11)
#define RTL_RCR_RXFTH_NONE (7u << 13)

#define RTL_TCR_MXDMA_UNLIMITED (7u << 8)
#define RTL_TCR_IFG_STANDARD    (3u << 24)

#define RTL_RX_STATUS_ROK (1u << 0)

#define RTL_TSD_TOK  (1u << 15)
#define RTL_TSD_TUN  (1u << 14)
#define RTL_TSD_TABT (1u << 31)

/* ======================================================================== */
/* Состояние драйвера                                                       */
/* ======================================================================== */

#define RTL_RX_BUF_LOGICAL_SIZE 8192u
#define RTL_RX_BUF_PHYS_SIZE    (8192u + 16u + 1500u)
#define RTL_RX_BUF_PAGES        3 // ceil(9708/4096)

#define RTL_TX_SLOTS       4
#define RTL_TX_SLOT_BYTES  4096u

static int      rtl_found = 0;
static uint16_t rtl_io_base = 0;
static uint8_t  rtl_mac[6];

static uint64_t rtl_rx_buf_phys = 0;
static uint8_t *rtl_rx_buf_virt = NULL;
static uint32_t rtl_rx_offset = 0;

static uint64_t rtl_tx_buf_phys[RTL_TX_SLOTS];
static uint8_t *rtl_tx_buf_virt[RTL_TX_SLOTS];
static int      rtl_tx_next = 0;

#define RTL_POLL_MAX_PACKETS_PER_TICK 8

/* ======================================================================== */
/* PCI-обнаружение                                                          */
/* ======================================================================== */

static int rtl8139_find_controller(void) {
    struct lufira_pci_device dev;
    if (sys_driver_claim_device(0x10EC, 0x8139, &dev) != 0) {
        return 0;
    }

    if (dev.bars[0].size == 0 || !dev.bars[0].is_io) {
        printf("[RTL8139] Expected I/O BAR0\n");
        return 0;
    }
    if (dev.bars[0].address > 0xFFFF) {
        printf("[RTL8139] Invalid I/O BAR address\n");
        return 0;
    }

    rtl_io_base = (uint16_t)dev.bars[0].address;

    netd_pci_enable(&dev, PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER | PCI_COMMAND_INT_DISABLE);
    return 1;
}

/* ======================================================================== */
/* Сброс и настройка                                                        */
/* ======================================================================== */

static int rtl8139_reset(void) {
    io_out8((uint16_t)(rtl_io_base + RTL_REG_CONFIG1), 0x00);
    io_out8((uint16_t)(rtl_io_base + RTL_REG_CR), RTL_CR_RST);

    for (int i = 0; i < 1000; i++) {
        if (!(io_in8((uint16_t)(rtl_io_base + RTL_REG_CR)) & RTL_CR_RST)) {
            return 1;
        }
        sys_msleep(1);
    }

    printf("[RTL8139] Reset timed out\n");
    return 0;
}

static int rtl8139_alloc_buffers(void) {
    uint64_t phys = 0;
    long virt = sys_dma_alloc(RTL_RX_BUF_PAGES, &phys);
    if (virt < 0) {
        printf("[RTL8139] Failed to allocate contiguous RX buffer\n");
        return 0;
    }
    rtl_rx_buf_phys = phys;
    rtl_rx_buf_virt = (uint8_t *)virt;
    memset(rtl_rx_buf_virt, 0, (size_t)RTL_RX_BUF_PAGES * 4096);

    uint64_t tx_phys = 0;
    long tx_virt = sys_dma_alloc(RTL_TX_SLOTS, &tx_phys);
    if (tx_virt < 0) {
        printf("[RTL8139] Failed to allocate TX buffers\n");
        return 0;
    }
    for (int i = 0; i < RTL_TX_SLOTS; i++) {
        rtl_tx_buf_phys[i] = tx_phys + (uint64_t)i * RTL_TX_SLOT_BYTES;
        rtl_tx_buf_virt[i] = (uint8_t *)(tx_virt + (long)i * RTL_TX_SLOT_BYTES);
    }

    return 1;
}

static void rtl8139_configure(void) {
    io_out32((uint16_t)(rtl_io_base + RTL_REG_RBSTART), (uint32_t)rtl_rx_buf_phys);
    io_out16((uint16_t)(rtl_io_base + RTL_REG_IMR), 0x0000);

    io_out32((uint16_t)(rtl_io_base + RTL_REG_TCR),
              RTL_TCR_MXDMA_UNLIMITED | RTL_TCR_IFG_STANDARD);

    io_out32((uint16_t)(rtl_io_base + RTL_REG_RCR),
              RTL_RCR_APM | RTL_RCR_AB | RTL_RCR_WRAP |
              RTL_RCR_MXDMA_UNLIMITED | RTL_RCR_RBLEN_8K | RTL_RCR_RXFTH_NONE);

    io_out8((uint16_t)(rtl_io_base + RTL_REG_CR), RTL_CR_RE | RTL_CR_TE);

    // CAPR аппаратно всегда смещён на -16 от настоящей позиции чтения.
    io_out16((uint16_t)(rtl_io_base + RTL_REG_CAPR), (uint16_t)(0u - 16u));
    rtl_rx_offset = 0;

    for (int i = 0; i < 6; i++) {
        rtl_mac[i] = io_in8((uint16_t)(rtl_io_base + RTL_REG_MAC0 + i));
    }
}

/* ======================================================================== */
/* Публичное API                                                            */
/* ======================================================================== */

void rtl8139_init(void) {
    if (!rtl8139_find_controller()) return;
    if (!rtl8139_reset()) return;
    if (!rtl8139_alloc_buffers()) return;
    rtl8139_configure();

    rtl_found = 1;

    printf("[RTL8139] MAC = %x:%x:%x:%x:%x:%x\n",
         rtl_mac[0], rtl_mac[1], rtl_mac[2], rtl_mac[3], rtl_mac[4], rtl_mac[5]);
}

int rtl8139_found(void) {
    return rtl_found;
}

const uint8_t *rtl8139_get_mac(void) {
    return rtl_mac;
}

int rtl8139_send(const void *frame, uint16_t len) {
    if (!rtl_found || !frame || len == 0) return -1;
    if (len > RTL_TX_SLOT_BYTES) return -1;

    int slot = rtl_tx_next;
    rtl_tx_next = (rtl_tx_next + 1) % RTL_TX_SLOTS;

    memcpy(rtl_tx_buf_virt[slot], frame, len);

    uint16_t tsad_port = (uint16_t)(rtl_io_base + RTL_REG_TSAD0 + slot * 4);
    uint16_t tsd_port  = (uint16_t)(rtl_io_base + RTL_REG_TSD0 + slot * 4);

    io_out32(tsad_port, (uint32_t)rtl_tx_buf_phys[slot]);
    uint32_t tx_len = len < 60 ? 60 : len;
    io_out32(tsd_port, tx_len);

    for (int i = 0; i < 200; i++) {
        uint32_t status = io_in32(tsd_port);
        if (status & (RTL_TSD_TOK | RTL_TSD_TUN | RTL_TSD_TABT)) {
            if (status & RTL_TSD_TOK) return 0;
            printf("[RTL8139] TX error, TSD=%x\n", status);
            return -1;
        }
        sys_msleep(1);
    }

    printf("[RTL8139] TX timed out\n");
    return -1;
}

void rtl8139_poll(void) {
    if (!rtl_found) return;

    for (int i = 0; i < RTL_POLL_MAX_PACKETS_PER_TICK; i++) {
        if (io_in8((uint16_t)(rtl_io_base + RTL_REG_CR)) & RTL_CR_BUFE) break;

        uint8_t *hdr = rtl_rx_buf_virt + rtl_rx_offset;
        uint16_t status = *(uint16_t *)hdr;
        uint16_t frame_len = *(uint16_t *)(hdr + 2);

        if (!(status & RTL_RX_STATUS_ROK) || frame_len < 4 || frame_len > 1600) {
            printf("[RTL8139] bad RX header, status=%x len=%u\n", status, frame_len);
            break;
        }

        uint16_t payload_len = (uint16_t)(frame_len - 4);
        eth_receive(hdr + 4, payload_len);

        uint32_t advance = (uint32_t)((frame_len + 4 + 3) & ~3u);
        rtl_rx_offset = (rtl_rx_offset + advance) % RTL_RX_BUF_LOGICAL_SIZE;

        io_out16((uint16_t)(rtl_io_base + RTL_REG_CAPR),
                   (uint16_t)(rtl_rx_offset - 16));
    }

    uint16_t isr = io_in16((uint16_t)(rtl_io_base + RTL_REG_ISR));
    if (isr) io_out16((uint16_t)(rtl_io_base + RTL_REG_ISR), isr);
}
