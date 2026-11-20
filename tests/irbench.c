/* irbench: per-frame dynamic instruction counter (perf_event_open).
 *
 * usage: irbench enc|encenh|dec corpus.raw
 *
 * Counts real dynamic instructions for every frame of the corpus in a
 * single continuous encode/decode pass (state carries across frames,
 * as in live use).  Prints per-frame counts, avg and worst.  This is
 * the instruction-count basis for the M4 projection: 2.17 cycles/Ir
 * at 100 MHz (host-vs-hardware calibrated anchor, see README), so
 * worst-case frames must stay under 2,000,000 cycles / 2.17
 * ~= 921,659 Ir to fit the 20 ms budget.
 */
#include <errno.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "imbe2.h"

static int64_t ir_fd = -1;

static void die_or(const char *what, int rc)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    exit(rc);
}

static void ir_init(void)
{
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_HARDWARE;
    a.size = sizeof a;
    a.config = PERF_COUNT_HW_INSTRUCTIONS;
    a.disabled = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    ir_fd = syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
    if (ir_fd < 0)
        die_or("perf_event_open", 1);
}

static uint64_t ir_start(void)
{
    ioctl(ir_fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(ir_fd, PERF_EVENT_IOC_ENABLE, 0);
    return 0;
}

static uint64_t ir_stop(void)
{
    ioctl(ir_fd, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t cnt = 0;
    read(ir_fd, &cnt, sizeof cnt);
    return cnt;
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: irbench enc|encenh|dec corpus.raw\n");
        return 1;
    }
    int enc_mode = !strcmp(argv[1], "enc");
    int enh_mode = !strcmp(argv[1], "encenh");
    FILE *f = fopen(argv[2], "rb");
    if (!f)
    {
        perror(argv[2]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    int frames = n / 320;
    int16_t *corpus = malloc((size_t)frames * 320);
    if (fread(corpus, 320, (size_t)frames, f) != (size_t)frames)
    {
        fprintf(stderr, "short read\n");
        return 1;
    }
    fclose(f);

    ir_init();

    static unsigned char state[16384];
    static int16_t speech[160], out[160];
    static uint8_t bits[11];

    if (enc_mode || enh_mode)
    {
        imbe2_encoder_init(state);
        if (enh_mode)
            imbe2_encoder_set_enh(state, 1);
        /* warmup: first frame includes cold-start costs (not counted) */
        imbe2_encode(state, bits, &corpus[0]);
        imbe2_encoder_init(state);
        if (enh_mode)
            imbe2_encoder_set_enh(state, 1);
        imbe2_encode(state, bits, &corpus[0]); /* re-warm */
        imbe2_encoder_init(state);
        if (enh_mode)
            imbe2_encoder_set_enh(state, 1);

        uint64_t total = 0, worst = 0, worst_ss = 0;
        int worst_fr = -1, worst_ss_fr = -1;
        static uint64_t counts[8192];
        for (int fr = 0; fr < frames && fr < 8192; fr++)
        {
            ir_start();
            imbe2_encode(state, bits, &corpus[fr * 160]);
            uint64_t c = ir_stop();
            counts[fr] = c;
            total += c;
            if (c > worst)
            {
                worst = c;
                worst_fr = fr;
            }
            if (fr >= 1 && c > worst_ss)
            {
                worst_ss = c;
                worst_ss_fr = fr;
            }
        }
        /* top 5 frames */
        int top[5] = { -1, -1, -1, -1, -1 };
        for (int fr = 0; fr < frames && fr < 8192; fr++)
            for (int t = 0; t < 5; t++)
                if (top[t] < 0 || counts[fr] > counts[top[t]])
                {
                    for (int u = 4; u > t; u--)
                        top[u] = top[u - 1];
                    top[t] = fr;
                    break;
                }
        printf("%s %s: %d frames: avg %lu ir/frame, worst %lu ir (frame %d), total %lu\n",
               argv[1], argv[2], frames,
               (unsigned long)(total / (uint64_t)frames), (unsigned long)worst,
               worst_fr, (unsigned long)total);
        printf("  worst steady-state (frame>=1): %lu ir (frame %d)\n",
               (unsigned long)worst_ss, worst_ss_fr);
        for (int t = 0; t < 5; t++)
            if (top[t] >= 0)
                printf("  top%d: frame %d = %lu ir\n", t + 1, top[t], (unsigned long)counts[top[t]]);
        printf("  @100MHz M4: avg %.2f ms, worst %.2f ms, worst-ss %.2f ms (budget 20.00 ms)\n",
               (double)total / frames * 2.17 / 1e5,
               (double)worst * 2.17 / 1e5,
               (double)worst_ss * 2.17 / 1e5);
    }
    else
    {
        /* pre-encode for the decode path */
        imbe2_encoder_init(state);
        uint8_t *framebits = malloc((size_t)frames * 11);
        for (int fr = 0; fr < frames; fr++)
            imbe2_encode(state, &framebits[fr * 11], &corpus[fr * 160]);
        imbe2_decoder_init(state);
        imbe2_decode(state, out, framebits); /* warmup */
        imbe2_decoder_init(state);

        uint64_t total = 0, worst = 0;
        int worst_fr = -1;
        for (int fr = 0; fr < frames; fr++)
        {
            ir_start();
            imbe2_decode(state, out, &framebits[fr * 11]);
            uint64_t c = ir_stop();
            total += c;
            if (c > worst)
            {
                worst = c;
                worst_fr = fr;
            }
        }
        printf("dec %s: %d frames: avg %lu ir/frame, worst %lu ir (frame %d)\n",
               argv[2], frames, (unsigned long)(total / (uint64_t)frames),
               (unsigned long)worst, worst_fr);
    }
    return 0;
}
