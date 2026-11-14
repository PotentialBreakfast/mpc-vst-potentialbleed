/* PotentialBleed (PotentialBreaks): ghost bleed for MPC OS standalone devices.
 * It listens for hits and, behind each one, strikes a small synthetic instrument: a set of tuned resonances (decaying
 * sines) plus a burst of filtered noise. The residue sits quietly under the track, like the other instruments that
 * leak into a real recording. Eight sources, all generated here (no recordings):
 *   WIRES      snare-wire rattle: bright noise that buzzes, a little shell
 *   CABINET    low cabinet thump and boom
 *   SOUNDBOARD wooden piano-board ring
 *   TINES      bright struck tines
 *   STRINGS    string scrape and ring
 *   CYMBAL     a diffuse metallic wash
 *   ROOM       a small room's low ring and air
 *   BARS       vibraphone-like bars
 * BLEED (how much), DECAY, SENSE (how hard a hit must be), CHANCE (how often a hit leaks), DUCK (less under loud audio),
 * GAPS (more in the quiet spaces), TONE (dark - bright), TICK (a needle tick on each hit), DRIFT (slow stereo wobble of
 * the whole sound), LISTEN (the residue on its own), OUTPUT. No lookahead: nothing is delayed, so timing on MPC stays put.
 * 44.1 kHz, interleaved int16 stereo, 128-frame blocks (wrapper/engine.h). Original code. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
#include "../shared/looks/potential_theme.h"   /* the LOOK switch (parameter "theme") */

#define FS 44100.0f
#define PI_F 3.14159265f

enum {
    P_BLEED, P_OUT, P_SOURCE, P_DECAY, P_SENSE, P_CHANCE, P_DUCK, P_GAPS, P_TONE, P_TICK, P_DRIFT, P_LISTEN,
    NP
};
static const struct { const char *key; float def; } PDEF[NP] = {
    {"bleed", 35}, {"output", 0}, {"source", 0}, {"decay", 45}, {"sense", -30}, {"chance", 80},
    {"duck", 50}, {"gaps", 30}, {"tone", 0}, {"tick", 15}, {"drift", 15}, {"listen", 0},
};

/* ---- the eight sources ---- */
#define NMODES 8
typedef struct {
    int n;
    float f[NMODES], g[NMODES], d[NMODES];   /* mode frequency (Hz), level, decay (times DECAY) */
    float noise_f, noise_q, noise_g, noise_d; /* noise burst: band centre, Q, level, decay (times DECAY) */
    float rattle;                             /* 0..1: the noise buzzes (wires, scrape) */
    float level;                              /* overall trim so the sources sit at similar loudness */
} source_t;
#define NSRC 8
static const source_t SRC[NSRC] = {
    /* WIRES */      {2, {182, 331}, {0.25f, 0.12f}, {0.35f, 0.25f}, 4800, 0.9f, 1.0f, 0.45f, 0.85f, 2.85f},
    /* CABINET */    {5, {58, 87, 121, 176, 243}, {1.0f, 0.75f, 0.5f, 0.32f, 0.2f}, {1.0f, 0.85f, 0.7f, 0.55f, 0.45f}, 260, 0.7f, 0.25f, 0.3f, 0, 0.52f},
    /* SOUNDBOARD */ {8, {108, 163, 219, 281, 365, 498, 731, 1093}, {0.8f, 0.7f, 0.6f, 0.55f, 0.45f, 0.35f, 0.25f, 0.15f},
                      {1.0f, 0.95f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f}, 950, 0.6f, 0.18f, 0.25f, 0, 0.54f},
    /* TINES */      {4, {1047, 2791, 5243, 7911}, {1.0f, 0.45f, 0.25f, 0.12f}, {1.2f, 0.75f, 0.45f, 0.3f}, 3200, 1.2f, 0.05f, 0.1f, 0, 0.57f},
    /* STRINGS */    {6, {196, 393.4f, 591.2f, 789.6f, 989, 1189.5f}, {0.7f, 0.55f, 0.42f, 0.33f, 0.25f, 0.2f},
                      {1.4f, 1.2f, 1.0f, 0.85f, 0.7f, 0.6f}, 2600, 2.0f, 0.45f, 0.22f, 0.6f, 0.77f},
    /* CYMBAL */     {6, {3217, 4103, 5287, 6691, 8123, 9509}, {0.35f, 0.3f, 0.28f, 0.25f, 0.2f, 0.15f},
                      {1.5f, 1.4f, 1.3f, 1.2f, 1.1f, 1.0f}, 8200, 0.5f, 0.7f, 1.1f, 0.1f, 0.69f},
    /* ROOM */       {5, {119, 167, 233, 311, 419}, {0.5f, 0.45f, 0.4f, 0.32f, 0.25f}, {0.45f, 0.42f, 0.38f, 0.34f, 0.3f}, 1300, 0.4f, 0.55f, 0.55f, 0, 7.00f},
    /* BARS */       {3, {523.3f, 2056, 4709}, {1.0f, 0.35f, 0.12f}, {1.6f, 0.7f, 0.3f}, 2000, 1.0f, 0.03f, 0.08f, 0, 0.88f},
};

