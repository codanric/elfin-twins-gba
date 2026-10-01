/*
 * Host regression tests for gba/common/gba_link.c.
 *
 * Linux can map a scratch page at the GBA MMIO address, so the production
 * transport runs unchanged. These tests model completed multiplayer transfers
 * by writing the same SIOCNT/SIOMULTI state the GBA hardware exposes.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../common/gba_link.h"

#define MMIO_PAGE ((void *)0x04000000u)
#define MMIO_SIZE 0x3000u

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

/* Physical master: SD=1, SI=0, multiplayer ID=0. */
static void set_master_bus(uint16_t extra) {
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_115200 |
                     GBA_SIO_IRQ | GBA_SIO_MULTI_SD | extra;
}

/* First child: SD=1, SI=1, multiplayer ID=1. */
static void set_child_bus(uint16_t extra) {
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_115200 |
                     GBA_SIO_IRQ | GBA_SIO_MULTI_SD | GBA_SIO_MULTI_SI |
                     0x0010 | extra;
}

static void test_hardware_init_and_role(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    CHECK(GBA_REG_RCNT == 0, "RCNT enters serial/multiplayer mode");
    CHECK(GBA_REG_SIOCNT ==
          (GBA_SIO_MULTI | GBA_SIO_BAUD_115200 | GBA_SIO_IRQ),
          "SIOCNT initialized to multiplayer 115200 + SERIAL IRQ");
    CHECK(GBA_REG_SIOMLT_SEND == ELINK_HELLO,
          "HELLO is preloaded before first transfer");

    set_master_bus(0);
    gba_link_service(&l);
    CHECK(l.parent == 1, "SD=1 SI=0 ID0 is hardware master");
    CHECK(l.local_id == 0, "master has player ID 0");
    CHECK((GBA_REG_SIOCNT & GBA_SIO_START) != 0,
          "hardware master starts transfer");

    set_child_bus(0);
    gba_link_service(&l);
    CHECK(l.parent == 0, "ID1/SI child is not hardware master");
    CHECK(l.local_id == 1, "child samples player ID 1");
    CHECK((GBA_REG_SIOCNT & GBA_SIO_START) == 0,
          "child never starts transfer");
}

static void test_hello_then_queued_data(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    CHECK(gba_link_send_count(&l, 10), "DATA queues before peer handshake");
    CHECK(l.tx_len == 1, "pre-ready DATA remains queued");
    CHECK(l.tx_word == ELINK_HELLO, "first physical word remains HELLO");

    /* Master completes first transfer; child slot contains HELLO. */
    set_master_bus(0);
    GBA_REG_SIOMULTI0 = ELINK_HELLO; /* our own just-sent word */
    GBA_REG_SIOMULTI1 = ELINK_HELLO; /* peer */
    GBA_REG_SIOMULTI2 = 0xFFFF;
    GBA_REG_SIOMULTI3 = 0xFFFF;
    gba_link_on_serial(&l);

    CHECK(l.peer_seen && l.ready, "peer HELLO establishes logical readiness");
    CHECK(l.last_rx_word == ELINK_HELLO, "HELLO recorded as last receive");
    CHECK(l.tx_word == (uint16_t)(ELINK_DATA | 10),
          "queued DATA preloaded immediately after HELLO completion");
    CHECK(GBA_REG_SIOMLT_SEND == (uint16_t)(ELINK_DATA | 10),
          "next hardware transfer is preloaded with DATA");

    /* Complete DATA transfer. Peer is idle; our DATA must count exactly once. */
    set_master_bus(0);
    GBA_REG_SIOMULTI0 = (uint16_t)(ELINK_DATA | 10);
    GBA_REG_SIOMULTI1 = ELINK_IDLE;
    gba_link_on_serial(&l);
    CHECK(l.frames_tx == 1, "DATA completion counted exactly once");
    CHECK(l.last_tx_word == (uint16_t)(ELINK_DATA | 10),
          "last TX records completed DATA");
    CHECK(l.tx_word == ELINK_IDLE, "no queued event preloads IDLE");

    set_master_bus(0);
    GBA_REG_SIOMULTI0 = ELINK_IDLE;
    GBA_REG_SIOMULTI1 = ELINK_IDLE;
    gba_link_on_serial(&l);
    CHECK(l.frames_tx == 1, "later IDLE transfer does not recount DATA");
}

