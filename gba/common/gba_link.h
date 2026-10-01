/*
 * Reliable GBA-cable transport for Elfin Twins.
 *
 * The original toy protocol is retained above this layer as "edge count"
 * messages. The physical GBA cable uses the GBA's 16-bit Multi-Player SIO
 * mode instead of bit-banging SD as an open-drain wire.
 *
 * One GBA is the SIO parent (the cable determines the terminal role); the
 * parent clocks transfers. Both ends therefore get a bidirectional mailbox:
 * the parent polls, and each side may have one Elfin message queued.
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
#define GBA_SIO_IRQ         0x4000
#define GBA_SIO_START       0x0080
#define GBA_SIO_READY       0x0008
#define GBA_SIO_CHILD       0x0004

#define ELINK_FRAME_MAGIC   0xA000
#define ELINK_FRAME_MASK    0xF000
#define ELINK_FRAME_EDGE    0x0100
#define ELINK_FRAME_IDLE    0x0000

typedef struct {
    uint8_t enabled;
    uint8_t parent;
    uint8_t ready;
    uint8_t transfer_done;

    uint16_t tx_word;
    uint16_t rx_word;
    uint8_t tx_pending;
    uint8_t rx_pending;

    uint16_t poll_ticks;
    uint32_t edges_rx;
    uint32_t edges_tx;
} gba_link_t;

void gba_link_init(gba_link_t *l);
void gba_link_irq(gba_link_t *l);
void gba_link_service(gba_link_t *l);
int gba_link_send_edge_count(gba_link_t *l, uint8_t edges);
int gba_link_recv_edge_count(gba_link_t *l, uint8_t *edges);
int gba_link_is_ready(const gba_link_t *l);
int gba_link_is_parent(const gba_link_t *l);

#endif