static inline float decay_ms(float pct) { return 30.0f * powf(50.0f, pct / 100.0f); }   /* 30 .. 1500 ms */

#define DRIFT_LEN 1024
typedef struct { float re, im, cr, ci; } ring_t;   /* a decaying sine: (re, im) turns by (cr, ci) each sample */

typedef struct {
    float p[NP];
    pt_theme_t theme;
    int wild;
    uint64_t pending;
    uint32_t dice, rng;
    /* detector */
    float fast, slow, level; int holdoff, armed;
    /* residue */
    ring_t m[2][NMODES];
    float noise_env, noise_k, rat, rat_k;
    float nb_ic1[2], nb_ic2[2];                 /* the noise's band-pass */
    float tone_lp[2];
    float tick_env, tick_hp[2], tick_last[2];
    /* drift */
    float dbuf[2][DRIFT_LEN]; int dpos; float wow, wander, wander_t; int wander_n;
    float s_out, s_bleed;
    int last_src;
    unsigned hits;                              /* for the tests */
} bleed_t;

static inline float frand(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return (float)(int32_t)x * (1.0f / 2147483648.0f); }
static inline float urand(uint32_t *s) { return 0.5f * (frand(s) + 1.0f); }
static inline float onepole_k(float hz) { return 1.0f - expf(-2.0f * PI_F * hz / FS); }
static inline int ival(const bleed_t *s, int i) { return (int)lroundf(s->p[i]); }

static void reset(bleed_t *s) {
    pt_theme_t theme = s->theme;
    memset(s, 0, sizeof *s);
    s->theme = theme;
    for (int i = 0; i < NP; i++) s->p[i] = PDEF[i].def;
    s->rng = 0x2468aceu;
    s->dice = 0x9e3779b9u ^ (uint32_t)(uintptr_t)s;
    s->wild = 1;
    s->s_out = -1; s->s_bleed = -1;   /* -1: take the setting as it is on the first block (a project's own, not a glide from the default) */
    s->last_src = -1;
    s->armed = 1;
}

/* ---- ROLL: a new source and shape ---- */
static float dice_u(bleed_t *s) { uint32_t x = s->dice; x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->dice = x; return (x >> 8) / 16777216.0f; }
static float dice_in(bleed_t *s, float lo, float hi) { return roundf(lo + (hi - lo) * dice_u(s)); }
static void roll(bleed_t *s) {
    int w = s->wild;
    if (w > 0 || dice_u(s) < 0.5f) s->p[P_SOURCE] = (float)(int)(dice_u(s) * NSRC * 0.9999f);   /* MILD keeps the source half the time */
    s->p[P_BLEED] = dice_in(s, 15, w == 0 ? 40 : w == 1 ? 55 : 80);
    s->p[P_DECAY] = dice_in(s, w == 2 ? 10 : 25, w == 2 ? 95 : 70);
    s->p[P_CHANCE] = dice_in(s, w == 2 ? 30 : 55, 100);
    s->p[P_DUCK] = dice_in(s, w == 2 ? 10 : 35, 85);
    s->p[P_GAPS] = dice_in(s, 0, w == 0 ? 45 : 80);
    s->p[P_TONE] = dice_in(s, w == 0 ? -35 : -80, w == 0 ? 25 : 60);
    s->p[P_TICK] = dice_in(s, 0, w == 0 ? 25 : w == 1 ? 45 : 80);
    s->p[P_DRIFT] = dice_in(s, 0, w == 0 ? 25 : w == 1 ? 45 : 80);
    for (int i = P_BLEED; i <= P_DRIFT; i++) if (i != P_OUT && i != P_SENSE) s->pending |= 1ull << i;
}

