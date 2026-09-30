/*
 * Headless mGBA runner used to test the GBA ROMs without hardware.
 *
 *   mgba_run ROM [options]
 *     --frames N              run N frames (default 600)
 *     --keys A-B:MASK         hold key MASK (hex GBA KEYINPUT bits) frames A..B
 *     --shot F:file.ppm       screenshot after frame F
 *     --rtc T                 fake RTC: unix time at frame 0 (enables RTC)
 *     --rtc-jump F:SECS       at frame F the fake clock jumps forward SECS
 *     --save file.sav         use (and write back) this SRAM file
 *     --dump F:ADDR:LEN:file  dump memory after frame F
 *     --reset F               reset the console after frame F
 *
 * Build: gcc -O2 -o mgba_run mgba_run.c -lmgba
 */
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba-util/vfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_EV 256

struct keyev { int a, b; unsigned mask; };
struct shot { int f; const char *path; };
struct jump { int f; long secs; };
struct dump { int f; unsigned addr, len; const char *path; };

static struct keyev keys[MAX_EV]; static int nkeys;
static struct shot shots[MAX_EV]; static int nshots;
static struct jump jumps[MAX_EV]; static int njumps;
static struct dump dumps[MAX_EV]; static int ndumps;
static int resets[MAX_EV]; static int nresets;

struct fake_rtc {
    struct mRTCSource d;
    struct mCore *core;
    time_t base;
    long offset;
};

static void rtc_sample(struct mRTCSource *s) { (void)s; }

static time_t rtc_unix(struct mRTCSource *s) {
    struct fake_rtc *r = (struct fake_rtc *)s;
    return r->base + r->offset + (time_t)(r->core->frameCounter(r->core) / 60);
}

static void write_ppm(const char *path, const color_t *buf, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) {
        uint32_t c = buf[i];
        unsigned char px[3] = {c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s ROM [options]\n", argv[0]);
        return 1;
    }
    const char *rom = argv[1];
    int frames = 600;
    long rtc_base = -1;
    const char *save = NULL;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames")) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keys")) {
            struct keyev *k = &keys[nkeys++];
            sscanf(argv[++i], "%d-%d:%x", &k->a, &k->b, &k->mask);
        } else if (!strcmp(argv[i], "--shot")) {
            char *s = argv[++i];
            shots[nshots].f = atoi(s);
            shots[nshots++].path = strchr(s, ':') + 1;
        } else if (!strcmp(argv[i], "--rtc")) rtc_base = atol(argv[++i]);
        else if (!strcmp(argv[i], "--rtc-jump")) {
            sscanf(argv[++i], "%d:%ld", &jumps[njumps].f, &jumps[njumps].secs);
            njumps++;
        } else if (!strcmp(argv[i], "--save")) save = argv[++i];
        else if (!strcmp(argv[i], "--dump")) {
            char *s = argv[++i];
            struct dump *d = &dumps[ndumps++];
            sscanf(s, "%d:%x:%x", &d->f, &d->addr, &d->len);
            char *p = strchr(strchr(strchr(s, ':') + 1, ':') + 1, ':');
            d->path = p + 1;
        } else if (!strcmp(argv[i], "--reset")) resets[nresets++] = atoi(argv[++i]);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
    }

    struct mCore *core = mCoreFind(rom);
    if (!core || !core->init(core)) { fprintf(stderr, "cannot init core\n"); return 1; }
    mCoreInitConfig(core, NULL);

    unsigned w, h;
    core->desiredVideoDimensions(core, &w, &h);
    color_t *buf = calloc(w * h, sizeof(color_t));
    core->setVideoBuffer(core, buf, w);

    if (!mCoreLoadFile(core, rom)) { fprintf(stderr, "cannot load %s\n", rom); return 1; }
    if (save) {
        struct VFile *vf = VFileOpen(save, O_CREAT | O_RDWR);
        if (!vf || !core->loadSave(core, vf)) fprintf(stderr, "warning: cannot use save %s\n", save);
    }

    struct fake_rtc rtc = {0};
    if (rtc_base >= 0) {
        rtc.d.sample = rtc_sample;
        rtc.d.unixTime = rtc_unix;
        rtc.core = core;
        rtc.base = rtc_base;
        mCoreSetRTC(core, &rtc.d);
    }

    core->reset(core);
    if (rtc_base >= 0) {
        /* Force the cartridge RTC on regardless of the game database. */
        struct GBA *gba = core->board;
        GBAHardwareInitRTC(&gba->memory.hw);
    }

    for (int f = 1; f <= frames; f++) {
        unsigned mask = 0;
        for (int k = 0; k < nkeys; k++)
            if (f >= keys[k].a && f <= keys[k].b) mask |= keys[k].mask;
        core->setKeys(core, mask);
        for (int j = 0; j < njumps; j++)
            if (jumps[j].f == f) rtc.offset += jumps[j].secs;
        core->runFrame(core);
        for (int s = 0; s < nshots; s++)
            if (shots[s].f == f) write_ppm(shots[s].path, buf, w, h);
        for (int d = 0; d < ndumps; d++) {
            if (dumps[d].f != f) continue;
            FILE *o = fopen(dumps[d].path, "wb");
            for (unsigned a = 0; a < dumps[d].len; a++) {
                unsigned char b = (unsigned char)core->busRead8(core, dumps[d].addr + a);
                fwrite(&b, 1, 1, o);
            }
            fclose(o);
        }
        for (int r = 0; r < nresets; r++)
            if (resets[r] == f) {
                core->reset(core);
                if (rtc_base >= 0) {
                    struct GBA *gba = core->board;
                    GBAHardwareInitRTC(&gba->memory.hw);
                }
            }
    }
    core->unloadROM(core);
    core->deinit(core);
    free(buf);
    return 0;
}
