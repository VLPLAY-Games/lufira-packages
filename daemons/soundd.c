// soundd.c — звуковой демон (v0.9.5: звук вынесен из ядра LufiraOS в
// userspace). Заявляет HDA (предпочтительно) либо AC'97 через
// SYS_DRIVER_CLAIM_DEVICE_BY_CLASS, регистрируется единственным звуковым
// демоном системы (SYS_SOUNDD_REGISTER, root-only — запускается кернелом
// при загрузке, см. spawn_daemon_process() в kernel.c), и дальше крутит один
// цикл: вычитывает PCM-кадры из общего кольца ядра (SYS_SOUND_CONSUME,
// наполняется любым клиентом через SYS_SOUND_PLAY_PCM) и держит непрерывно
// играющий аппаратный кольцевой буфер — ОБА бэкенда теперь работают именно
// так (раньше HDA делал полный stop/reset/start на каждый вызов
// hda_play_pcm_stereo(), отсюда и баг "только короткие бипы", см. ниже).
// Тем же циклом неблокирующе вычитывает control-plane запросы
// (SOUNDD_OP_*, soundd_protocol.h) — SYS_SOUND_INFO/SET_VOLUME/STOP
// (syscall.c) релеят их сюда через mailbox.
//
// Регистры/протоколы контроллеров ниже — прямой перенос бывших
// kernel/drivers/sound/{ac97,hda}.c: тот же алгоритм, только порты/MMIO/DMA
// идут через новые привилегированные syscall'ы (driver.h) вместо прямого
// доступа к железу, которым обладало только ядро.

#include <stdio.h>
#include <string.h>
#include <lufira/syscall.h>
#include <lufira/soundd_protocol.h>

/* ============================================================ */
/* PCI command register + port I/O + MMIO — общие для обоих бэкендов */
/* ============================================================ */

#define PCI_COMMAND             0x04
#define PCI_COMMAND_IO          (1u << 0)
#define PCI_COMMAND_MEMORY      (1u << 1)
#define PCI_COMMAND_BUS_MASTER  (1u << 2)
#define PCI_COMMAND_INT_DISABLE (1u << 10)

static void pci_enable(const struct lufira_pci_device *dev, uint32_t bits) {
    uint32_t cmd = 0;
    sys_pci_config_read(dev->bus, dev->device, dev->function, PCI_COMMAND, 2, &cmd);
    cmd |= bits;
    sys_pci_config_write(dev->bus, dev->device, dev->function, PCI_COMMAND, 2, cmd);
}

static inline uint8_t io_in8(uint16_t port) { uint32_t v = 0; sys_io_in(port, 1, &v); return (uint8_t)v; }
static inline uint16_t io_in16(uint16_t port) { uint32_t v = 0; sys_io_in(port, 2, &v); return (uint16_t)v; }
static inline uint32_t io_in32(uint16_t port) { uint32_t v = 0; sys_io_in(port, 4, &v); return v; }
static inline void io_out8(uint16_t port, uint8_t v) { sys_io_out(port, 1, v); }
static inline void io_out16(uint16_t port, uint16_t v) { sys_io_out(port, 2, v); }
static inline void io_out32(uint16_t port, uint32_t v) { sys_io_out(port, 4, v); }

// Чисто CPU-спин, без зависимости от таймера — как ac97_delay() в
// kernel/drivers/sound/ac97.c (тот же приём, не нужен syscall).
static void cpu_delay(void) {
    for (volatile uint32_t i = 0; i < 100000; i++) __asm__ volatile ("pause");
}

static inline uint8_t  reg_read8(void *base, uint32_t off)  { return *(volatile uint8_t  *)((uint8_t *)base + off); }
static inline uint16_t reg_read16(void *base, uint32_t off) { return *(volatile uint16_t *)((uint8_t *)base + off); }
static inline uint32_t reg_read32(void *base, uint32_t off) { return *(volatile uint32_t *)((uint8_t *)base + off); }
static inline void reg_write8(void *base, uint32_t off, uint8_t val)   { *(volatile uint8_t  *)((uint8_t *)base + off) = val; }
static inline void reg_write16(void *base, uint32_t off, uint16_t val) { *(volatile uint16_t *)((uint8_t *)base + off) = val; }
static inline void reg_write32(void *base, uint32_t off, uint32_t val) { *(volatile uint32_t *)((uint8_t *)base + off) = val; }

/* ============================================================ */
/* AC'97 backend — порт kernel/drivers/sound/ac97.c                */
/* ============================================================ */

#define AC97_REG_RESET              0x00
#define AC97_REG_MASTER_VOLUME      0x02
#define AC97_REG_PCM_OUT_VOLUME     0x18
#define AC97_REG_POWERDOWN          0x26
#define AC97_REG_EXT_AUDIO_ID       0x28
#define AC97_REG_EXT_AUDIO_CTRL     0x2A
#define AC97_REG_PCM_FRONT_RATE     0x2C
#define AC97_REG_VENDOR_ID1         0x7C
#define AC97_REG_VENDOR_ID2         0x7E

#define AC97_EACS_VRA               (1u << 0)
#define AC97_POWER_DAC              (1u << 9)
#define AC97_VOLUME_MUTE            (1u << 15)

#define AC97_BM_GLOB_CNT           0x2C
#define AC97_BM_GLOB_STA           0x30

#define AC97_PO_BDBAR              0x10
#define AC97_PO_CIV                0x14
#define AC97_PO_LVI                0x15
#define AC97_PO_SR                 0x16
#define AC97_PO_CR                 0x1B

#define AC97_SR_DCH                0x0001
#define AC97_SR_W1C_MASK           0x001C

#define AC97_CR_RPBM               0x01
#define AC97_CR_RR                 0x02

#define AC97_GLOB_COLD_RESET       (1u << 1)
#define AC97_GLOB_PCR              (1u << 8)

#define AC97_BDL_ENTRIES           32
#define AC97_DMA_PAGE_SIZE         4096
#define AC97_BDL_IOC               (1u << 15)
#define AC97_BDL_BUP               (1u << 14)
// См. комментарий у этой же константы в ac97.c — ограничивает, насколько
// далеко ac97_stream_write() может убежать от CIV, не дожидаясь playback.
#define AC97_STREAM_MAX_LOOKAHEAD_PAGES 4

struct ac97_bdl_entry { uint32_t buffer; uint16_t samples; uint16_t flags; };

