/*
 * Cartridge RTC driver. See rtc.h.
 *
 * The RTC sits on the cartridge GPIO port:
 *   0x080000C4  data      bit0 = SCK, bit1 = SIO, bit2 = CS
 *   0x080000C6  direction (1 = output)
 *   0x080000C8  control   (1 = GPIO readable)
 * Commands are 8 bits, MSB first: 0110 ccc r (r = 1 for read).
 * Data bytes are sent/received LSB first. Values are BCD.
 */
#include "rtc.h"

#define GPIO_DATA (*(volatile uint16_t *)0x080000C4)
#define GPIO_DIR  (*(volatile uint16_t *)0x080000C6)
#define GPIO_CTRL (*(volatile uint16_t *)0x080000C8)
#define REG_IME   (*(volatile uint16_t *)0x04000208)

#define SCK 1
#define SIO 2
#define CS  4

#define CMD_RESET     0x60
#define CMD_STATUS_W  0x62
#define CMD_STATUS_R  0x63
#define CMD_DATETIME_W 0x64
#define CMD_DATETIME_R 0x65

#define STAT_24H   0x40
#define STAT_POWER 0x80

/* Nintendo's RTC library signature. Emulators (mGBA, NanoBoyAdvance...) and
 * several flashcart loaders scan for it to decide whether to provide an RTC. */
const char rtc_signature[] __attribute__((aligned(4))) = "SIIRTC_V001";

static int present;

static void begin(void) {
    GPIO_DIR = CS | SCK;
    GPIO_DATA = SCK;
    GPIO_DATA = CS | SCK;
}

static void end(void) {
    GPIO_DATA = SCK;
    GPIO_DATA = SCK;
}

static void send_cmd(uint8_t cmd) {
    GPIO_DIR = CS | SIO | SCK;
    for (int i = 7; i >= 0; i--) {
        uint16_t b = ((cmd >> i) & 1) << 1;
        GPIO_DATA = CS | b;
        GPIO_DATA = CS | b;
        GPIO_DATA = CS | b;
        GPIO_DATA = CS | b | SCK;
    }
}

static void send_bits(uint32_t data, int len) {
    GPIO_DIR = CS | SIO | SCK;
    for (int i = 0; i < len; i++) {
        uint16_t b = ((data >> i) & 1) << 1;
        GPIO_DATA = CS | b;
        GPIO_DATA = CS | b;
        GPIO_DATA = CS | b;
        GPIO_DATA = CS | b | SCK;
    }
}

static uint32_t recv_bits(int len) {
    uint32_t data = 0;
    GPIO_DIR = CS | SCK;
    for (int i = 0; i < len; i++) {
        GPIO_DATA = CS;
        GPIO_DATA = CS;
        GPIO_DATA = CS;
        GPIO_DATA = CS;
        GPIO_DATA = CS | SCK;
        data |= (uint32_t)((GPIO_DATA & SIO) >> 1) << i;
    }
    return data;
}

static void settle(void) {
    for (volatile int i = 0; i < 2000; i++)
        ;
}

static uint8_t bcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0xF)); }
static uint8_t tobcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int bcd_ok(uint8_t v) { return (v & 0xF) < 10 && (v >> 4) < 10; }

uint8_t rtc_status(void) {
    uint16_t ime = REG_IME;
    REG_IME = 0;
    begin();
    send_cmd(CMD_STATUS_R);
    uint8_t s = (uint8_t)recv_bits(8);
    end();
    REG_IME = ime;
    return s;
}

static void write_status(uint8_t s) {
    begin();
    send_cmd(CMD_STATUS_W);
    send_bits(s, 8);
    end();
    settle();
}

static void reset_rtc(void) {
    begin();
    send_cmd(CMD_RESET);
    end();
    settle();
}

static int read_raw(uint8_t out[7]) {
    uint16_t ime = REG_IME;
    REG_IME = 0;
    begin();
    send_cmd(CMD_DATETIME_R);
    uint32_t date = recv_bits(32);
    uint32_t time = recv_bits(24);
    end();
    REG_IME = ime;
    out[0] = date & 0xFF;          /* year */
    out[1] = (date >> 8) & 0x1F;   /* month */
    out[2] = (date >> 16) & 0x3F;  /* day */
    out[3] = (date >> 24) & 0x07;  /* weekday */
    out[4] = time & 0x3F;          /* hour (bit 7 = PM flag in 12h mode) */
    out[5] = (time >> 8) & 0x7F;   /* minute */
    out[6] = (time >> 16) & 0x7F;  /* second */
    if ((time & 0x80) && !(out[4] >= 0x12)) {
        /* 12h mode PM flag: convert to 24h */
        uint8_t h = bcd(out[4]) + 12;
        out[4] = tobcd(h >= 24 ? h - 12 : h);
    }
    return 0;
}

