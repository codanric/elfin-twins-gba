/*
 * Elfin Twins link transport using the GBA multiplayer SIO hardware.
 *
 * The low-level sequencing intentionally follows the proven cable model used
 * by retail GBA titles such as Pokemon Emerald:
 *   - RCNT = 0, multiplayer mode, 115200 bps, SERIAL IRQ enabled
 *   - master is identified from SD/SI terminal bits + multiplayer ID
 *   - only the hardware master starts transfers
 *   - SERIAL IRQ is the authoritative transfer-completion path
 *   - each transfer carries one already-loaded 16-bit word per GBA
 *
 * Elfin itself only needs a tiny logical transport, so no ACK, retry,
 * sequence-number or polling protocol is layered on top of multiplayer SIO.
 */
#ifndef GBA_LINK_H
#define GBA_LINK_H

#include <stdint.h>

#define GBA_REG_SIOMULTI0    (*(volatile uint16_t *)0x04000120)
#define GBA_REG_SIOMULTI1    (*(volatile uint16_t *)0x04000122)
#define GBA_REG_SIOMULTI2    (*(volatile uint16_t *)0x04000124)
#define GBA_REG_SIOMULTI3    (*(volatile uint16_t *)0x04000126)
#define GBA_REG_SIOCNT       (*(volatile uint16_t *)0x04000128)
#define GBA_REG_SIOMLT_SEND  (*(volatile uint16_t *)0x0400012A)
#define GBA_REG_RCNT         (*(volatile uint16_t *)0x04000134)

/* SIOCNT multiplayer-mode fields. Names match the hardware terminals used by
 * Pokemon Emerald's IsSioMultiMaster()/CheckMasterOrSlave(), rather than
 * treating SI/SD as generic "child"/"ready" flags. */
#define GBA_SIO_BAUD_115200  0x0003
#define GBA_SIO_MULTI_SI     0x0004
#define GBA_SIO_MULTI_SD     0x0008
#define GBA_SIO_ID_MASK      0x0030
#define GBA_SIO_ERROR        0x0040
#define GBA_SIO_START        0x0080
#define GBA_SIO_MULTI        0x2000
#define GBA_SIO_IRQ          0x4000

/* One 16-bit logical word per completed multiplayer transfer. */
#define ELINK_IDLE           0x0000
#define ELINK_HELLO          0xE100
#define ELINK_DATA           0xE200
#define ELINK_WAKE           0xE300
#define ELINK_KIND_MASK      0xFF00
#define ELINK_VALUE_MASK     0x000F

#define ELINK_QUEUE_SIZE     8
#define ELINK_TX_WAKE        0x80

typedef struct {
    uint8_t enabled;
    uint8_t hw_enabled;
    uint8_t ready;          /* logical peer handshake seen */
    uint8_t parent;         /* hardware multiplayer master */
    uint8_t peer_seen;
    uint8_t local_id;

    uint8_t tx_queue[ELINK_QUEUE_SIZE];
    uint8_t tx_head;
    uint8_t tx_len;
    uint8_t rx_queue[ELINK_QUEUE_SIZE];
    uint8_t rx_head;
    uint8_t rx_len;

    uint8_t wake_pending;
    uint8_t wake_delivered;
    uint8_t wake_only;

    /* ROM-hook state lives here so it resets with the cable session. */
    uint8_t answer_armed;
    uint8_t recv_armed;

    /* Word currently preloaded in SIOMLT_SEND and diagnostics. */
    uint16_t tx_word;
    uint16_t last_rx_word;
    uint16_t last_tx_word;
    uint16_t last_sio;

    uint32_t frames_rx;
    uint32_t frames_tx;
    uint32_t sio_errors;
} gba_link_t;

void gba_link_init(gba_link_t *l);
void gba_link_set_enabled(gba_link_t *l, int enabled);

/* Called by the master pacing timer. Non-masters never set SIO_START. */
void gba_link_service(gba_link_t *l);

/* Called directly from the GBA SERIAL IRQ after a multiplayer transfer. */
void gba_link_on_serial(gba_link_t *l);

int gba_link_send_count(gba_link_t *l, uint8_t count);
int gba_link_send_wake(gba_link_t *l);
int gba_link_recv_count(gba_link_t *l, uint8_t *count);

int gba_link_is_ready(const gba_link_t *l);
int gba_link_is_parent(const gba_link_t *l);
uint16_t gba_link_status(const gba_link_t *l);

int gba_link_wake_pending(const gba_link_t *l);
int gba_link_line_low(const gba_link_t *l);
int gba_link_wake_is_only(const gba_link_t *l);
void gba_link_mark_wake_delivered(gba_link_t *l);
void gba_link_release_wake(gba_link_t *l);

#endif