static struct lufira_pci_device ac97_dev;
static int      ac97_available = 0;
static uint16_t ac97_nam_base = 0, ac97_nabm_base = 0;
static uint8_t  ac97_volume = 100;
static uint32_t ac97_sample_rate = 48000;
static uint64_t ac97_bdl_phys = 0;
static struct ac97_bdl_entry *ac97_bdl = NULL;
static uint64_t ac97_dma_phys[AC97_BDL_ENTRIES];
static int16_t *ac97_dma_virt[AC97_BDL_ENTRIES];
static int      ac97_stream_running = 0;
static uint32_t ac97_stream_next_page = 0;

static uint16_t ac97_read_codec(uint8_t reg) {
    if (ac97_nam_base == 0) return 0xFFFF;
    return io_in16((uint16_t)(ac97_nam_base + reg));
}
static void ac97_write_codec(uint8_t reg, uint16_t value) {
    if (ac97_nam_base == 0) return;
    io_out16((uint16_t)(ac97_nam_base + reg), value);
}

static int ac97_find_controller(void) {
    if (sys_driver_claim_device_by_class(0x04, 0x01, &ac97_dev) != 0) {
        printf("[AC97] Controller not found\n");
        return 0;
    }

    if (ac97_dev.bars[0].size == 0 || ac97_dev.bars[1].size == 0 ||
        !ac97_dev.bars[0].is_io || !ac97_dev.bars[1].is_io) {
        printf("[AC97] Expected I/O BARs\n");
        return 0;
    }
    if (ac97_dev.bars[0].address > 0xFFFF || ac97_dev.bars[1].address > 0xFFFF) {
        printf("[AC97] Invalid I/O BAR address\n");
        return 0;
    }

    // Для Intel ICH AC'97: BAR0 = Native Audio Mixer, BAR1 = Native Audio Bus Master.
    ac97_nam_base = (uint16_t)ac97_dev.bars[0].address;
    ac97_nabm_base = (uint16_t)ac97_dev.bars[1].address;

    pci_enable(&ac97_dev, PCI_COMMAND_IO | PCI_COMMAND_BUS_MASTER | PCI_COMMAND_INT_DISABLE);
    return 1;
}

static int ac97_controller_reset(void) {
    uint32_t control = io_in32((uint16_t)(ac97_nabm_base + AC97_BM_GLOB_CNT));
    control |= AC97_GLOB_COLD_RESET;
    io_out32((uint16_t)(ac97_nabm_base + AC97_BM_GLOB_CNT), control);
    cpu_delay();

    io_out32((uint16_t)(ac97_nabm_base + AC97_BM_GLOB_CNT), AC97_GLOB_COLD_RESET);
    cpu_delay();

    for (uint32_t i = 0; i < 1000; i++) {
        uint32_t status = io_in32((uint16_t)(ac97_nabm_base + AC97_BM_GLOB_STA));
        if (status & AC97_GLOB_PCR) return 1;
        cpu_delay();
    }
    printf("[AC97] Codec did not become ready\n");
    return 0;
}

static int ac97_codec_init(void) {
    ac97_write_codec(AC97_REG_RESET, 0);
    cpu_delay();

    uint16_t ext_audio_ctrl = ac97_read_codec(AC97_REG_EXT_AUDIO_CTRL);
    ext_audio_ctrl |= AC97_EACS_VRA;
    ac97_write_codec(AC97_REG_EXT_AUDIO_CTRL, ext_audio_ctrl);
    cpu_delay();

    ac97_write_codec(AC97_REG_PCM_FRONT_RATE, 48000);

    // Аттенюация: 0 = максимум, 31 = минимум (не громкость напрямую).
    ac97_write_codec(AC97_REG_MASTER_VOLUME, 0);
    ac97_write_codec(AC97_REG_PCM_OUT_VOLUME, 0);

    uint16_t power = ac97_read_codec(AC97_REG_POWERDOWN);
    power &= ~(uint16_t)AC97_POWER_DAC;
    ac97_write_codec(AC97_REG_POWERDOWN, power);
    return 1;
}

static int ac97_alloc_dma(void) {
    uint64_t phys = 0;
    long virt = sys_dma_alloc(1, &phys);
    if (virt < 0) { printf("[AC97] Failed to allocate BDL page\n"); return 0; }
    if (phys > 0xFFFFFFFFULL) { printf("[AC97] BDL above 4GB\n"); return 0; }

    ac97_bdl_phys = phys;
    ac97_bdl = (struct ac97_bdl_entry *)virt;
    memset(ac97_bdl, 0, AC97_BDL_ENTRIES * sizeof(struct ac97_bdl_entry));

    // ОДНИМ запросом на все 32 страницы данных — не по одной: MAX_MMAP_REGIONS
    // (process.h, LufiraOS) ограничивает процесс 32 ОБЛАСТЯМИ разом, а BDL
    // выше уже занял один слот. В отличие от ac97.c (ядро, где
    // pmm_alloc_page() per-page — дешёвый, обильный ресурс, а 32 независимые
    // страницы были сознательным выбором именно чтобы не требовать
    // непрерывности), 128KB подряд идущих физических страниц на старте
    // загрузки — тривиальный запрос, непрерывность не проблема здесь.
    uint64_t data_phys = 0;
    long data_virt = sys_dma_alloc(AC97_BDL_ENTRIES, &data_phys);
    if (data_virt < 0) { printf("[AC97] Failed to allocate DMA pages\n"); return 0; }
    if (data_phys + (uint64_t)AC97_BDL_ENTRIES * AC97_DMA_PAGE_SIZE > 0x100000000ULL) {
        printf("[AC97] DMA pages above 4GB\n");
        return 0;
    }
    memset((void *)data_virt, 0, (size_t)AC97_BDL_ENTRIES * AC97_DMA_PAGE_SIZE);

    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        uint64_t p = data_phys + (uint64_t)i * AC97_DMA_PAGE_SIZE;
        ac97_dma_phys[i] = p;
        ac97_dma_virt[i] = (int16_t *)(data_virt + (long)i * AC97_DMA_PAGE_SIZE);

        ac97_bdl[i].buffer = (uint32_t)p;
        ac97_bdl[i].samples = AC97_DMA_PAGE_SIZE / 2;
        ac97_bdl[i].flags = 0;
    }
    return 1;
}

