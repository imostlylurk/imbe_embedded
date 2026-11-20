/* stageir: per-stage dynamic instruction counter for the encoder.
 *
 * Reaches into imbe_encoder's private methods (same #define trick as
 * perf_check) and times each pipeline stage of encode() separately,
 * with real perf_event_open instruction counts:
 *
 *   slide    : pitch buffer slide
 *   lpf      : dc_rmv + pe_lpf
 *   pe       : pitch_est (incl. e_p + searches)
 *   fft      : windowing + 256-pt fft
 *   pref     : pitch_ref
 *   vu       : v_uv_det
 *   sa       : sa_encode
 *   pack     : encode_frame_vector
 *
 * usage: stageir corpus.raw [pass_filter]
 * Prints per-stage avg and worst, plus a worst-frame breakdown.
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

#include "typedef.h"
#include "globals.h"
#include "imbe.h"
#define private public
#include "imbe_vocoder.h"
#undef private
#include "encode.h"
#include "dc_rmv.h"
#include "pe_lpf.h"
#include "pitch_est.h"
#include "pitch_ref.h"
#include "v_uv_det.h"
#include "sa_encode.h"
#include "ch_encode.h"
#include "dsp_sub.h"
#include "tbls.h"

#ifdef PREF_COUNT
extern unsigned long pref_cnt_outer, pref_cnt_inner1, pref_cnt_mac;
extern unsigned long pref_cnt_inner2, pref_cnt_lsum, pref_cnt_frames;
extern void pref_dump(void);
#endif
#ifdef SA_COUNT
unsigned long sa_cnt_harms, sa_cnt_dct_terms, sa_cnt_qnt, sa_cnt_frames;
#endif

static int64_t ir_fd = -1;

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
    {
        fprintf(stderr, "perf_event_open: %s\n", strerror(errno));
        exit(1);
    }
}

static inline void ir_start(void)
{
    ioctl(ir_fd, PERF_EVENT_IOC_RESET, 0);
    ioctl(ir_fd, PERF_EVENT_IOC_ENABLE, 0);
}

static inline uint64_t ir_stop(void)
{
    ioctl(ir_fd, PERF_EVENT_IOC_DISABLE, 0);
    uint64_t c = 0;
    read(ir_fd, &c, sizeof c);
    return c;
}

enum STAGE { SLIDE, LPF, PE, FFT, PREF, VU, SA, PACK, NSTAGE };
static const char *stage_name[NSTAGE] = {
    "slide", "lpf", "pe", "fft", "pref", "vu", "sa", "pack"
};

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: stageir corpus.raw\n");
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f)
    {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    int frames = n / 320;
    int16_t *corpus = (int16_t *)malloc((size_t)frames * 320);
    if (fread(corpus, 320, frames, f) != (size_t)frames)
    {
        fprintf(stderr, "short read\n");
        return 1;
    }
    fclose(f);
    ir_init();

    static imbe_encoder enc;
    uint64_t tot[NSTAGE] = {0};
    uint64_t worst[NSTAGE] = {0};
    uint64_t worst_total = 0;
    int worst_fr = -1;
    uint64_t fsum[NSTAGE] = {0};
    /* top-5 worst steady-state (fr >= 1) per-stage breakdown */
    uint64_t top5[NSTAGE][5] = {0};
    int top5_fr[5] = { -1, -1, -1, -1, -1 };
    uint64_t top5_tot[5] = {0};
    uint64_t stage_c[NSTAGE];
#if defined(SA_COUNT) || defined(PREF_COUNT)
    uint64_t worst_probe_ir = 0;
    int wp_fr = -1;
    unsigned long wp_harms, wp_dct, wp_qnt, wp_o1, wp_i1, wp_m, wp_i2, wp_ls;
    unsigned long tot_harms = 0, tot_dct = 0, tot_qnt = 0;
    unsigned long tot_o1 = 0, tot_i1 = 0, tot_m = 0, tot_i2 = 0, tot_ls = 0;
