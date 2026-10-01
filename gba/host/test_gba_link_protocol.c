/*
 * Host regression tests for gba/common/gba_link.c.
 *
 * Linux can map a scratch page at the GBA MMIO address, so the production
 * transport can be exercised unchanged: no test-only register abstraction is
 * compiled into the ROM.
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
#define MMIO_SIZE 0x2000u

static int failures;

static uint16_t frame(uint16_t type, uint8_t seq, uint8_t value) {
    return (uint16_t)(ELINK_MAGIC | type | ((seq & 0x0F) << 4) | (value & 0x0F));
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        ++failures; \
    } \
} while (0)

static void clear_mmio(void) {
    memset(MMIO_PAGE, 0, MMIO_SIZE);
}

static void set_parent_ready(void) {
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_38400 |
                     GBA_SIO_IRQ | GBA_SIO_READY;
}

static void set_child_ready(void) {
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_38400 |
                     GBA_SIO_IRQ | GBA_SIO_READY | GBA_SIO_CHILD;
}

static void test_queue_before_ready(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    CHECK(gba_link_send_count(&l, 10), "DATA can queue before SIO READY");
    CHECK(l.tx_len == 1, "pre-ready DATA remains queued");

    l.peer_seen = 1; /* handshake already established for this unit test */
    set_parent_ready();
    gba_link_service(&l);

    CHECK((GBA_REG_SIOCNT & GBA_SIO_START) != 0, "parent starts transfer");
    CHECK(GBA_REG_SIOMLT_SEND == frame(ELINK_DATA, 0, 10),
          "queued DATA becomes first ready transfer");
    CHECK(l.tx_inflight == 1, "DATA is held in-flight until ACK");
}

static void test_data_duplicate_and_wake(void) {
    gba_link_t l;
    uint8_t value = 0xFF;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    set_child_ready();
    GBA_REG_SIOMULTI0 = frame(ELINK_DATA, 0, 10);
    gba_link_on_serial(&l);

    CHECK(l.rx_len == 1, "new DATA enqueued once");
    CHECK(gba_link_line_low(&l), "DATA asserts virtual wake line");
    CHECK(!gba_link_wake_is_only(&l), "DATA wake is not wake-only");
    CHECK(gba_link_recv_count(&l, &value) && value == 10,
          "DATA payload delivered to link_recv consumer");
    CHECK(!gba_link_line_low(&l), "consuming final DATA releases wake line");

    /* Replaying seq 0 must ACK again but never enqueue it twice. */
    set_child_ready();
    GBA_REG_SIOMULTI0 = frame(ELINK_DATA, 0, 10);
    gba_link_on_serial(&l);
    CHECK(l.rx_len == 0, "duplicate DATA is suppressed");
    CHECK(GBA_REG_SIOMLT_SEND == frame(ELINK_ACK, 0, 0),
          "duplicate DATA is ACKed again");

    /* Expected sequence is now 1. WAKE wakes only; it is not DATA=1. */
    set_child_ready();
    GBA_REG_SIOMULTI0 = frame(ELINK_WAKE, 1, 0);
    gba_link_on_serial(&l);
    CHECK(l.rx_len == 0, "WAKE does not poison DATA queue");
    CHECK(gba_link_wake_pending(&l), "WAKE raises one pending emulated wake");
    CHECK(gba_link_wake_is_only(&l), "WAKE is tagged wake-only");
    CHECK(gba_link_line_low(&l), "WAKE holds virtual PA5 low");

    gba_link_mark_wake_delivered(&l);
    CHECK(!gba_link_wake_pending(&l), "delivered WAKE does not retrigger");
    CHECK(gba_link_line_low(&l), "delivered WAKE remains low for ROM handler");
    gba_link_release_wake(&l);
    CHECK(!gba_link_line_low(&l), "WAKE releases explicitly after handler");
}

static void test_child_ack_preload_persists(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    set_child_ready();
    GBA_REG_SIOMULTI0 = frame(ELINK_DATA, 0, 4);
    gba_link_on_serial(&l);

    uint16_t ack = frame(ELINK_ACK, 0, 0);
    CHECK(GBA_REG_SIOMLT_SEND == ack, "child preloads ACK immediately");
    CHECK(l.slave_word_loaded, "child marks preloaded word owned");

    /* The slower timer service must not replace that ACK with PING. */
    set_child_ready();
    gba_link_service(&l);
    CHECK(GBA_REG_SIOMLT_SEND == ack, "timer preserves child ACK preload");
}

static void test_wake_tx_and_ack(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);
    l.peer_seen = 1;

    CHECK(gba_link_send_wake(&l), "wake event queues");
    set_parent_ready();
    gba_link_service(&l);
    CHECK(GBA_REG_SIOMLT_SEND == frame(ELINK_WAKE, 0, 0),
          "wake event uses dedicated WAKE frame");

    set_parent_ready();
    GBA_REG_SIOMULTI1 = frame(ELINK_ACK, 0, 0);
    gba_link_on_serial(&l);
    CHECK(!l.tx_inflight, "ACK completes wake event");
    CHECK(l.frames_tx == 1, "completed wake counted once");
}

int main(void) {
    void *p = mmap(MMIO_PAGE, MMIO_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap GBA MMIO scratch page failed: %s\n", strerror(errno));
        return 2;
    }

    test_queue_before_ready();
    test_data_duplicate_and_wake();
    test_child_ack_preload_persists();
    test_wake_tx_and_ack();

    munmap(MMIO_PAGE, MMIO_SIZE);
    if (failures)
        return 1;

    puts("PASS: native GBA link transport protocol regressions");
    return 0;
}
