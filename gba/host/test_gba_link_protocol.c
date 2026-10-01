/*
 * Host regression tests for gba/common/gba_link.c.
 *
 * Linux maps a scratch page at the GBA MMIO address so the production
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

#include "gba_link.h"

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

static void set_master_bus(uint16_t extra) {
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_115200 |
                     GBA_SIO_IRQ | GBA_SIO_MULTI_SD | extra;
}

static void set_child_bus(uint16_t extra) {
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_115200 |
                     GBA_SIO_IRQ | GBA_SIO_MULTI_SD | GBA_SIO_MULTI_SI |
                     0x0010 | extra;
}

static void master_handshake(gba_link_t *l) {
    set_master_bus(0);
    gba_link_service(l);
    GBA_REG_SIOCNT &= (uint16_t)~GBA_SIO_START;
    GBA_REG_SIOMULTI0 = ELINK_SLAVE_HANDSHAKE;
    GBA_REG_SIOMULTI1 = ELINK_SLAVE_HANDSHAKE;
    GBA_REG_SIOMULTI2 = 0xFFFF;
    GBA_REG_SIOMULTI3 = 0xFFFF;
    gba_link_on_serial(l);

    set_master_bus(0);
    gba_link_service(l);
    GBA_REG_SIOCNT &= (uint16_t)~GBA_SIO_START;
    GBA_REG_SIOMULTI0 = ELINK_MASTER_HANDSHAKE;
    GBA_REG_SIOMULTI1 = ELINK_SLAVE_HANDSHAKE;
    GBA_REG_SIOMULTI2 = 0xFFFF;
    GBA_REG_SIOMULTI3 = 0xFFFF;
    gba_link_on_serial(l);
}

static void child_handshake(gba_link_t *l) {
    set_child_bus(0);
    gba_link_service(l);
    GBA_REG_SIOMULTI0 = ELINK_SLAVE_HANDSHAKE;
    GBA_REG_SIOMULTI1 = ELINK_SLAVE_HANDSHAKE;
    GBA_REG_SIOMULTI2 = 0xFFFF;
    GBA_REG_SIOMULTI3 = 0xFFFF;
    gba_link_on_serial(l);

    set_child_bus(0);
    gba_link_service(l);
    GBA_REG_SIOMULTI0 = ELINK_MASTER_HANDSHAKE;
    GBA_REG_SIOMULTI1 = ELINK_SLAVE_HANDSHAKE;
    GBA_REG_SIOMULTI2 = 0xFFFF;
    GBA_REG_SIOMULTI3 = 0xFFFF;
    gba_link_on_serial(l);
}

static void test_hardware_handshake_and_locked_role(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    CHECK(GBA_REG_RCNT == 0, "RCNT enters serial/multiplayer mode");
    CHECK(GBA_REG_SIOCNT ==
          (GBA_SIO_MULTI | GBA_SIO_BAUD_115200 | GBA_SIO_IRQ),
          "SIOCNT initialized to multiplayer 115200 + SERIAL IRQ");
    CHECK(GBA_REG_SIOMLT_SEND == ELINK_SLAVE_HANDSHAKE,
          "slave handshake is preloaded before first transfer");

    set_master_bus(0);
    gba_link_service(&l);
    CHECK(l.parent == 1, "SD=1 SI=0 ID0 elects hardware master");
    CHECK((GBA_REG_SIOCNT & GBA_SIO_START) != 0,
          "hardware master starts handshake transfer");

    GBA_REG_SIOCNT &= (uint16_t)~GBA_SIO_START;
    GBA_REG_SIOMULTI0 = ELINK_SLAVE_HANDSHAKE;
    GBA_REG_SIOMULTI1 = ELINK_SLAVE_HANDSHAKE;
    GBA_REG_SIOMULTI2 = GBA_REG_SIOMULTI3 = 0xFFFF;
    gba_link_on_serial(&l);
    CHECK(!gba_link_is_ready(&l), "one player-count observation is not enough");
    CHECK(l.tx_word == ELINK_MASTER_HANDSHAKE,
          "elected master advertises master handshake next");

    set_master_bus(0);
    GBA_REG_SIOMULTI0 = ELINK_MASTER_HANDSHAKE;
    GBA_REG_SIOMULTI1 = ELINK_SLAVE_HANDSHAKE;
    gba_link_on_serial(&l);
    CHECK(gba_link_is_ready(&l), "stable second handshake establishes bus");
    CHECK(l.parent == 1, "master role stays locked after handshake");

    /* Emerald no longer re-evaluates SD after connection establishment. */
    GBA_REG_SIOCNT = GBA_SIO_MULTI | GBA_SIO_BAUD_115200 | GBA_SIO_IRQ;
    gba_link_service(&l);
    CHECK(l.parent == 1, "transient SD low does not erase locked master role");
    CHECK((GBA_REG_SIOCNT & GBA_SIO_START) != 0,
          "locked master continues offering transfers after handshake");
}