static void ac97_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    ac97_volume = volume;

    uint8_t attenuation = (uint8_t)(31 - ((uint32_t)volume * 31) / 100);
    uint16_t value = ((uint16_t)attenuation << 8) | attenuation;
    if (volume == 0) value |= AC97_VOLUME_MUTE;

    ac97_write_codec(AC97_REG_MASTER_VOLUME, value);
    ac97_write_codec(AC97_REG_PCM_OUT_VOLUME, value);
}
static uint8_t ac97_get_volume(void) { return ac97_volume; }

static void ac97_set_sample_rate(uint32_t rate) {
    if (rate < 8000) rate = 8000;
    if (rate > 96000) rate = 96000;
    ac97_sample_rate = rate;
    ac97_write_codec(AC97_REG_PCM_FRONT_RATE, (uint16_t)rate);
}
static uint32_t ac97_get_sample_rate(void) { return ac97_sample_rate; }

static void ac97_reset_pcm_out(void) {
    uint16_t port = (uint16_t)(ac97_nabm_base + AC97_PO_CR);
    uint8_t control = io_in8(port);
    control &= ~(uint8_t)AC97_CR_RPBM;
    io_out8(port, control);

    io_out8(port, AC97_CR_RR);
    for (uint32_t i = 0; i < 100000; i++) {
        if (!(io_in8(port) & AC97_CR_RR)) break;
        __asm__ volatile ("pause");
    }
}
static void ac97_clear_pcm_status(void) {
    io_out16((uint16_t)(ac97_nabm_base + AC97_PO_SR), AC97_SR_W1C_MASK);
}

// Публикует один дескриптор кольца и продвигает LVI на него — контроллер
// не идёт дальше последнего увиденного LVI (см. разбор в ac97.c), поэтому
// это нужно делать на КАЖДУЮ записанную страницу, не только при старте.
static void ac97_bdl_mark_valid(uint32_t page, uint32_t frames) {
    ac97_bdl[page].buffer = (uint32_t)ac97_dma_phys[page];
    ac97_bdl[page].samples = (uint16_t)(frames * 2);
    ac97_bdl[page].flags = AC97_BDL_IOC;
    io_out8((uint16_t)(ac97_nabm_base + AC97_PO_LVI), (uint8_t)page);
}

// Непрерывный режим (см. разбор архитектуры в ac97.c) — канал стартует один
// раз и крутится по кольцу AC97_BDL_ENTRIES страниц до ac97_stop().
static void ac97_stream_write(const int16_t *samples, uint32_t frames) {
    if (!ac97_available || !samples || frames == 0) return;

    const uint32_t page_frames = AC97_DMA_PAGE_SIZE / 4;

    if (!ac97_stream_running) {
        ac97_reset_pcm_out();
        ac97_clear_pcm_status();
        io_out32((uint16_t)(ac97_nabm_base + AC97_PO_BDBAR), (uint32_t)ac97_bdl_phys);
    }

    uint32_t produced = 0;
    while (produced < frames) {
        uint32_t chunk = frames - produced;
        if (chunk > page_frames) chunk = page_frames;

        uint32_t page = ac97_stream_next_page;

        if (ac97_stream_running) {
            // Ждём, пока страница не окажется вне CIV и разрыв с ним не
            // станет не больше AC97_STREAM_MAX_LOOKAHEAD_PAGES.
            for (;;) {
                uint8_t civ = io_in8((uint16_t)(ac97_nabm_base + AC97_PO_CIV));
                uint32_t ahead = ((uint32_t)page - civ + AC97_BDL_ENTRIES) % AC97_BDL_ENTRIES;
                if (ahead != 0 && ahead <= AC97_STREAM_MAX_LOOKAHEAD_PAGES) break;
                __asm__ volatile ("pause");
            }
        }

        memcpy(ac97_dma_virt[page], samples + (uint64_t)produced * 2, (size_t)chunk * 4);
        ac97_bdl_mark_valid(page, chunk);

        ac97_stream_next_page = (page + 1) % AC97_BDL_ENTRIES;
        produced += chunk;
    }

    if (!ac97_stream_running) {
        for (uint32_t i = 0; i < AC97_BDL_ENTRIES; i++) {
            if (ac97_bdl[i].samples != 0) continue;
            ac97_bdl_mark_valid(i, page_frames);
        }
        io_out8((uint16_t)(ac97_nabm_base + AC97_PO_CR), AC97_CR_RPBM);
        ac97_stream_running = 1;
    }
}

static void ac97_stop(void) {
    if (!ac97_available) return;
    io_out8((uint16_t)(ac97_nabm_base + AC97_PO_CR), 0);
    ac97_clear_pcm_status();
    ac97_stream_running = 0;
    ac97_stream_next_page = 0;
}

static int ac97_init(void) {
    if (!ac97_find_controller()) return 0;
    if (!ac97_controller_reset()) return 0;
    ac97_available = 1;
    if (!ac97_codec_init()) { ac97_available = 0; return 0; }
    if (!ac97_alloc_dma()) { ac97_available = 0; return 0; }

    ac97_set_sample_rate(48000);
    ac97_set_volume(80);
    printf("[AC97] ready (%u Hz, %u%%)\n", ac97_sample_rate, ac97_volume);
    return 1;
}

/* ============================================================ */
/* HDA backend — порт kernel/drivers/sound/hda.c, с ПЕРЕПИСАННЫМ выводом  */
/* потока на непрерывное кольцо (было: stop/reset/start на каждый вызов, */
/* отсюда баг "Doom со звуком только короткие бипы" — см. hda_stream_*)  */
/* ============================================================ */

#define HDA_REG_GCAP      0x00
#define HDA_REG_GCTL      0x08
#define HDA_REG_STATESTS  0x0E
#define HDA_REG_CORBLBASE 0x40
#define HDA_REG_CORBUBASE 0x44
#define HDA_REG_CORBWP    0x48
#define HDA_REG_CORBRP    0x4A
#define HDA_REG_CORBCTL   0x4C
#define HDA_REG_CORBSIZE  0x4E
#define HDA_REG_RIRBLBASE 0x50
#define HDA_REG_RIRBUBASE 0x54
#define HDA_REG_RIRBWP    0x58
#define HDA_REG_RINTCNT   0x5A
#define HDA_REG_RIRBCTL   0x5C
#define HDA_REG_RIRBSTS   0x5D
#define HDA_REG_RIRBSIZE  0x5E
#define HDA_REG_SD0_BASE  0x80