int mpc_engine_changed(void *inst, char *key, int len) {
    bleed_t *s = inst;
    for (int i = 0; i < NP; i++)
        if (s->pending & (1ull << i)) { s->pending &= ~(1ull << i); snprintf(key, len, "%s", PDEF[i].key); return 1; }
    return 0;
}

static void *create(const char *data_dir) {
    bleed_t *s = calloc(1, sizeof *s);
    if (s) { reset(s); pt_theme_init(&s->theme, data_dir); }
    return s;
}
static void destroy(void *inst) { free(inst); }
static void midi(void *inst, const uint8_t *msg, int len) { (void)inst; (void)msg; (void)len; }
static int find(const char *key) { for (int i = 0; i < NP; i++) if (!strcmp(key, PDEF[i].key)) return i; return -1; }

static void set_param(void *inst, const char *key, const char *val) {
    bleed_t *s = inst;
    if (pt_theme_set(&s->theme, key, val)) return;
    if (!strcmp(key, "state")) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", val);
        char *save = NULL;
        for (char *tok = strtok_r(tmp, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            int i = find(tok);
            if (i >= 0) s->p[i] = (float)atof(eq + 1);
            else if (!strcmp(tok, "wild")) { int w = atoi(eq + 1); s->wild = w < 0 ? 0 : w > 2 ? 2 : w; }
        }
        if (ival(s, P_SOURCE) < 0 || ival(s, P_SOURCE) >= NSRC) s->p[P_SOURCE] = 0;
        return;
    }
    if (!strcmp(key, "dice")) { if (atof(val) > 0.5) roll(s); return; }
    if (!strcmp(key, "wild")) { int w = (int)lroundf((float)atof(val)); s->wild = w < 0 ? 0 : w > 2 ? 2 : w; return; }
    int i = find(key);
    if (i >= 0) s->p[i] = (float)atof(val);
}

static int get_param(void *inst, const char *key, char *buf, int buf_len) {
    bleed_t *s = inst;
    { int n = pt_theme_get(&s->theme, key, buf, buf_len); if (n) return n; }
    if (!strcmp(key, "state")) {
        int n = 0;
        for (int i = 0; i < NP && n < buf_len; i++) n += snprintf(buf + n, buf_len - n, "%s=%g;", PDEF[i].key, (double)s->p[i]);
        if (n < buf_len) n += snprintf(buf + n, buf_len - n, "wild=%d;", s->wild);
        return n < buf_len ? n : buf_len - 1;
    }
    if (!strcmp(key, "dice")) return snprintf(buf, buf_len, "0");
    if (!strcmp(key, "wild")) return snprintf(buf, buf_len, "%d", s->wild);
    if (!strcmp(key, "decay_display")) return snprintf(buf, buf_len, "%.0f ms", (double)decay_ms(s->p[P_DECAY]));
    if (!strcmp(key, "tone_display")) {
        int t = ival(s, P_TONE);
        return t == 0 ? snprintf(buf, buf_len, "FLAT") : snprintf(buf, buf_len, "%s %d", t < 0 ? "DARK" : "BRIGHT", abs(t));
    }
    int i = find(key);
    if (i < 0) return 0;
    if (i == P_SOURCE || i == P_LISTEN) return snprintf(buf, buf_len, "%d", ival(s, i));
    return snprintf(buf, buf_len, "%g", (double)s->p[i]);
}

static void render(void *inst, int16_t *out_lr, int frames) { (void)inst; memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t)); }

/* a hit: strike the source. h = how hard (0..1). Each hit is tuned a hair differently, as a real instrument would be. */
static void strike(bleed_t *s, float h) {
    int src = ival(s, P_SOURCE);
    if (src < 0 || src >= NSRC) src = 0;
    const source_t *S = &SRC[src];
    float dec = decay_ms(s->p[P_DECAY]) / 1000.0f;
    float detune = 1.0f + 0.015f * frand(&s->rng);
    for (int c = 0; c < 2; c++)
        for (int k = 0; k < S->n; k++) {
            ring_t *m = &s->m[c][k];
            float f = S->f[k] * detune * (c ? 1.0035f : 0.9965f);   /* left and right a touch apart: width */
            if (f > 18000.0f) f = 18000.0f;
            float t60 = dec * S->d[k];
            float r = expf(-6.9078f / (t60 * FS));
            m->cr = r * cosf(2 * PI_F * f / FS);
            m->ci = r * sinf(2 * PI_F * f / FS);
            m->im += h * S->g[k] * (0.7f + 0.3f * urand(&s->rng));   /* adds to whatever is still ringing */
        }
    s->noise_env += h * S->noise_g;
    if (s->noise_env > 1.5f) s->noise_env = 1.5f;
    s->noise_k = expf(-6.9078f / (dec * S->noise_d * FS));
    s->tick_env = h;
    s->hits++;
}

