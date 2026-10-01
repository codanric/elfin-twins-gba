/*
 * Reliable GBA-cable transport for Elfin Twins.
 *
 * The original toy protocol is carried as two event types:
 *   WAKE  = the instant PA5 is pulled low (needed for the key interrupt)
 *   COUNT = the subsequent PA5 edge count after the transmitter releases
 *
 * The GBA physical cable uses 16-bit Multi-Player SIO. This implementation
 * intentionally polls SIOCNT from a dedicated timer rather than depending on
 * the SERIAL IRQ.
 */
#ifndef GBA_LINK_H
#define GBA_LINK_H

#include <stdint.h>

#define GBA_REG_SIOMULTI0   (*(volatile uint16_t *)0x04000120)
#define GBA_REG_SIOMULTI1   (*(volatile uint16_t *)0x04000122)
#define GBA_REG_SIOMLT_SEND (*(volatile uint16_t *)0x0400012A)
#define GBA_REG_SIOCNT      (*(volatile uint16_t *)0x04000128)
#define GBA_REG_RCNT        (*(volatile uint16_t *)0x04000134)

#define GBA_SIO_BAUD_115200 0x0003
#define GBA_SIO_MULTI       0x2000
#define GBA_SIO_START       0x0080
#define GBA_SIO_READY       0x0008
#define GBA_SIO_CHILD       0x0004
#define GBA_SIO_ERROR       0x0040

#define ELINK_FRAME_MAGIC   0xA000
#define ELINK_FRAME_MASK    0xF000
#define ELINK_FRAME_COUNT   0x0100
#define ELINK_FRAME_WAKE    0x0200

#define ELINK_RX_WAKE       1
#define ELINK_RX_COUNT      2

#define ELINK_TX_QUEUE_SIZE 4

typedef struct {
    uint8_t enabled;
    uint8_t parent;
    uint8_t ready;
    uint8_t transfer_active;
    uint8_t tx_inflight;

    uint16_t tx_queue[ELINK_TX_QUEUE_SIZE];
    uint8_t tx_head;
    uint8_t tx_count;

    uint16_t rx_word;
    uint8_t rx_pending;
    uint8_t rx_type;
    uint8_t rx_value;

    uint8_t poll_ticks;
    uint32_t edges_rx;
    uint32_t edges_tx;
} gba_link_t;

void gba_link_init(gba_link_t *l);
void gba_link_service(gba_link_t *l);
int gba_link_send_wake(gba_link_t *l);
int gba_link_send_edge_count(gba_link_t *l, uint8_t edges);
int gba_link_recv_event(gba_link_t *l, uint8_t *type, uint8_t *value);
int gba_link_is_ready(const gba_link_t *l);
int gba_link_is_parent(const gba_link_t *l);

#endif
