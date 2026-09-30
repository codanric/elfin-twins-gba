/*
 * Cartridge real-time clock (Seiko S-3511A compatible, as used by Pokemon
 * Ruby/Sapphire/Emerald and emulated by RTC-capable flashcarts).
 *
 * Protocol reference: agbabi (zlib licence) and GBATEK.
 */
#ifndef RTC_H
#define RTC_H

#include <stdint.h>

typedef struct {
    uint16_t year;   /* 2000-2099 */
    uint8_t month;   /* 1-12 */
    uint8_t day;     /* 1-31 */
    uint8_t wday;    /* 0-6 as stored by the RTC */
    uint8_t hour;    /* 0-23 */
    uint8_t minute;  /* 0-59 */
    uint8_t second;  /* 0-59 */
} rtc_time_t;

enum {
    RTC_OK = 0,
    RTC_POWER_LOST = 1,   /* RTC was reset (battery ran out); time is not meaningful */
    RTC_NOT_FOUND = 2,
};

/* Initialise the RTC. Returns RTC_OK, RTC_POWER_LOST or RTC_NOT_FOUND. */
int rtc_init(void);

/* 1 when rtc_init() found a working RTC. */
int rtc_present(void);

/* Read the current date/time. Returns 0 on success, -1 if the data is invalid. */
int rtc_get(rtc_time_t *t);

/* Set the date/time. */
void rtc_set(const rtc_time_t *t);

/* Raw status register (bit 6 = 24h mode, bit 7 = power lost). */
uint8_t rtc_status(void);

/* Seconds since 2000-01-01 00:00:00. */
uint32_t rtc_to_seconds(const rtc_time_t *t);
void rtc_from_seconds(uint32_t secs, rtc_time_t *t);

#endif