static void test_receive_data_and_wake(void) {
    gba_link_t l;
    uint8_t value = 0xFF;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    /* Child receives DATA from master slot 0. */
    set_child_bus(0);
    GBA_REG_SIOMULTI0 = (uint16_t)(ELINK_DATA | 4);
    GBA_REG_SIOMULTI1 = ELINK_HELLO; /* own slot, skipped by local_id */
    GBA_REG_SIOMULTI2 = 0xFFFF;
    GBA_REG_SIOMULTI3 = 0xFFFF;
    gba_link_on_serial(&l);

    CHECK(l.peer_seen && l.ready, "received DATA also establishes peer");
    CHECK(l.frames_rx == 1, "received DATA counted");
    CHECK(l.rx_len == 1, "received DATA queued for ROM");
    CHECK(gba_link_line_low(&l), "DATA asserts synthetic PA5 low");
    CHECK(!gba_link_wake_is_only(&l), "DATA wake is not wake-only");
    CHECK(gba_link_recv_count(&l, &value) && value == 4,
          "ROM consumer receives DATA payload");
    CHECK(!gba_link_line_low(&l), "consuming final DATA releases PA5");

    /* WAKE is reliable as a hardware word but never enters DATA queue. */
    set_child_bus(0);
    GBA_REG_SIOMULTI0 = ELINK_WAKE;
    GBA_REG_SIOMULTI1 = ELINK_IDLE;
    gba_link_on_serial(&l);

    CHECK(l.frames_rx == 2, "WAKE completion counted");
    CHECK(l.rx_len == 0, "WAKE does not poison DATA queue");
    CHECK(gba_link_wake_pending(&l), "WAKE raises emulated interrupt event");
    CHECK(gba_link_wake_is_only(&l), "WAKE tagged wake-only");
    CHECK(gba_link_line_low(&l), "WAKE holds synthetic PA5 low");

    gba_link_mark_wake_delivered(&l);
    CHECK(!gba_link_wake_pending(&l), "delivered WAKE does not retrigger");
    CHECK(gba_link_line_low(&l), "WAKE remains low for ROM wake handler");
    gba_link_release_wake(&l);
    CHECK(!gba_link_line_low(&l), "WAKE releases after ROM handler");
}

static void test_wake_transmit_once(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    CHECK(gba_link_send_wake(&l), "WAKE event queues before handshake");

    /* First completion receives HELLO and preloads queued WAKE. */
    set_master_bus(0);
    GBA_REG_SIOMULTI0 = ELINK_HELLO;
    GBA_REG_SIOMULTI1 = ELINK_HELLO;
    gba_link_on_serial(&l);
    CHECK(l.tx_word == ELINK_WAKE, "queued WAKE becomes next hardware word");

    set_master_bus(0);
    GBA_REG_SIOMULTI0 = ELINK_WAKE;
    GBA_REG_SIOMULTI1 = ELINK_IDLE;
    gba_link_on_serial(&l);
    CHECK(l.frames_tx == 1, "WAKE completion counted once");
    CHECK(l.last_tx_word == ELINK_WAKE, "last TX records WAKE");
    CHECK(l.tx_word == ELINK_IDLE, "WAKE is not retransmitted without requeue");
}

static void test_sio_error_state(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    l.peer_seen = 1;
    l.ready = 1;
    set_master_bus(GBA_SIO_ERROR);
    GBA_REG_SIOMULTI0 = ELINK_IDLE;
    GBA_REG_SIOMULTI1 = 0xFFFF;
    GBA_REG_SIOMULTI2 = 0xFFFF;
    GBA_REG_SIOMULTI3 = 0xFFFF;
    gba_link_on_serial(&l);

    CHECK(l.sio_errors == 1, "hardware SIO error is counted");
    CHECK(!l.peer_seen && !l.ready, "SIO error drops logical peer readiness");
    CHECK(l.tx_word == ELINK_HELLO, "after error next word re-advertises HELLO");
}

int main(void) {
    void *p = mmap(MMIO_PAGE, MMIO_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap GBA MMIO scratch page failed: %s\n", strerror(errno));
        return 2;
    }

    test_hardware_init_and_role();
    test_hello_then_queued_data();
    test_receive_data_and_wake();
    test_wake_transmit_once();
    test_sio_error_state();

    munmap(MMIO_PAGE, MMIO_SIZE);
    if (failures)
        return 1;

    puts("PASS: Emerald-style native GBA link transport regressions");
    return 0;
}