#define HDA_GCAP_OSS(v)  (((v) >> 12) & 0xFu)
#define HDA_GCAP_ISS(v)  (((v) >> 8) & 0xFu)

#define HDA_GCTL_CRST (1u << 0)
#define HDA_CORBCTL_RUN   (1u << 1)
#define HDA_RIRBCTL_DMAEN (1u << 1)
#define HDA_RIRBCTL_IRQEN (1u << 0)

#define HDA_SDCTL_SRST (1u << 0)
#define HDA_SDCTL_RUN  (1u << 1)
#define HDA_SDCTL_STRM_SHIFT 20
#define HDA_SDSTS_BCIS (1u << 2)

#define HDA_SD_CTL   0x00
#define HDA_SD_STS   0x03
#define HDA_SD_LPIB  0x04
#define HDA_SD_CBL   0x08
#define HDA_SD_LVI   0x0C
#define HDA_SD_FMT   0x12
#define HDA_SD_BDPL  0x18
#define HDA_SD_BDPU  0x1C

#define HDA_VERB_TIMEOUT_MS 100 // см. комментарий у cpu_delay()/sys_sleep() ниже

struct hda_rirb_entry { uint32_t resp; uint32_t resp_ex; };
struct hda_bdl_entry { uint64_t address; uint32_t length; uint32_t ioc; };

static struct lufira_pci_device hda_dev;
static void *g_mmio = NULL;
static int   g_present = 0;
static int   g_ready = 0;

static uint32_t *g_corb_virt = NULL; static uint64_t g_corb_phys = 0; static uint16_t g_corb_entries = 0;
static struct hda_rirb_entry *g_rirb_virt = NULL; static uint64_t g_rirb_phys = 0; static uint16_t g_rirb_entries = 0;
static uint16_t g_rirb_read_idx = 0;

static uint8_t g_codec_addr = 0;
static uint8_t g_afg_nid = 0;
static uint8_t g_dac_nid = 0, g_pin_nid = 0;
static int     g_pin_via_node = -1;
static uint32_t g_dac_amp_steps = 0, g_pin_amp_steps = 0;

#define HDA_WIDGET_TYPE_AUDIO_OUTPUT 0x0
#define HDA_WIDGET_TYPE_PIN_COMPLEX  0x4

#define HDA_VERB_GET_PARAMETER   0xF00
#define HDA_VERB_GET_CONN_LIST   0xF02
#define HDA_VERB_SET_CONN_SELECT 0x701
#define HDA_VERB_SET_POWER_STATE 0x705
#define HDA_VERB_SET_STREAM_CHAN 0x706
#define HDA_VERB_SET_PIN_CTL     0x707
#define HDA_VERB_GET_CONFIG_DEF  0xF1C

#define HDA_LVERB_SET_AMP_GAIN_MUTE 0x3
#define HDA_LVERB_SET_CONV_FORMAT   0x2

#define HDA_PARAM_SUBORDINATE_NODE_COUNT 0x04
#define HDA_PARAM_FUNCTION_GROUP_TYPE    0x05
#define HDA_PARAM_AUDIO_WIDGET_CAPS      0x09
#define HDA_PARAM_PIN_CAPS               0x0C
#define HDA_PARAM_CONN_LIST_LENGTH       0x0E
#define HDA_PARAM_OUT_AMP_CAPS           0x12

static int hda_corb_send_raw(uint32_t verb, uint32_t *out_resp) {
    uint16_t wp = (uint16_t)(reg_read16(g_mmio, HDA_REG_CORBWP) & 0xFFu);
    uint16_t new_wp = (uint16_t)((wp + 1) % g_corb_entries);
    g_corb_virt[new_wp] = verb;
    reg_write16(g_mmio, HDA_REG_CORBWP, new_wp);

    for (int ms = 0; ms <= HDA_VERB_TIMEOUT_MS; ms++) {
        uint16_t rirb_wp = (uint16_t)(reg_read16(g_mmio, HDA_REG_RIRBWP) & 0xFFu);
        if (rirb_wp != g_rirb_read_idx) {
            g_rirb_read_idx = (uint16_t)((g_rirb_read_idx + 1) % g_rirb_entries);
            if (out_resp) *out_resp = g_rirb_virt[g_rirb_read_idx].resp;
            reg_write8(g_mmio, HDA_REG_RIRBSTS, reg_read8(g_mmio, HDA_REG_RIRBSTS));
            return 0;
        }
        sys_msleep(1);
    }
    printf("[HDA] verb timed out (verb=%u)\n", verb);
    return -1;
}

static int hda_verb(uint8_t nid, uint16_t verb12, uint8_t payload8, uint32_t *out_resp) {
    uint32_t w = ((uint32_t)g_codec_addr << 28) | ((uint32_t)nid << 20)
               | ((uint32_t)verb12 << 8) | payload8;
    return hda_corb_send_raw(w, out_resp);
}
static int hda_verb_long(uint8_t nid, uint8_t verb4, uint16_t payload16, uint32_t *out_resp) {
    uint32_t w = ((uint32_t)g_codec_addr << 28) | ((uint32_t)nid << 20)
               | ((uint32_t)verb4 << 16) | payload16;
    return hda_corb_send_raw(w, out_resp);
}
static uint32_t hda_get_param(uint8_t nid, uint8_t param_id) {
    uint32_t resp = 0;
    hda_verb(nid, HDA_VERB_GET_PARAMETER, param_id, &resp);
    return resp;
}

static int hda_find_controller(void) {
    if (sys_driver_claim_device_by_class(0x04, 0x03, &hda_dev) != 0) {
        printf("[HDA] Controller not found\n");
        return 0;
    }
    if (hda_dev.bars[0].size == 0 || hda_dev.bars[0].is_io) {
        printf("[HDA] BAR0 missing or not MMIO\n");
        return 0;
    }

    pci_enable(&hda_dev, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER | PCI_COMMAND_INT_DISABLE);

    long virt = sys_mmap_mmio(hda_dev.bars[0].address, hda_dev.bars[0].size);
    if (virt < 0) { printf("[HDA] Failed to map BAR0\n"); return 0; }
    g_mmio = (void *)virt;
    return 1;
}

