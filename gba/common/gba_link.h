/*
 * Reliable GBA-cable transport for Elfin Twins.
 *
 * The original toy protocol is retained above this layer as "edge count"
 * messages. The physical GBA cable uses the GBA's 16-bit Multi-Player SIO
 * mode instead of bit-banging a cable pin.
 *
 * This transport deliberately does NOT depend on the GBA SERIAL interrupt.
 * libtonc's SERIAL dispatcher is known to occasionally lose serial IRQs on
 * real hardware; a missed IRQ is fatal for a mailbox-style slave transmitter.
 * A dedicated 61.04 us timer polls SIOCNT instead.
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
#define ELINK_FRAME_EDGE    0x0100
#define ELINK_FRAME_IDLE    0x0000

typedef struct {
    uint8_t enabled;
    uint8_t parent;
    uint8_t ready;
    uint8_t transfer_active;

    uint16_t tx_word;
    uint16_t rx_word;
    uint8_t tx_pending;
    uint8_t rx_pending;

    uint8_t poll_ticks;
    uint32_t edges_rx;
    uint32_t edges_tx;
} gba_link_t;

void gba_link_init(gba_link_t *l);
void gba_link_service(gba_link_t *l);
int gba_link_send_edge_count(gba_link_t *l, uint8_t edges);
int gba_link_recv_edge_count(gba_link_t *l, uint8_t *edges);
int gba_link_is_ready(const gba_link_t *l);
int gba_link_is_parent(const gba_link_t *l);

#endif
