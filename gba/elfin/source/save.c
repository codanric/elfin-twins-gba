/*
 * SRAM save - see save.h.
 */
#include <string.h>
#include "save.h"

#define SRAM        ((volatile uint8_t *)0x0E000000)
#define SLOT_SIZE   0x2000
#define SAVE_MAGIC  0x31464C45u   /* "ELF1" */
#define SAVE_VERSION 1

/* Tells flashcart loaders and emulators that this ROM uses SRAM. */
const char save_type_tag[] __attribute__((aligned(4))) = "SRAM_V113";

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t state_size;
    uint32_t seq;
    uint32_t rtc_secs;
    settings_t settings;
    uint32_t checksum;
} save_hdr_t;

static uint32_t checksum(const uint8_t *p, uint32_t n, uint32_t h) {
    /* FNV-1a */
    for (uint32_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void sram_read(uint32_t off, void *dst, uint32_t n) {
    uint8_t *d = dst;
    for (uint32_t i = 0; i < n; i++)
        d[i] = SRAM[off + i];
}

static void sram_write(uint32_t off, const void *src, uint32_t n) {
    const uint8_t *s = src;
    for (uint32_t i = 0; i < n; i++)
        SRAM[off + i] = s[i];
}

static uint32_t hdr_sum(const save_hdr_t *h, const uint8_t *state) {
    save_hdr_t t = *h;
    t.checksum = 0;
    uint32_t s = checksum((const uint8_t *)&t, sizeof(t), 2166136261u);
    return checksum(state, h->state_size, s);
}

static int slot_valid(int slot, save_hdr_t *h, uint8_t *state) {
    sram_read(slot * SLOT_SIZE, h, sizeof(*h));
    if (h->magic != SAVE_MAGIC || h->version != SAVE_VERSION ||
        h->state_size != sizeof(splb20_t))
        return 0;
    sram_read(slot * SLOT_SIZE + sizeof(*h), state, h->state_size);
    return hdr_sum(h, state) == h->checksum;
}

static uint8_t buf[sizeof(splb20_t)] __attribute__((aligned(4), section(".sbss")));
static uint32_t last_seq;
static int last_slot = 1;

int save_load(splb20_t *cpu, settings_t *settings, uint32_t *rtc_secs) {
    save_hdr_t h[2];
    int best = -1;
    for (int s = 0; s < 2; s++) {
        if (slot_valid(s, &h[s], buf)) {
            if (best < 0 || h[s].seq > h[best].seq)
                best = s;
        }
    }
    if (best < 0)
        return 0;
    slot_valid(best, &h[best], buf);
    /*
     * ROM and hook pointers are runtime bindings, not emulated machine state.
     * Saves from another ROM build may contain perfectly valid CPU/RAM state
     * but stale code/data addresses for these fields.
     */
    const uint8_t *rom = cpu->rom;
    splb20_pc_hook_t pc_hook = cpu->pc_hook;
    void *pc_hook_user = cpu->pc_hook_user;
    memcpy(cpu, buf, sizeof(splb20_t));
    cpu->rom = rom;
    cpu->pc_hook = pc_hook;
    cpu->pc_hook_user = pc_hook_user;
    *settings = h[best].settings;
    *rtc_secs = h[best].rtc_secs;
    last_seq = h[best].seq;
    last_slot = best;
    return 1;
}

void save_write(const splb20_t *state, const settings_t *settings, uint32_t rtc_secs) {
    save_hdr_t h;
    h.magic = SAVE_MAGIC;
    h.version = SAVE_VERSION;
    h.state_size = sizeof(splb20_t);
    h.seq = ++last_seq;
    h.rtc_secs = rtc_secs;
    h.settings = *settings;
    h.checksum = hdr_sum(&h, (const uint8_t *)state);
    int slot = last_slot ^ 1;
    /* invalidate first, write state, then the header last */
    SRAM[slot * SLOT_SIZE] = 0;
    sram_write(slot * SLOT_SIZE + sizeof(h), state, sizeof(splb20_t));
    sram_write(slot * SLOT_SIZE + 4, ((const uint8_t *)&h) + 4, sizeof(h) - 4);
    sram_write(slot * SLOT_SIZE, &h, 4);
    last_slot = slot;
}

void save_erase(void) {
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 16; i++)
            SRAM[s * SLOT_SIZE + i] = 0;
    last_seq = 0;
}