static int hda_reset_controller(void) {
    reg_write32(g_mmio, HDA_REG_GCTL, reg_read32(g_mmio, HDA_REG_GCTL) & ~(uint32_t)HDA_GCTL_CRST);
    for (int i = 0; i < 200 && (reg_read32(g_mmio, HDA_REG_GCTL) & HDA_GCTL_CRST); i++) sys_msleep(1);
    if (reg_read32(g_mmio, HDA_REG_GCTL) & HDA_GCTL_CRST) { printf("[HDA] Controller did not enter reset\n"); return 0; }

    reg_write32(g_mmio, HDA_REG_GCTL, reg_read32(g_mmio, HDA_REG_GCTL) | HDA_GCTL_CRST);
    for (int i = 0; i < 200 && !(reg_read32(g_mmio, HDA_REG_GCTL) & HDA_GCTL_CRST); i++) sys_msleep(1);
    if (!(reg_read32(g_mmio, HDA_REG_GCTL) & HDA_GCTL_CRST)) { printf("[HDA] Controller did not leave reset\n"); return 0; }

    sys_msleep(1); // errata — settle time после CRST=1, см. hda.c
    return 1;
}

static int hda_setup_corb_rirb(void) {
    uint64_t corb_phys = 0, rirb_phys = 0;
    long corb_virt = sys_dma_alloc(1, &corb_phys);
    long rirb_virt = sys_dma_alloc(1, &rirb_phys);
    if (corb_virt < 0 || rirb_virt < 0) { printf("[HDA] Out of memory for CORB/RIRB\n"); return 0; }

    g_corb_phys = corb_phys; g_corb_virt = (uint32_t *)corb_virt;
    g_rirb_phys = rirb_phys; g_rirb_virt = (struct hda_rirb_entry *)rirb_virt;
    memset(g_corb_virt, 0, 4096);
    memset(g_rirb_virt, 0, 4096);

    reg_write8(g_mmio, HDA_REG_CORBCTL, 0);
    reg_write8(g_mmio, HDA_REG_RIRBCTL, 0);

    uint8_t corb_cap = (uint8_t)(reg_read8(g_mmio, HDA_REG_CORBSIZE) >> 4);
    uint8_t corb_sel = (corb_cap & 0x4) ? 2 : (corb_cap & 0x2) ? 1 : 0;
    g_corb_entries = (corb_sel == 2) ? 256 : (corb_sel == 1) ? 16 : 2;
    reg_write8(g_mmio, HDA_REG_CORBSIZE, corb_sel);

    uint8_t rirb_cap = (uint8_t)(reg_read8(g_mmio, HDA_REG_RIRBSIZE) >> 4);
    uint8_t rirb_sel = (rirb_cap & 0x4) ? 2 : (rirb_cap & 0x2) ? 1 : 0;
    g_rirb_entries = (rirb_sel == 2) ? 256 : (rirb_sel == 1) ? 16 : 2;
    reg_write8(g_mmio, HDA_REG_RIRBSIZE, rirb_sel);

    reg_write32(g_mmio, HDA_REG_CORBLBASE, (uint32_t)(corb_phys & 0xFFFFFFFFu));
    reg_write32(g_mmio, HDA_REG_CORBUBASE, (uint32_t)(corb_phys >> 32));
    reg_write32(g_mmio, HDA_REG_RIRBLBASE, (uint32_t)(rirb_phys & 0xFFFFFFFFu));
    reg_write32(g_mmio, HDA_REG_RIRBUBASE, (uint32_t)(rirb_phys >> 32));

    reg_write16(g_mmio, HDA_REG_CORBRP, 0x8000);
    for (int i = 0; i < 200 && !(reg_read16(g_mmio, HDA_REG_CORBRP) & 0x8000); i++) sys_msleep(1);
    reg_write16(g_mmio, HDA_REG_CORBRP, 0);
    for (int i = 0; i < 200 && (reg_read16(g_mmio, HDA_REG_CORBRP) & 0x8000); i++) sys_msleep(1);
    reg_write16(g_mmio, HDA_REG_CORBWP, 0);

    reg_write16(g_mmio, HDA_REG_RIRBWP, 0x8000);
    g_rirb_read_idx = 0;
    reg_write16(g_mmio, HDA_REG_RINTCNT, 0xFF);

    reg_write8(g_mmio, HDA_REG_CORBCTL, HDA_CORBCTL_RUN);
    reg_write8(g_mmio, HDA_REG_RIRBCTL, HDA_RIRBCTL_DMAEN | HDA_RIRBCTL_IRQEN);
    return 1;
}

static uint8_t hda_conn_list_first(uint8_t nid) {
    uint32_t resp = hda_get_param(nid, HDA_PARAM_CONN_LIST_LENGTH);
    if ((resp & 0x80) != 0) return 0;
    if ((resp & 0x7F) == 0) return 0;
    uint32_t entries;
    if (hda_verb(nid, HDA_VERB_GET_CONN_LIST, 0, &entries) != 0) return 0;
    return (uint8_t)(entries & 0xFFu);
}