static void test_queued_data_waits_for_bus_then_completes(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);

    CHECK(gba_link_send_count(&l, 10), "DATA can queue during bus handshake");
    CHECK(l.tx_len == 1, "queued DATA is retained during handshake");

    master_handshake(&l);
    CHECK(gba_link_is_ready(&l), "master handshake completes");
    CHECK(l.tx_word == (uint16_t)(ELINK_DATA | 10),
          "first queued Elfin DATA is preloaded after bus handshake");

    set_master_bus(0);
    GBA_REG_SIOMULTI0 = (uint16_t)(ELINK_DATA | 10);
    GBA_REG_SIOMULTI1 = ELINK_IDLE;
    GBA_REG_SIOMULTI2 = GBA_REG_SIOMULTI3 = 0xFFFF;
    gba_link_on_serial(&l);
    CHECK(l.frames_tx == 1, "SERIAL completion counts DATA exactly once");
    CHECK(l.last_tx_word == (uint16_t)(ELINK_DATA | 10),
          "completed DATA is recorded");
    CHECK(l.tx_word == ELINK_IDLE, "next empty transfer preloads IDLE");
}

static void test_receive_data_and_wake(void) {
    gba_link_t l;
    uint8_t value = 0xFF;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);
    child_handshake(&l);

    CHECK(gba_link_is_ready(&l), "child handshake completes");
    CHECK(!l.parent && l.local_id == 1, "child keeps ID1 non-master role");

    set_child_bus(0);
    GBA_REG_SIOMULTI0 = (uint16_t)(ELINK_DATA | 4);
    GBA_REG_SIOMULTI1 = ELINK_IDLE;
    GBA_REG_SIOMULTI2 = GBA_REG_SIOMULTI3 = 0xFFFF;
    gba_link_on_serial(&l);

    CHECK(l.frames_rx == 1, "received DATA counted once");
    CHECK(l.rx_len == 1, "received DATA queued for ROM");
    CHECK(gba_link_line_low(&l), "DATA asserts synthetic PA5 low");
    CHECK(!gba_link_wake_is_only(&l), "DATA wake has receive payload");
    CHECK(gba_link_recv_count(&l, &value) && value == 4,
          "ROM consumer receives exact Elfin edge count");
    CHECK(!gba_link_line_low(&l), "consuming last DATA releases PA5");

    set_child_bus(0);
    GBA_REG_SIOMULTI0 = ELINK_WAKE;
    GBA_REG_SIOMULTI1 = ELINK_IDLE;
    gba_link_on_serial(&l);
    CHECK(l.frames_rx == 2, "WAKE completion counted");
    CHECK(l.rx_len == 0, "WAKE never contaminates DATA queue");
    CHECK(gba_link_wake_pending(&l) && gba_link_wake_is_only(&l),
          "WAKE raises wake-only synthetic PA5 event");
    gba_link_mark_wake_delivered(&l);
    CHECK(gba_link_line_low(&l), "delivered WAKE stays low during ROM handler");
    gba_link_release_wake(&l);
    CHECK(!gba_link_line_low(&l), "WAKE releases explicitly");
}

static void test_error_requeues_inflight_payload(void) {
    gba_link_t l;
    clear_mmio();
    gba_link_init(&l);
    gba_link_set_enabled(&l, 1);
    CHECK(gba_link_send_count(&l, 7), "DATA queues");
    master_handshake(&l);
    CHECK(l.tx_word == (uint16_t)(ELINK_DATA | 7), "DATA is in flight");

    set_master_bus(GBA_SIO_ERROR);
    gba_link_on_serial(&l);
    CHECK(l.sio_errors == 1, "hardware SIO error is counted");
    CHECK(!gba_link_is_ready(&l), "hardware error returns to bus handshake");
    CHECK(l.tx_len == 1, "in-flight Elfin DATA is requeued after error");
    CHECK(l.tx_word == ELINK_SLAVE_HANDSHAKE,
          "error restart advertises handshake rather than stale DATA");
}

int main(void) {
    void *p = mmap(MMIO_PAGE, MMIO_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "mmap GBA MMIO scratch page failed: %s\n", strerror(errno));
        return 2;
    }

    test_hardware_handshake_and_locked_role();
    test_queued_data_waits_for_bus_then_completes();
    test_receive_data_and_wake();
    test_error_requeues_inflight_payload();

    munmap(MMIO_PAGE, MMIO_SIZE);
    if (failures)
        return 1;

    puts("PASS: Emerald-style native GBA link transport regressions");
    return 0;
}