#endif

    for (int fr = 0; fr < frames; fr++)
    {
        Word16 *snd = &corpus[fr * 160];
        int16_t words[8];
        uint64_t c[NSTAGE] = {0};

        /* noise gate: skip (standard mode only, enh=0) */
        ir_start();
        for (int i = 0; i < PITCH_EST_BUF_SIZE - FRAME; i++)
        {
            enc.pitch_est_buf[i] = enc.pitch_est_buf[i + FRAME];
            enc.pitch_ref_buf[i] = enc.pitch_ref_buf[i + FRAME];
        }
        c[SLIDE] = ir_stop();

        ir_start();
        dc_rmv(snd, &enc.pitch_ref_buf[PITCH_EST_BUF_SIZE - FRAME],
               &enc.dc_rmv_mem, FRAME);
        pe_lpf(&enc.pitch_ref_buf[PITCH_EST_BUF_SIZE - FRAME],
               &enc.pitch_est_buf[PITCH_EST_BUF_SIZE - FRAME],
               enc.pe_lpf_mem, FRAME);
        c[LPF] = ir_stop();

        ir_start();
        enc.pitch_est(&enc.my_imbe_param, enc.pitch_est_buf);
        c[PE] = ir_stop();

        ir_start();
        Word16 *wr_ptr = (Word16 *)wr;
        Word16 *sig_ptr = &enc.pitch_ref_buf[40];
        for (int i = 146; i < 256; i++)
        {
            enc.fft_buf[i].re = mult(*sig_ptr++, *wr_ptr++);
            enc.fft_buf[i].im = 0;
        }
        enc.fft_buf[0].re = *sig_ptr++;
        enc.fft_buf[0].im = 0;
        wr_ptr--;
        for (int i = 1; i < 111; i++)
        {
            enc.fft_buf[i].re = mult(*sig_ptr++, *wr_ptr--);
            enc.fft_buf[i].im = 0;
        }
        for (int i = 111; i < 146; i++)
            enc.fft_buf[i].re = enc.fft_buf[i].im = 0;
        fft(enc.wr_array, enc.wi_array, (Word16 *)&enc.fft_buf, FFTLENGTH, 1);
        c[FFT] = ir_stop();

        ir_start();
        pitch_ref(&enc.my_imbe_param, enc.fft_buf);
        c[PREF] = ir_stop();

        ir_start();
        enc.v_uv_det(&enc.my_imbe_param, enc.fft_buf);
        c[VU] = ir_stop();

        ir_start();
        enc.sa_encode(&enc.my_imbe_param);
        c[SA] = ir_stop();
#if defined(SA_COUNT) || defined(PREF_COUNT)
        {
            static unsigned long prev_harms = 0, prev_dct = 0, prev_qnt = 0,
                                 prev_o1 = 0, prev_i1 = 0, prev_m = 0,
                                 prev_i2 = 0, prev_ls = 0;
            unsigned long dh = sa_cnt_harms - prev_harms;
            unsigned long dd = sa_cnt_dct_terms - prev_dct;
            unsigned long dq = sa_cnt_qnt - prev_qnt;
            unsigned long do1 = pref_cnt_outer - prev_o1;
            unsigned long di1 = pref_cnt_inner1 - prev_i1;
            unsigned long dm = pref_cnt_mac - prev_m;
            unsigned long di2 = pref_cnt_inner2 - prev_i2;
            unsigned long dls = pref_cnt_lsum - prev_ls;
            prev_harms = sa_cnt_harms; prev_dct = sa_cnt_dct_terms; prev_qnt = sa_cnt_qnt;
            prev_o1 = pref_cnt_outer; prev_i1 = pref_cnt_inner1; prev_m = pref_cnt_mac;
            prev_i2 = pref_cnt_inner2; prev_ls = pref_cnt_lsum;
            unsigned long frame_ir = 0;
            for (int s = 0; s < NSTAGE; s++) frame_ir += c[s];
            if (frame_ir > worst_probe_ir)
            {
                worst_probe_ir = frame_ir;
                wp_harms = dh; wp_dct = dd; wp_qnt = dq;
                wp_o1 = do1; wp_i1 = di1; wp_m = dm; wp_i2 = di2; wp_ls = dls;
                wp_fr = fr;
            }
            tot_harms += dh; tot_dct += dd; tot_qnt += dq;
            tot_o1 += do1; tot_i1 += di1; tot_m += dm; tot_i2 += di2; tot_ls += dls;
        }
#endif

        ir_start();
        encode_frame_vector(&enc.my_imbe_param, words);
        c[PACK] = ir_stop();

        uint64_t ft = 0;
        for (int s = 0; s < NSTAGE; s++)
        {
            tot[s] += c[s];
            if (c[s] > worst[s])
                worst[s] = c[s];
            ft += c[s];
            stage_c[s] = c[s];
        }
        if (ft > worst_total)
        {
            worst_total = ft;
            worst_fr = fr;
            for (int s = 0; s < NSTAGE; s++)
                fsum[s] = c[s];
        }
        if (fr >= 1)
            for (int t = 0; t < 5; t++)
                if (top5_fr[t] < 0 || ft > top5_tot[t])
                {
                    for (int u = 4; u > t; u--)
                    {
                        top5_fr[u] = top5_fr[u - 1];
                        top5_tot[u] = top5_tot[u - 1];
                        for (int s = 0; s < NSTAGE; s++)
                            top5[s][u] = top5[s][u - 1];
                    }
                    top5_fr[t] = fr;
                    top5_tot[t] = ft;
                    for (int s = 0; s < NSTAGE; s++)
                        top5[s][t] = stage_c[s];
                    break;
                }
    }

    uint64_t tot_all = 0;
    for (int s = 0; s < NSTAGE; s++)
        tot_all += tot[s];
    printf("stageir %s: %d frames\n", argv[1], frames);
    printf("%-6s %12s %10s %10s %8s\n", "stage", "avg ir", "worst ir",
           "sum ir", "avg %");
    for (int s = 0; s < NSTAGE; s++)
    {
        printf("%-6s %12lu %10lu %10lu %7.1f%%\n", stage_name[s],
               (unsigned long)(tot[s] / frames),
               (unsigned long)worst[s], (unsigned long)tot[s],
               100.0 * tot[s] / (double)tot_all);
    }
    printf("%-6s %12lu %10lu %10lu\n", "TOTAL",
           (unsigned long)(tot_all / frames),
           (unsigned long)worst_total, (unsigned long)tot_all);
    printf("worst frame %d breakdown: ", worst_fr);
    for (int s = 0; s < NSTAGE; s++)
        printf("%s=%lu ", stage_name[s], (unsigned long)fsum[s]);
    printf("\n");
    for (int t = 0; t < 5; t++)
    {
        if (top5_fr[t] < 0)
            continue;
        printf("ss-worst#%d frame %d: ", t + 1, top5_fr[t]);
        for (int s = 0; s < NSTAGE; s++)
            printf("%s=%lu ", stage_name[s], (unsigned long)top5[s][t]);
        printf("\n");
    }
#if defined(SA_COUNT) || defined(PREF_COUNT)
    printf("probe totals over %d frames: ", frames);
    printf("sa: num_harms=%lu/df dct_terms=%lu/df qnt=%lu/df ",
           tot_harms / frames, tot_dct / frames, tot_qnt / frames);
    printf("pref: outer=%lu/df inner1=%lu/df mac=%lu/df inner2=%lu/df lsum=%lu/df\n",
           tot_o1 / frames, tot_i1 / frames, tot_m / frames,
           tot_i2 / frames, tot_ls / frames);
    printf("probe worst frame %d (ir=%lu): ", wp_fr, (unsigned long)worst_probe_ir);
    printf("sa: num_harms=%lu dct_terms=%lu qnt=%lu ", wp_harms, wp_dct, wp_qnt);
    printf("pref: outer=%lu inner1=%lu mac=%lu inner2=%lu lsum=%lu\n",
           wp_o1, wp_i1, wp_m, wp_i2, wp_ls);
#endif
    printf("@100MHz M4 (2.17 c/ir): avg %.2f ms, worst %.2f ms (budget 20)\n",
           (double)tot_all / frames * 2.17 / 1e5,
           (double)worst_total * 2.17 / 1e5);
    return 0;
}