int rtc_get(rtc_time_t *t) {
    uint8_t r[7];
    read_raw(r);
    for (int i = 0; i < 7; i++)
        if (!bcd_ok(r[i]))
            return -1;
    t->year = 2000 + bcd(r[0]);
    t->month = bcd(r[1]);
    t->day = bcd(r[2]);
    t->wday = bcd(r[3]);
    t->hour = bcd(r[4]);
    t->minute = bcd(r[5]);
    t->second = bcd(r[6]);
    if (t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 ||
        t->hour > 23 || t->minute > 59 || t->second > 59)
        return -1;
    return 0;
}

void rtc_set(const rtc_time_t *t) {
    uint32_t date = tobcd((uint8_t)(t->year - 2000)) |
                    ((uint32_t)tobcd(t->month) << 8) |
                    ((uint32_t)tobcd(t->day) << 16) |
                    ((uint32_t)tobcd(t->wday) << 24);
    uint32_t time = tobcd(t->hour) |
                    ((uint32_t)tobcd(t->minute) << 8) |
                    ((uint32_t)tobcd(t->second) << 16);
    uint16_t ime = REG_IME;
    REG_IME = 0;
    begin();
    send_cmd(CMD_DATETIME_W);
    send_bits(date, 32);
    send_bits(time, 24);
    end();
    settle();
    REG_IME = ime;
}

int rtc_present(void) { return present; }

int rtc_init(void) {
    /* Reference the signature so --gc-sections keeps it in the ROM. */
    if (((volatile const char *)rtc_signature)[0] != 'S')
        return RTC_NOT_FOUND;

    GPIO_CTRL = 1;
    uint16_t ime = REG_IME;
    REG_IME = 0;

    uint8_t s = rtc_status();
    int result = RTC_OK;
    if (s & STAT_POWER) {
        reset_rtc();
        write_status(STAT_24H);
        result = RTC_POWER_LOST;
    } else if (!(s & STAT_24H)) {
        write_status(STAT_24H);
    }

    rtc_time_t t;
    int ok = rtc_get(&t) == 0;
    /* Without an RTC the GPIO reads back ROM bytes (or open bus); a status
     * of 0x00/0xFF together with an invalid date means nothing is there. */
    uint8_t s2 = rtc_status();
    if (!ok || !(s2 & STAT_24H) || s2 == 0xFF) {
        present = 0;
        REG_IME = ime;
        return RTC_NOT_FOUND;
    }
    present = 1;
    REG_IME = ime;
    return result;
}

/* ------------------------------------------------------ date arithmetic */

static int is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static int days_in_month(int y, int m) {
    return (m == 2 && is_leap(y)) ? 29 : mdays[m - 1];
}

uint32_t rtc_to_seconds(const rtc_time_t *t) {
    uint32_t days = 0;
    for (int y = 2000; y < t->year; y++)
        days += is_leap(y) ? 366 : 365;
    for (int m = 1; m < t->month; m++)
        days += days_in_month(t->year, m);
    days += t->day - 1;
    return days * 86400u + t->hour * 3600u + t->minute * 60u + t->second;
}

void rtc_from_seconds(uint32_t secs, rtc_time_t *t) {
    uint32_t days = secs / 86400u;
    uint32_t rem = secs % 86400u;
    t->hour = (uint8_t)(rem / 3600);
    t->minute = (uint8_t)((rem / 60) % 60);
    t->second = (uint8_t)(rem % 60);
    t->wday = (uint8_t)((days + 6) % 7);  /* 2000-01-01 was a Saturday (6) */
    int y = 2000;
    while (days >= (uint32_t)(is_leap(y) ? 366 : 365)) {
        days -= is_leap(y) ? 366 : 365;
        y++;
    }
    int m = 1;
    while (days >= (uint32_t)days_in_month(y, m)) {
        days -= days_in_month(y, m);
        m++;
    }
    t->year = (uint16_t)y;
    t->month = (uint8_t)m;
    t->day = (uint8_t)(days + 1);
}
