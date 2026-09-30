#ifndef SAMPLER_H
#define SAMPLER_H

#include <stdint.h>

#define SAMPLE_HZ 8192

typedef struct {
    uint32_t ticks;
    uint16_t last;
    uint32_t edges[4];
    /* burst framing for SC (0) and SD (1) */
    uint32_t cur_burst[2];
    uint32_t quiet[2];
    uint32_t last_burst[2];
    uint8_t cur_burst_own[2];
    uint8_t last_burst_own[2];
    uint32_t burst_count[2];
    /* transmitter */
    uint8_t tx_active;
    uint8_t tx_pin;
    uint8_t tx_level_high;
    uint8_t tx_writes_left;
    uint16_t tx_timer;
    uint32_t bursts_sent;
} sampler_t;

extern volatile sampler_t smp;

void sampler_isr(void) __attribute__((long_call));
void sampler_start_burst(int pin) __attribute__((long_call));

#endif