static int hda_discover_codec(void) {
    uint32_t root_sub = hda_get_param(0, HDA_PARAM_SUBORDINATE_NODE_COUNT);
    uint8_t fg_start = (uint8_t)((root_sub >> 16) & 0xFFu);
    uint8_t fg_count = (uint8_t)(root_sub & 0xFFu);

    uint8_t afg = 0;
    for (uint8_t nid = fg_start; nid < fg_start + fg_count; nid++) {
        uint32_t fg_type = hda_get_param(nid, HDA_PARAM_FUNCTION_GROUP_TYPE);
        if ((fg_type & 0xFFu) == 0x01) { afg = nid; break; }
    }
    if (!afg) { printf("[HDA] No Audio Function Group found\n"); return 0; }

    uint32_t afg_sub = hda_get_param(afg, HDA_PARAM_SUBORDINATE_NODE_COUNT);
    uint8_t w_start = (uint8_t)((afg_sub >> 16) & 0xFFu);
    uint8_t w_count = (uint8_t)(afg_sub & 0xFFu);

    uint8_t dac = 0, pin = 0;
    for (uint16_t nid = w_start; nid < (uint16_t)w_start + w_count; nid++) {
        uint32_t caps = hda_get_param((uint8_t)nid, HDA_PARAM_AUDIO_WIDGET_CAPS);
        uint8_t type = (uint8_t)((caps >> 20) & 0xFu);

        if (type == HDA_WIDGET_TYPE_AUDIO_OUTPUT && !dac) {
            dac = (uint8_t)nid;
        } else if (type == HDA_WIDGET_TYPE_PIN_COMPLEX) {
            uint32_t cfgdef; hda_verb((uint8_t)nid, HDA_VERB_GET_CONFIG_DEF, 0, &cfgdef);
            uint8_t port_conn = (uint8_t)((cfgdef >> 30) & 0x3u);
            uint8_t default_dev = (uint8_t)((cfgdef >> 20) & 0xFu);
            uint32_t pincaps = hda_get_param((uint8_t)nid, HDA_PARAM_PIN_CAPS);
            int output_capable = (pincaps & (1u << 4)) != 0;

            if (!pin && port_conn != 1 && output_capable &&
                (default_dev == 0x0 || default_dev == 0x1 || default_dev == 0x2))
            {
                pin = (uint8_t)nid;
            }
        }
    }

    if (!dac || !pin) { printf("[HDA] No usable DAC/output pin found (dac=%u pin=%u)\n", dac, pin); return 0; }

    uint8_t via = 0;
    uint8_t first = hda_conn_list_first(pin);
    if (first == dac) {
        via = 0;
    } else if (first != 0 && hda_conn_list_first(first) == dac) {
        via = first;
    } else {
        printf("[HDA] Pin %u does not reach DAC %u within 2 hops (first=%u)\n", pin, dac, first);
        return 0;
    }

    g_afg_nid = afg; g_dac_nid = dac; g_pin_nid = pin;
    g_pin_via_node = via ? (int)via : -1;
    return 1;
}

static void hda_select_connection_to_dac(uint8_t node, uint8_t target_dac) {
    uint32_t len_resp = hda_get_param(node, HDA_PARAM_CONN_LIST_LENGTH);
    if (len_resp & 0x80) return;
    uint8_t len = (uint8_t)(len_resp & 0x7Fu);
    if (len <= 1) return;

    uint32_t entries;
    if (hda_verb(node, HDA_VERB_GET_CONN_LIST, 0, &entries) != 0) return;
    for (uint8_t i = 0; i < len && i < 4; i++) {
        uint8_t e = (uint8_t)((entries >> (i * 8)) & 0xFFu);
        if (e == target_dac) { hda_verb(node, HDA_VERB_SET_CONN_SELECT, i, NULL); return; }
    }
}

static uint8_t  g_volume = 80;
static uint32_t g_sample_rate = 48000;
static uint16_t g_fmt_reg = 0x11;

static uint16_t hda_rate_to_fmt(uint32_t rate, uint32_t *out_actual_rate) {
    static const struct { uint32_t rate; uint8_t base, mult, div; } table[] = {
        { 8000,  0, 0, 5 }, { 11025, 1, 0, 3 }, { 16000, 0, 0, 2 },
        { 22050, 1, 0, 1 }, { 32000, 0, 1, 2 }, { 44100, 1, 0, 0 },
        { 48000, 0, 0, 0 }, { 88200, 1, 1, 0 }, { 96000, 0, 1, 0 },
        { 176400,1, 3, 0 }, { 192000,0, 3, 0 },
    };
    uint32_t best = 48000; uint32_t best_diff = 0xFFFFFFFFu;
    uint8_t base = 0, mult = 0, div = 0;
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        uint32_t diff = (rate > table[i].rate) ? (rate - table[i].rate) : (table[i].rate - rate);
        if (diff < best_diff) { best_diff = diff; best = table[i].rate; base = table[i].base; mult = table[i].mult; div = table[i].div; }
    }
    if (out_actual_rate) *out_actual_rate = best;
    return (uint16_t)(((uint32_t)base << 14) | ((uint32_t)mult << 11) | ((uint32_t)div << 8) | (1u << 4) | 1u);
}

static void hda_set_sample_rate(uint32_t rate) {
    uint32_t actual;
    g_fmt_reg = hda_rate_to_fmt(rate, &actual);
    g_sample_rate = actual;
}
static uint32_t hda_get_sample_rate(void) { return g_sample_rate; }

static void hda_apply_amp(uint8_t nid, uint32_t num_steps, uint8_t volume) {
    if (num_steps == 0) return;
    uint32_t gain = ((uint32_t)volume * num_steps) / 100;
    if (gain > 0x7F) gain = 0x7F;
    uint8_t mute = (volume == 0) ? 1 : 0;
    uint16_t base = (uint16_t)((1u << 15) | ((uint32_t)mute << 7) | gain);
    hda_verb_long(nid, HDA_LVERB_SET_AMP_GAIN_MUTE, (uint16_t)(base | (1u << 13)), NULL);
    hda_verb_long(nid, HDA_LVERB_SET_AMP_GAIN_MUTE, (uint16_t)(base | (1u << 12)), NULL);
}

static void hda_set_volume(uint8_t volume) {
    if (volume > 100) volume = 100;
    g_volume = volume;
    if (!g_present) return;
    hda_apply_amp(g_dac_nid, g_dac_amp_steps, volume);
    hda_apply_amp(g_pin_nid, g_pin_amp_steps, volume);
}
static uint8_t hda_get_volume(void) { return g_volume; }

#define HDA_BDL_ENTRIES   32
#define HDA_DMA_PAGE_SIZE 4096
// Как у AC'97 (AC97_STREAM_MAX_LOOKAHEAD_PAGES) — не даём producer'у
// убежать от аппаратной позиции чтения дальше, чем на это число страниц.
#define HDA_STREAM_MAX_LOOKAHEAD_PAGES 4

static void  *g_sd0_base = NULL;
static uint64_t g_bdl_phys = 0; static struct hda_bdl_entry *g_bdl_virt = NULL;
static uint64_t g_data_phys[HDA_BDL_ENTRIES]; static int16_t *g_data_virt[HDA_BDL_ENTRIES];
static int      g_stream_running = 0;
static uint32_t g_stream_next_page = 0;

