/*
 * Reliable two-GBA link transport for the Elfin Twins port.
 *
 * The original toy has a one-wire pulse-count protocol. A real GBA link
 * cable already provides a hardware-clocked two-device serial transport, so
 * the GBA port tunnels each Elfin wire transaction through the GBA SIO in
 * normal 32-bit mode at 256 kbit/s. The emulated SPLB20 still sees the
 * original falling edge and pulse timing; only the electrical transport is
 * different.
 *
 * Normal-mode SIO is used instead of RCNT GPIO bit-banging:
 *   - the caller is the GBA SIO master;
 *   - the answering GBA remains the SIO slave;
 *   - every transfer is master-clocked and can carry one application frame
 *     in each direction;
 *   - DATA frames are stop-and-wait acknowledged and retransmitted until
 *     the peer confirms the sequence number.
 *
 * Both ends must run this ROM and be connected with a standard GBA link
 * cable. This transport is intentionally not electrically compatible with
 * the original toy's one-wire protocol.
 */
#ifndef GBA_LINK_H
#define GBA_LINK_H

#include <stdint.h>

/* GBA serial registers. */
#define ELINK_REG_SI0      (*(volatile uint32_t *)0x04000120)
#define ELINK_REG_SIOCNT   (*(volatile uint16_t *)0x04000128)
#define ELINK_REG_RCNT     (*(volatile uint16_t *)0x04000134)

/* SIOCNT bits used here. */
#define ELINK_SIO_START    0x0080
#define ELINK_SIO_32BIT    0x1000
#define ELINK_SIO_MASTER   0x0001
#define ELINK_SIO_SO_HIGH  0x0008
#define ELINK_SIO_SI       0x0004

/* Protocol framing. */
#define ELINK_MAGIC        0xE1
#define ELINK_FRAME_IDLE   0x00
#define ELINK_FRAME_BURST  0x01  /* original link_send, arg = toggle count */
#define ELINK_FRAME_EDGE   0x02  /* answerer "I'm here" falling edge only */
#define ELINK_FRAME_ACK    0xFE

#define ELINK_RXQ_SIZE     4

enum {
    ELINK_ROLE_SLAVE = 0,
    ELINK_ROLE_MASTER = 1
};

typedef struct {
    uint8_t role;
    uint8_t connected;
    uint8_t tx_seq;
    uint8_t last_rx_seq;
    uint8_t last_rx_valid;

    /* One reliable DATA mailbox. */
    uint8_t tx_valid;
    uint8_t tx_seq_pending;
    uint32_t tx_word;

    /* One pending ACK; ACK has priority over DATA. */
    uint8_t ack_valid;
    uint8_t ack_seq;

    /* Received DATA frames waiting for the Elfin bridge. */
    uint32_t rxq[ELINK_RXQ_SIZE];
    uint8_t rx_head;
    uint8_t rx_tail;
} gba_link_t;

#ifdef __cplusplus
extern "C" {
#endif

void gba_link_init(gba_link_t *l);
void gba_link_set_master(gba_link_t *l);
void gba_link_set_slave(gba_link_t *l);

/* Service the hardware transfer state machine. Call from the 2048 Hz IRQ. */
void gba_link_service(gba_link_t *l);

/* Queue one application frame. Returns 1 when accepted. */
int gba_link_send(gba_link_t *l, uint8_t frame_type, uint8_t arg);

/* Pop one received application frame. Returns 1 if available. */
int gba_link_recv(gba_link_t *l, uint8_t *frame_type, uint8_t *arg);

/* Status helpers. */
int gba_link_is_master(const gba_link_t *l);
int gba_link_is_connected(const gba_link_t *l);
int gba_link_idle(const gba_link_t *l);

#ifdef __cplusplus
}
#endif

#endif
