#include "netrand.h"
#include <lufira/syscall.h>
#include <string.h>
#include "sha256.h"

static inline uint64_t netrand_read_tsc(void) {
    uint32_t lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// См. комментарий в netrand.h — тот же приём, что tls_gather_random()
// (tls.c), свой отдельный пул.
static void netrand_bytes(uint8_t *out, uint32_t len) {
    static uint8_t pool[SHA256_DIGEST_SIZE];
    static uint64_t counter = 0;

    uint32_t done = 0;
    while (done < len) {
        uint8_t mix[SHA256_DIGEST_SIZE + 8 * 5];
        memcpy(mix, pool, SHA256_DIGEST_SIZE);

        uint64_t samples[5];
        samples[0] = netrand_read_tsc();
        samples[1] = (uint64_t)sys_gettick();
        samples[2] = netrand_read_tsc();
        samples[3] = counter++;
        samples[4] = netrand_read_tsc();
        for (int i = 0; i < 5; i++) {
            for (int b = 0; b < 8; b++) {
                mix[SHA256_DIGEST_SIZE + i * 8 + b] = (uint8_t)(samples[i] >> (b * 8));
            }
        }

        sha256(mix, sizeof(mix), pool);

        uint32_t take = len - done;
        if (take > SHA256_DIGEST_SIZE) take = SHA256_DIGEST_SIZE;
        memcpy(out + done, pool, take);
        done += take;
    }
}

uint32_t netrand_u32(void) {
    uint8_t b[4];
    netrand_bytes(b, sizeof(b));
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

uint16_t netrand_u16(void) {
    uint8_t b[2];
    netrand_bytes(b, sizeof(b));
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}