static int hda_alloc_stream_buffers(void) {
    uint64_t bdl_phys = 0;
    long bdl_virt = sys_dma_alloc(1, &bdl_phys);
    if (bdl_virt < 0) return 0;
    g_bdl_phys = bdl_phys;
    g_bdl_virt = (struct hda_bdl_entry *)bdl_virt;
    memset(g_bdl_virt, 0, 4096);

    // ОДНИМ запросом на все 32 страницы — см. тот же разбор у
    // ac97_alloc_dma() про MAX_MMAP_REGIONS выше.
    uint64_t data_phys = 0;
    long data_virt = sys_dma_alloc(HDA_BDL_ENTRIES, &data_phys);
    if (data_virt < 0) return 0;
    memset((void *)data_virt, 0, (size_t)HDA_BDL_ENTRIES * HDA_DMA_PAGE_SIZE);

    for (int i = 0; i < HDA_BDL_ENTRIES; i++) {
        uint64_t p = data_phys + (uint64_t)i * HDA_DMA_PAGE_SIZE;
        g_data_phys[i] = p;
        g_data_virt[i] = (int16_t *)(data_virt + (long)i * HDA_DMA_PAGE_SIZE);
    }
    return 1;
}

// Программирует BDL как ПОСТОЯННОЕ кольцо из HDA_BDL_ENTRIES страниц
// (каждая с IOC) и запускает поток ОДИН раз — в отличие от бывшего
// hda_run_buffer() (kernel/drivers/sound/hda.c), контроллер сам
// зацикливается на entry 0 по достижении LVI (не сбрасывает RUN), это и
// даёт настоящее непрерывное воспроизведение без щелчков/бипов между
// слайсами.
static void hda_stream_start_once(void) {
    reg_write8(g_sd0_base, HDA_SD_CTL, HDA_SDCTL_SRST);
    for (int i = 0; i < 200 && !(reg_read8(g_sd0_base, HDA_SD_CTL) & HDA_SDCTL_SRST); i++) sys_msleep(1);
    reg_write8(g_sd0_base, HDA_SD_CTL, 0);
    for (int i = 0; i < 200 && (reg_read8(g_sd0_base, HDA_SD_CTL) & HDA_SDCTL_SRST); i++) sys_msleep(1);

    for (int i = 0; i < HDA_BDL_ENTRIES; i++) {
        g_bdl_virt[i].address = g_data_phys[i];
        g_bdl_virt[i].length = HDA_DMA_PAGE_SIZE;
        g_bdl_virt[i].ioc = 1;
    }

    reg_write32(g_sd0_base, HDA_SD_CBL, HDA_DMA_PAGE_SIZE * HDA_BDL_ENTRIES);
    reg_write16(g_sd0_base, HDA_SD_LVI, HDA_BDL_ENTRIES - 1);
    reg_write16(g_sd0_base, HDA_SD_FMT, g_fmt_reg);
    reg_write32(g_sd0_base, HDA_SD_BDPL, (uint32_t)(g_bdl_phys & 0xFFFFFFFFu));
    reg_write32(g_sd0_base, HDA_SD_BDPU, (uint32_t)(g_bdl_phys >> 32));

    hda_verb_long(g_dac_nid, HDA_LVERB_SET_CONV_FORMAT, g_fmt_reg, NULL);
    hda_verb(g_dac_nid, HDA_VERB_SET_STREAM_CHAN, (1u << 4) | 0u, NULL);

    reg_write8(g_sd0_base, HDA_SD_STS, reg_read8(g_sd0_base, HDA_SD_STS));

    uint32_t ctl = reg_read32(g_sd0_base, HDA_SD_CTL);
    ctl &= ~(uint32_t)0xFFFFFFu;
    ctl |= (1u << HDA_SDCTL_STRM_SHIFT) | HDA_SDCTL_RUN;
    reg_write32(g_sd0_base, HDA_SD_CTL, ctl);
}

// Текущая читаемая контроллером страница — тот же принцип, что AC'97 CIV,
// только вычисляется из LPIB (счётчик байт с начала кольца) делением на
// размер страницы, поскольку у HDA нет отдельного регистра "текущий индекс".
static uint32_t hda_current_hw_page(void) {
    uint32_t lpib = reg_read32(g_sd0_base, HDA_SD_LPIB);
    return (lpib / HDA_DMA_PAGE_SIZE) % HDA_BDL_ENTRIES;
}

static void hda_stream_write(const int16_t *samples, uint32_t frames) {
    if (!g_ready || !samples || frames == 0) return;

    const uint32_t frames_per_page = HDA_DMA_PAGE_SIZE / 4;

    if (!g_stream_running) {
        hda_stream_start_once();
    }

    uint32_t produced = 0;
    while (produced < frames) {
        uint32_t chunk = frames - produced;
        if (chunk > frames_per_page) chunk = frames_per_page;

        uint32_t page = g_stream_next_page;

        if (g_stream_running) {
            for (;;) {
                uint32_t hw_page = hda_current_hw_page();
                uint32_t ahead = (page - hw_page + HDA_BDL_ENTRIES) % HDA_BDL_ENTRIES;
                if (ahead != 0 && ahead <= HDA_STREAM_MAX_LOOKAHEAD_PAGES) break;
                __asm__ volatile ("pause");
            }
        }

        memcpy(g_data_virt[page], samples + (uint64_t)produced * 2, (size_t)chunk * 4);
        if (chunk < frames_per_page) {
            // Неполная последняя страница — тишина в хвосте (BDL-запись на
            // эту страницу уже фиксированной длины HDA_DMA_PAGE_SIZE,
            // перепрограммировать entry на каждый вызов не нужно).
            memset(g_data_virt[page] + (size_t)chunk * 2, 0, (size_t)(frames_per_page - chunk) * 4);
        }

        g_stream_next_page = (page + 1) % HDA_BDL_ENTRIES;
        produced += chunk;
    }

    g_stream_running = 1;
}

static void hda_stream_stop(void) {
    if (!g_ready || !g_sd0_base) return;
    reg_write32(g_sd0_base, HDA_SD_CTL, reg_read32(g_sd0_base, HDA_SD_CTL) & ~(uint32_t)HDA_SDCTL_RUN);
    g_stream_running = 0;
    g_stream_next_page = 0;
}

