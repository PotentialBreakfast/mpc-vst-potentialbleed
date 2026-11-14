/* PotentialBleed on the computer: hits are found, the residue rings and dies away, it stays clean when off.
 * Build: gcc -fsanitize=address,undefined -O1 -std=gnu11 -I src -I vst/build -I <kit>/wrapper tests/test_bleed.c -lm */
#include "../src/bleed.c"
#include <time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static void set(bleed_t *s, const char *k, double v) { char b[32]; snprintf(b, sizeof b, "%g", v); set_param(s, k, b); }

/* a drum hit every 0.5 s: a burst of noise falling off over ~60 ms, at peak `amp` */
static float drums(long t, float amp) {
    static uint32_t r = 99;
    long ph = t % 22050;
    return ph < 5000 ? amp * frand(&r) * expf(-(float)ph / 2600.0f) : 0.0f;
}
static void run(bleed_t *s, float amp, long n, float *outL, int silent) {
    int16_t in[256], out[256];
    for (long t = 0; t < n; t += 128) {
        for (int i = 0; i < 128; i++) { float v = silent ? 0 : drums(t + i, amp); in[2 * i] = in[2 * i + 1] = (int16_t)lrintf(v * 32767.0f); }
        process(s, in, out, 128);
        for (int i = 0; i < 128 && t + i < n; i++) if (outL) outL[t + i] = out[2 * i] / 32768.0f;
    }
}
static double rms(const float *x, long a, long b) { double e = 0; for (long i = a; i < b; i++) e += x[i] * x[i]; return sqrt(e / (b - a)); }

int main(void) {
    static float y[44100 * 6];
    const long N = 44100 * 4;   /* 8 hits */
    /* silence in = silence out, at every source and the loudest settings */
    for (int src = 0; src < NSRC; src++) {
        bleed_t *s = create(NULL); set(s, "source", src); set(s, "bleed", 100); set(s, "gaps", 100); set(s, "tick", 100);
        run(s, 0, N, y, 1); CHECK(rms(y, 0, N) < 1e-5, "src %d: silence gives %g", src, rms(y, 0, N)); destroy(s);
    }
    /* hits are found: 8 drum hits at -12 dB -> 8 strikes at CHANCE 100 */
    { bleed_t *s = create(NULL); set(s, "chance", 100); run(s, 0.25f, N - 500, y, 0); CHECK(s->hits == 8, "found %u hits of 8", s->hits); destroy(s); }
    /* SENSE above the hits: none; CHANCE 0: none; CHANCE 50: some */
    { bleed_t *s = create(NULL); set(s, "chance", 100); set(s, "sense", -6); run(s, 0.1f, N, y, 0); CHECK(s->hits == 0, "sense -6 dB still found %u", s->hits); destroy(s); }
    { bleed_t *s = create(NULL); set(s, "chance", 0); run(s, 0.25f, N, y, 0); CHECK(s->hits == 0, "chance 0 still struck %u", s->hits); destroy(s); }
    { bleed_t *s = create(NULL); set(s, "chance", 50); run(s, 0.25f, N * 4, NULL, 0); CHECK(s->hits > 6 && s->hits < 26, "chance 50: %u of 32", s->hits); destroy(s); }
    /* every source rings after a hit (LISTEN on), dies away, never clips or goes NaN */
    for (int src = 0; src < NSRC; src++) {
        bleed_t *s = create(NULL); set(s, "source", src); set(s, "listen", 1); set(s, "bleed", 100); set(s, "chance", 100); set(s, "duck", 0); set(s, "decay", 100);
        int16_t in[256], out[256]; long t = 0; float mx = 0; double early = 0, late = 0;
        for (; t < 44100 * 5; t += 128) {
            for (int i = 0; i < 128; i++) { float v = (t + i) < 5000 ? drums(t + i, 0.5f) : 0; in[2 * i] = in[2 * i + 1] = (int16_t)lrintf(v * 32767); }
            process(s, in, out, 128);
            for (int i = 0; i < 128; i++) { float v = fabsf(out[2 * i] / 32768.0f); if (v > mx) mx = v; long u = t + i;
                if (u > 6000 && u < 12000) early += v * v; if (u > 44100 * 4) late += v * v; }
        }
        early = sqrt(early / 6000); late = sqrt(late / 44100);
        CHECK(early > 3e-3, "src %d: no residue after the hit (%g)", src, early);
        CHECK(late < early * 0.01 || late < 1e-5, "src %d: still ringing 4 s on (%g vs %g)", src, late, early);
        CHECK(mx < 0.999f, "src %d: clipped (%f)", src, mx);
        printf("src %d (%s): residue %.1f dBFS just after the hit\n", src, (const char *[]){"WIRES","CABINET","SOUNDBOARD","TINES","STRINGS","CYMBAL","ROOM","BARS"}[src], 20 * log10(early));
        destroy(s);
    }
    /* BLEED 0, TICK 0, DRIFT 0: the audio passes untouched */
    { bleed_t *s = create(NULL); set(s, "bleed", 0); set(s, "tick", 0); set(s, "drift", 0);
      int16_t in[256], out[256]; long diff = 0; uint32_t r = 3;
      for (int b = 0; b < 2000; b++) { for (int i = 0; i < 256; i++) in[i] = (int16_t)(frand(&r) * 12000 * ((b % 40) < 3)); process(s, in, out, 128);
        for (int i = 0; i < 256; i++) diff += labs((long)in[i] - out[i]); }
      CHECK(diff < 2000 * 256 / 20, "bleed 0: audio changed (sum of differences %ld)", diff); destroy(s); }
    /* ROLL and the saved state */
    { bleed_t *s = create(NULL);
      for (int w = 0; w < 3; w++) { set(s, "wild", w); for (int r = 0; r < 3000; r++) { set_param(s, "dice", "1");
          for (int i = 0; i < NP; i++) if (i != P_TONE && i != P_SENSE && i != P_OUT) CHECK(s->p[i] >= 0 && s->p[i] <= 100, "roll %d: %s = %g", w, PDEF[i].key, s->p[i]);
          CHECK(s->p[P_TONE] >= -100 && s->p[P_TONE] <= 100 && ival(s, P_SOURCE) < NSRC, "roll tone/source"); } }
      char st[1024]; get_param(s, "state", st, sizeof st); bleed_t *t2 = create(NULL); set_param(t2, "state", st);
      CHECK(!memcmp(s->p, t2->p, sizeof s->p) && s->wild == t2->wild, "state does not round-trip"); destroy(s); destroy(t2); }
    { bleed_t *s = create(NULL); char b[32]; get_param(s, "decay_display", b, sizeof b); printf("decay at 45%%: %s\n", b);
      set(s, "tone", -30); get_param(s, "tone_display", b, sizeof b); CHECK(!strcmp(b, "DARK 30"), "tone text %s", b); destroy(s); }
    /* CPU here, worst case: the source with the most modes, many hits */
    { bleed_t *s = create(NULL); set(s, "source", 2); set(s, "chance", 100); set(s, "drift", 100); clock_t c0 = clock(); run(s, 0.3f, 44100 * 8, NULL, 0);
      printf("CPU here (SOUNDBOARD, every hit, DRIFT on): %.2f %% of real time\n", 100.0 * (clock() - c0) / CLOCKS_PER_SEC / 8.0); destroy(s); }
    printf(fails ? "FAILED (%d)\n" : "OK: silence, hit finding, sense and chance, eight sources ring and die away, clean when off, roll, state\n", fails);
    return fails != 0;
}