static void process_block(bleed_t *s, const int16_t *in_lr, int16_t *out_lr, int n) {
    int src = ival(s, P_SOURCE);
    if (src < 0 || src >= NSRC) src = 0;
    const source_t *S = &SRC[src];
    if (src != s->last_src) {   /* a new source: its noise band changes; what still rings keeps ringing */
        s->last_src = src;
        memset(s->nb_ic1, 0, sizeof s->nb_ic1); memset(s->nb_ic2, 0, sizeof s->nb_ic2);
    }
    const float thr = powf(10.0f, s->p[P_SENSE] / 20.0f);
    const float chance = s->p[P_CHANCE] / 100.0f, duck = s->p[P_DUCK] / 100.0f, gaps = s->p[P_GAPS] / 100.0f;
    const float tone = s->p[P_TONE] / 100.0f, tick = s->p[P_TICK] / 100.0f, drift = s->p[P_DRIFT] / 100.0f;
    const int listen = ival(s, P_LISTEN) == 1;
    const float g_lo = tone > 0 ? 1.0f - 0.85f * tone : 1.0f, g_hi = tone < 0 ? 1.0f + 0.85f * tone : 1.0f + 0.4f * tone;
    const float k_fast_a = onepole_k(1.0f / 0.0005f), k_fast_r = onepole_k(1.0f / 0.012f);
    const float k_slow = onepole_k(1.0f / 0.08f);
    const float k_lvl_a = onepole_k(1.0f / 0.003f), k_lvl_r = onepole_k(1.0f / 0.25f);
    const float k_tone = onepole_k(900.0f), k_tick_hp = onepole_k(1500.0f);
    /* the noise band-pass (TPT state-variable filter) */
    const float ng = tanf(PI_F * (S->noise_f > 18000 ? 18000 : S->noise_f) / FS), nk = 1.0f / S->noise_q;
    const float na1 = 1.0f / (1.0f + ng * (ng + nk)), na2 = ng * na1, na3 = ng * na2;
    const float out_t = powf(10.0f, s->p[P_OUT] / 20.0f), bleed_t_ = s->p[P_BLEED] / 100.0f;
    if (s->s_bleed < 0) s->s_bleed = bleed_t_;
    if (s->s_out < 0) s->s_out = out_t;
    const float wow_inc = 2 * PI_F * 0.37f / FS;
    const float depth = 55.0f * drift;   /* samples (~1.2 ms) */
    for (int i = 0; i < n; i++) {
        float x[2] = {in_lr[2 * i] * (1.0f / 32768.0f), in_lr[2 * i + 1] * (1.0f / 32768.0f)};
        /* the detector: a fast follower jumping well above a slow one, past SENSE, outside the hold-off */
        float a = 0.5f * (fabsf(x[0]) + fabsf(x[1]));
        s->fast += (a - s->fast) * (a > s->fast ? k_fast_a : k_fast_r);
        s->slow += (a - s->slow) * k_slow;
        s->level += (a - s->level) * (a > s->level ? k_lvl_a : k_lvl_r);
        if (s->holdoff > 0) s->holdoff--;
        if (!s->armed && s->fast < 1.2f * s->slow + 1e-4f) s->armed = 1;   /* the last hit has faded: ready for the next */
        if (s->armed && !s->holdoff && s->fast > thr && s->fast > 2.2f * s->slow + 1e-4f) {
            s->holdoff = (int)(0.045f * FS);   /* one strike per hit, not one per cycle */
            s->armed = 0;
            if (urand(&s->rng) < chance) {
                float h = s->fast * 2.0f;
                strike(s, h > 1.0f ? 1.0f : h);
            }
        }
        /* the residue */
        float noise_amt = s->noise_env;
        s->noise_env *= s->noise_k;
        if (S->rattle > 0) {   /* the buzz: the noise switched on and off at a jittery few hundred Hz */
            if (--s->rat_k <= 0) { s->rat = urand(&s->rng) < 0.55f ? 1.0f : 0.15f; s->rat_k = 40 + 120 * urand(&s->rng); }
            noise_amt *= 1.0f - S->rattle + S->rattle * s->rat;
        }
        float res[2];
        for (int c = 0; c < 2; c++) {
            float sum = 0;
            for (int k = 0; k < S->n; k++) {
                ring_t *m = &s->m[c][k];
                float re = m->re * m->cr - m->im * m->ci, im = m->re * m->ci + m->im * m->cr;
                m->re = re; m->im = im;
                sum += im;
            }
            float w = frand(&s->rng) * noise_amt;
            float v3 = w - s->nb_ic2[c], v1 = na1 * s->nb_ic1[c] + na2 * v3, v2 = s->nb_ic2[c] + na2 * s->nb_ic1[c] + na3 * v3;
            s->nb_ic1[c] = 2 * v1 - s->nb_ic1[c]; s->nb_ic2[c] = 2 * v2 - s->nb_ic2[c];
            float r = (sum + v1 * nk * 1.5f) * S->level;
            /* tone: dark <-> bright */
            s->tone_lp[c] += (r - s->tone_lp[c]) * k_tone;
            res[c] = s->tone_lp[c] * g_lo + (r - s->tone_lp[c]) * g_hi;
        }
        /* duck under loud audio, lift in the gaps */
        float lv = s->level * 3.0f; if (lv > 1) lv = 1;
        s->s_bleed += (bleed_t_ - s->s_bleed) * 0.001f;
        float rg = 0.22f * s->s_bleed * s->s_bleed * (1.0f - 0.9f * duck * lv) * (1.0f + 1.5f * gaps * (1.0f - lv));
        /* the needle tick: a short bright click on the hit */
        float tk[2] = {0, 0};
        if (s->tick_env > 1e-4f) {
            for (int c = 0; c < 2; c++) {
                float z = frand(&s->rng) * s->tick_env;
                s->tick_hp[c] += (z - s->tick_hp[c]) * k_tick_hp;
                tk[c] = (z - s->tick_hp[c]) * 0.12f * tick;
            }
            s->tick_env *= 0.93f;   /* ~1.5 ms */
        }
        float y[2];
        for (int c = 0; c < 2; c++) {
            float g = res[c] * rg * (listen ? 4.0f : 1.0f);
            g = 0.5f * tanhf(2.0f * g);   /* a soft ceiling: the residue never clips, however it's set */
            y[c] = (listen ? 0.0f : x[c]) + g + tk[c];
        }
        /* drift: a slow, slightly different wobble left and right (the whole sound) */
        s->dbuf[0][s->dpos] = y[0]; s->dbuf[1][s->dpos] = y[1];
        if (depth > 0.01f) {
            s->wow += wow_inc; if (s->wow > 2 * PI_F) s->wow -= 2 * PI_F;
            if (--s->wander_n <= 0) { s->wander_t = frand(&s->rng); s->wander_n = 30000; }
            s->wander += (s->wander_t - s->wander) * 0.00004f;
            for (int c = 0; c < 2; c++) {
                float mod = depth * (1.0f + 0.6f * sinf(s->wow + (c ? 1.3f * drift : 0)) + 0.4f * s->wander * (c ? -1.0f : 1.0f));
                float rp = (float)s->dpos - 2.0f - mod;
                while (rp < 0) rp += DRIFT_LEN;
                int i0 = (int)rp & (DRIFT_LEN - 1), i1 = (i0 + 1) & (DRIFT_LEN - 1);
                float f = rp - (float)i0;
                y[c] = s->dbuf[c][i0] + (s->dbuf[c][i1] - s->dbuf[c][i0]) * f;
            }
        }
        s->dpos = (s->dpos + 1) & (DRIFT_LEN - 1);
        s->s_out += (out_t - s->s_out) * 0.002f;
        for (int c = 0; c < 2; c++) {
            float v = y[c] * s->s_out;
            if (!(v == v)) v = 0;
            v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
            out_lr[2 * i + c] = v >= 1.0f ? 32767 : (int16_t)lrintf(v * 32768.0f);   /* the same scale as the input: untouched audio stays bit-exact */
        }
    }
    /* a source with fewer modes must not keep stale ones ringing at full: they decay on their own (r < 1), nothing to do */
}

static void process(void *inst, const int16_t *in_lr, int16_t *out_lr, int frames) {
    for (int done = 0; done < frames; done += 128) {
        int n = frames - done < 128 ? frames - done : 128;
        process_block(inst, in_lr + 2 * done, out_lr + 2 * done, n);
    }
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, process, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