static int hda_init(void) {
    if (!hda_find_controller()) return -1;
    if (!hda_reset_controller()) return -1;

    uint16_t statests = reg_read16(g_mmio, HDA_REG_STATESTS);
    if (statests == 0) { printf("[HDA] No codec responded (STATESTS=0)\n"); return -1; }
    for (int i = 0; i < 15; i++) if (statests & (1u << i)) { g_codec_addr = (uint8_t)i; break; }
    reg_write16(g_mmio, HDA_REG_STATESTS, statests);

    if (!hda_setup_corb_rirb()) return -1;
    g_present = 1;

    if (!hda_discover_codec()) return -1;

    uint32_t dac_amp_caps = hda_get_param(g_dac_nid, HDA_PARAM_OUT_AMP_CAPS);
    g_dac_amp_steps = dac_amp_caps ? (((dac_amp_caps >> 8) & 0x7Fu) + 1) : 0;
    uint32_t pin_amp_caps = hda_get_param(g_pin_nid, HDA_PARAM_OUT_AMP_CAPS);
    g_pin_amp_steps = pin_amp_caps ? (((pin_amp_caps >> 8) & 0x7Fu) + 1) : 0;

    hda_verb(g_afg_nid, HDA_VERB_SET_POWER_STATE, 0, NULL);
    hda_verb(g_dac_nid, HDA_VERB_SET_POWER_STATE, 0, NULL);
    hda_verb(g_pin_nid, HDA_VERB_SET_POWER_STATE, 0, NULL);
    if (g_pin_via_node > 0) hda_verb((uint8_t)g_pin_via_node, HDA_VERB_SET_POWER_STATE, 0, NULL);

    if (g_pin_via_node > 0) hda_select_connection_to_dac((uint8_t)g_pin_via_node, g_dac_nid);
    else hda_select_connection_to_dac(g_pin_nid, g_dac_nid);

    hda_verb(g_pin_nid, HDA_VERB_SET_PIN_CTL, (1u << 6) | (1u << 7), NULL);
    hda_verb(g_pin_nid, 0x70C, 0x02, NULL); // EAPD

    uint32_t oss = HDA_GCAP_OSS(reg_read16(g_mmio, HDA_REG_GCAP));
    uint32_t iss = HDA_GCAP_ISS(reg_read16(g_mmio, HDA_REG_GCAP));
    if (oss == 0) { printf("[HDA] Controller reports no output streams\n"); return -1; }
    g_sd0_base = (uint8_t *)g_mmio + HDA_REG_SD0_BASE + (uint64_t)iss * 0x20;

    if (!hda_alloc_stream_buffers()) { printf("[HDA] Out of memory for stream buffers\n"); return -1; }

    g_ready = 1;
    hda_set_sample_rate(48000);
    hda_set_volume(g_volume);
    printf("[HDA] ready (codec=%u dac=%u pin=%u)\n", g_codec_addr, g_dac_nid, g_pin_nid);
    return 0;
}

/* ============================================================ */
/* Диспетчер бэкендов + демон                                       */
/* ============================================================ */

typedef enum { BACKEND_NONE, BACKEND_HDA, BACKEND_AC97 } backend_t;
static backend_t g_backend = BACKEND_NONE;

static int backend_init(void) {
    if (hda_init() == 0) { g_backend = BACKEND_HDA; return 0; }
    if (ac97_init()) { g_backend = BACKEND_AC97; return 0; }
    g_backend = BACKEND_NONE;
    return -1;
}
static const char *backend_name(void) {
    switch (g_backend) {
        case BACKEND_HDA:  return "HDA";
        case BACKEND_AC97: return "AC'97";
        default:           return "none";
    }
}
static void backend_set_volume(uint8_t v) {
    if (g_backend == BACKEND_HDA) hda_set_volume(v);
    else if (g_backend == BACKEND_AC97) ac97_set_volume(v);
}
static uint8_t backend_get_volume(void) {
    if (g_backend == BACKEND_HDA) return hda_get_volume();
    if (g_backend == BACKEND_AC97) return ac97_get_volume();
    return 0;
}
static uint32_t backend_get_sample_rate(void) {
    if (g_backend == BACKEND_HDA) return hda_get_sample_rate();
    if (g_backend == BACKEND_AC97) return ac97_get_sample_rate();
    return 0;
}
static void backend_stream_write(const int16_t *samples, uint32_t frames) {
    if (g_backend == BACKEND_HDA) hda_stream_write(samples, frames);
    else if (g_backend == BACKEND_AC97) ac97_stream_write(samples, frames);
}
static void backend_stop(void) {
    if (g_backend == BACKEND_HDA) hda_stream_stop();
    else if (g_backend == BACKEND_AC97) ac97_stop();
}

static void handle_control_request(const struct lufira_ipc_msg *msg) {
    struct soundd_request req;
    memcpy(&req, msg->data, sizeof(req));

    struct soundd_reply reply;
    memset(&reply, 0, sizeof(reply));

    switch (req.opcode) {
    case SOUNDD_OP_INFO: {
        reply.result = (g_backend != BACKEND_NONE) ? 1 : 0;
        reply.a[0] = backend_get_volume();
        reply.a[1] = (int32_t)backend_get_sample_rate();
        const char *name = backend_name();
        size_t i = 0;
        for (; name[i] && i < sizeof(reply.str) - 1; i++) reply.str[i] = name[i];
        reply.str[i] = '\0';
        break;
    }
    case SOUNDD_OP_SET_VOLUME:
        backend_set_volume((uint8_t)req.a[0]);
        reply.result = 0;
        break;
    case SOUNDD_OP_STOP:
        backend_stop();
        reply.result = 0;
        break;
    default:
        reply.result = -1;
    }

    sys_ipc_send((long)msg->sender_pid, &reply, sizeof(reply));
}

#define CONSUME_CHUNK_FRAMES 1024

int main(void) {
    if (backend_init() != 0) {
        printf("soundd: no audio controller found, exiting\n");
        return 1;
    }
    printf("soundd: using %s\n", backend_name());

    if (sys_soundd_register() != 0) {
        printf("soundd: registration failed (already running, or not root?)\n");
        return 1;
    }

    static int16_t buf[CONSUME_CHUNK_FRAMES * 2];
    for (;;) {
        // Control-plane — неблокирующе, один запрос за итерацию (клиенты
        // SYS_SOUND_INFO/SET_VOLUME/STOP всё равно шлют не чаще, чем раз в
        // вызов, очередь не копится).
        struct lufira_ipc_msg msg;
        if (sys_ipc_recv(&msg, 0) == 1) {
            handle_control_request(&msg);
        }

        // PCM — короткий таймаут вместо вечной блокировки: иначе
        // control-plane выше не получал бы шанса выполниться, пока не
        // придёт следующий кадр звука.
        long n = sys_sound_consume(buf, CONSUME_CHUNK_FRAMES, 10);
        if (n > 0) backend_stream_write(buf, (uint32_t)n);
    }
}
