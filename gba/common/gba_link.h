/*
 * Native GBA link protocol for Elfin Twins.
 *
 * The GBA cable is a real four-line serial bus. Do not emulate the toy's
 * one-wire GPIO electrically; use the GBA's hardware multiplayer SIO exactly
 * as ordinary GBA multiplayer software does. The toy's small "edge count"
 * messages are carried as application packets on top of that transport.
 */
#ifndef GBA_LINK_H
#define GBA_LINK_H

#include <stdint.h>

#define GBA_REG_SIOMULTI0    (*(volatile uint16_t *)0x04000120)
#define GBA_REG_SIOMULTI1    (*(volatile uint16_t *)0x04000122)
#define GBA_REG_SIOMULTI2    (*(volatile uint16_t *)0x04000124)
#define GBA_REG_SIOMULTI3    (*(volatile uint16_t *)0x04000126)
#define GBA_REG_SIOMLT_SEND  (*(volatile uint16_t *)0x0400012A)
#define GBA_REG_SIOCNT      (*(volatile uint16_t *)0x04000128)
#define GBA_REG_RCNT        (*(volatile uint16_t *)0x04000134)

/* SIOCNT, multiplayer mode. */
#define GBA_SIO_BAUD_9600   0x0000
#define GBA_SIO_BAUD_38400  0x0001
#define GBA_SIO_BAUD_57600  0x0002
#define GBA_SIO_BAUD_115200 0x0003
#define GBA_SIO_MULTI       0x2000
#define GBA_SIO_START       0x0080
#define GBA_SIO_CHILD       0x0004
#define GBA_SIO_READY       0x0008
#define GBA_SIO_ERROR       0x0040

/* 16-bit application frame: magic | type | sequence | nibble payload. */
#define ELINK_MAGIC         0xA000
#define ELINK_MAGIC_MASK    0xF000
#define ELINK_HELLO         0x0100
#define ELINK_DATA          0x0200
#define ELINK_ACK           0x0300
#define ELINK_PING          0x0400
#define ELINK_RESET         0x0500
#define ELINK_TYPE_MASK     0x0F00

#define ELINK_RX_COUNT      1

#define ELINK_QUEUE_SIZE    8
#define ELINK_SEND_RETRIES  0xFF

typedef struct {
    uint8_t enabled;
    uint8_t hw_enabled;
    uint8_t ready;
    uint8_t parent;
    uint8_t peer_seen;

    uint8_t tx_seq;
    uint8_t tx_inflight;
    uint8_t tx_wait_seq;
    uint8_t tx_wait_value;
    uint8_t tx_queue[ELINK_QUEUE_SIZE];
    uint8_t tx_head;
    uint8_t tx_len;

    uint8_t rx_expected;
    uint8_t rx_queue[ELINK_QUEUE_SIZE];
    uint8_t rx_head;
    uint8_t rx_len;

    uint8_t ack_pending;
    uint8_t ack_seq;

    uint8_t wake_pending;
    uint8_t wake_delivered;

    uint8_t answer_armed;
    uint8_t recv_armed;
    uint16_t last_rx_word;
    uint16_t last_tx_word;
    uint16_t last_sio;

    uint16_t transfer_ticks;
    uint8_t transfer_active;
    uint8_t transfer_started_seen;

    uint32_t frames_rx;
    uint32_t frames_tx;
    uint32_t sio_errors;
} gba_link_t;

void gba_link_init(gba_link_t *l);
void gba_link_set_enabled(gba_link_t *l, int enabled);
void gba_link_service(gba_link_t *l);

int gba_link_send_count(gba_link_t *l, uint8_t count);
int gba_link_recv_count(gba_link_t *l, uint8_t *count);

int gba_link_is_ready(const gba_link_t *l);
int gba_link_is_parent(const gba_link_t *l);
uint16_t gba_link_status(const gba_link_t *l);
int gba_link_wake_pending(const gba_link_t *l);
void gba_link_mark_wake_delivered(gba_link_t *l);

#endif
