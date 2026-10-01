/*
 * Host regressions for the production Emerald-style gba_link.c.
 *
 * Linux maps a scratch page at the GBA MMIO address so the real transport
 * implementation can be tested without a test-only register abstraction.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "../common/gba_link.h"

#define MMIO_PAGE ((void *)0x04000000u)
#define MMIO_SIZE 0x2000u

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        ++failures; \
    } \
} while (0)

static void clear_mmio(void) {
    memset(MMIO_PAGE, 0, MMIO_SIZE);
}

static uint16_t base_sio(void) {
    return GBA_SIO_MULTI | GBA_SIO_BAUD_115200 | GBA_SIO_IRQ;
}

static void test_init_and_master_start(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    CHECK(GBA_REG_RCNT == 0, "RCNT enters serial mode");
    CHECK((GBA_REG_SIOCNT & (GBA_SIO_MULTI | 3 | GBA_SIO_IRQ)) == base_sio(),
          "SIO initializes multiplayer 115200 + IRQ");
    CHECK(GBA_REG_SIOMLT_SEND == ELINK_HELLO,
          "HELLO is preloaded before first transfer");

    /* Emerald master condition: SD=1, SI=0, ID=0. */
    GBA_REG_SIOCNT = base_sio() | GBA_SIO_MULTI_SD;
    gba_link_service(&l);
    CHECK(l.parent == 1, "SD/SI + ID identify hardware master");
    CHECK((GBA_REG_SIOCNT & GBA_SIO_START) != 0,
          "only master pacing asserts START");

    /* SI=1/ID1 is a slave and must never assert START itself. */
    GBA_REG_SIOCNT = base_sio() | GBA_SIO_MULTI_SI | (1u << 4);
    gba_link_service(&l);
    CHECK(l.parent == 0, "SI/ID1 identifies child");
    CHECK((GBA_REG_SIOCNT & GBA_SIO_START) == 0,
          "child does not clock multiplayer bus");
}

static void test_handshake_then_single_data(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    CHECK(gba_link_send_count(&l, 10), "DATA can queue before peer handshake");

    /* First master transfer: both sides had HELLO preloaded. */
    GBA_REG_SIOCNT = base_sio() | GBA_SIO_MULTI_SD;
    GBA_REG_SIOMULTI1 = ELINK_HELLO;
    gba_link_on_serial(&l);

    CHECK(l.peer_seen == 1 && gba_link_is_ready(&l),
          "peer HELLO establishes logical connection");
    CHECK(GBA_REG_SIOMLT_SEND == (ELINK_DATA | 10),
          "queued Elfin DATA is preloaded immediately after HELLO");

    /* Next completed transfer sends DATA exactly once, then returns to idle. */
    GBA_REG_SIOCNT = base_sio() | GBA_SIO_MULTI_SD;
    GBA_REG_SIOMULTI1 = ELINK_IDLE;
    gba_link_on_serial(&l);
    CHECK(l.frames_tx == 1, "DATA completion counted once");
    CHECK(GBA_REG_SIOMLT_SEND == ELINK_IDLE,
          "sender returns to idle instead of retransmitting/awaiting ACK");

    GBA_REG_SIOCNT = base_sio() | GBA_SIO_MULTI_SD;
    GBA_REG_SIOMULTI1 = ELINK_IDLE;
    gba_link_on_serial(&l);
    CHECK(l.frames_tx == 1, "idle transfer cannot duplicate previous DATA");
}

static void test_child_receive_and_wake(void) {
    gba_link_t l;
    uint8_t value = 0xFF;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    /* Child ID1 receives master's HELLO from slot 0. */
    GBA_REG_SIOCNT = base_sio() | GBA_SIO_MULTI_SI | (1u << 4);
    GBA_REG_SIOMULTI0 = ELINK_HELLO;
    gba_link_on_serial(&l);
    CHECK(l.local_id == 1 && !l.parent && l.peer_seen,
          "child consumes master HELLO from SIOMULTI0");

    /* One DATA word produces one queued edge count and one emulated wake. */
    GBA_REG_SIOCNT = base_sio() | GBA_SIO_MULTI_SI | (1u << 4);
    GBA_REG_SIOMULTI0 = ELINK_DATA | 4;
    gba_link_on_serial(&l);
    CHECK(l.frames_rx == 1, "DATA transfer counted once");
    CHECK(gba_link_wake_pending(&l), "DATA raises emulated PA5 wake");
    CHECK(!gba_link_wake_is_only(&l), "DATA wake has receive payload");
    CHECK(gba_link_recv_count(&l, &value) && value == 4,
          "DATA delivers exact Elfin edge count");
    CHECK(!gba_link_line_low(&l), "consuming last DATA releases virtual line");

    /* BA99 responder event is wake-only and never contaminates recv queue. */
    GBA_REG_SIOCNT = base_sio() | GBA_SIO_MULTI_SI | (1u << 4);
    GBA_REG_SIOMULTI0 = ELINK_WAKE;
    gba_link_on_serial(&l);
    CHECK(l.frames_rx == 2, "WAKE transfer counted");
    CHECK(gba_link_wake_pending(&l) && gba_link_wake_is_only(&l),
          "WAKE is distinguished from DATA");
    CHECK(!gba_link_recv_count(&l, &value),
          "WAKE leaves no link_recv payload");
    gba_link_mark_wake_delivered(&l);
    CHECK(gba_link_line_low(&l),
          "wake-only line remains low while ROM wake handler runs");
    gba_link_release_wake(&l);
    CHECK(!gba_link_line_low(&l), "wake-only event releases explicitly");
}

static void test_simultaneous_words_use_player_slots(void) {
    gba_link_t l;
    uint8_t value;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    /* Master ID0 must ignore its own slot0 and read the child from slot1. */
    GBA_REG_SIOCNT = base_sio() | GBA_SIO_MULTI_SD;
    GBA_REG_SIOMULTI0 = ELINK_DATA | 9; /* own echo; must be ignored */
    GBA_REG_SIOMULTI1 = ELINK_DATA | 7;
    gba_link_on_serial(&l);
    CHECK(gba_link_recv_count(&l, &value) && value == 7,
          "master reads peer slot, not own SIOMULTI0 slot");
}

int main(void) {
    void *p = mmap(MMIO_PAGE, MMIO_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap GBA MMIO scratch page failed: %s\n", strerror(errno));
        return 2;
    }

    test_init_and_master_start();
    test_handshake_then_single_data();
    test_child_receive_and_wake();
    test_simultaneous_words_use_player_slots();

    munmap(MMIO_PAGE, MMIO_SIZE);
    if (failures)
        return 1;

    puts("PASS: Emerald-style GBA multiplayer transport regressions");
    return 0;
}
