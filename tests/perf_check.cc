/* perf_check: exhaustive/randomised verification that the fast basic-op
 * and autocorr implementations are bit-exact (value AND Overflow flag)
 * against reference copies of the original algorithms.
 *
 * Build: g++ -O2 -Iinc tests/perf_check.cc src/*.cc -lm -o perf_check
 * (or link against libimbe2.a)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "typedef.h"
#include "basic_op.h"
#include "imbe.h"
#include "pe_lpf.h"
#include "pitch_est.h"
#include "tbls.h"
#include "globals.h"
#include "dc_rmv.h"
#include "dsp_sub.h"
/* test-only: reach the private autocorr() to unit-test it directly */
#define private public
#include "imbe_vocoder.h"
#undef private

/* ---------------- reference implementations (original algorithms) ------- */

static Word32 ref_L_shr(Word32 L_var1, Word16 var2, int *ovf);

static Word16 ref_div_s(Word16 var1, Word16 var2)
{
    Word16 var_out = 0, iteration;
    Word32 L_num, L_denom;
    if (var1 == 0) return 0;
    if (var1 == var2) return MAX_16;
    L_num = (Word32)(unsigned short)var1;
    L_denom = (Word32)(unsigned short)var2;
    for (iteration = 0; iteration < 15; iteration++)
    {
        var_out <<= 1;
        L_num <<= 1;
        if (L_num >= L_denom)
        {
            L_num = L_num - L_denom;
            var_out = (Word16)(var_out + 1);
        }
    }
    return var_out;
}

static Word16 ref_norm_s(Word16 var1)
{
    Word16 var_out;
    if (var1 == 0) return 0;
    if (var1 == (Word16)0xffff) return 15;
    if (var1 < 0) var1 = ~var1;
    for (var_out = 0; var1 < 0x4000; var_out++) var1 <<= 1;
    return var_out;
}

static Word16 ref_norm_l(Word32 L_var1)
{
    Word16 var_out;
    if (L_var1 == 0) return 0;
    if (L_var1 == (Word32)0xffffffffL) return 31;
    if (L_var1 < 0) L_var1 = ~L_var1;
    for (var_out = 0; L_var1 < (Word32)0x40000000L; var_out++) L_var1 <<= 1;
    return var_out;
}

static Word32 ref_L_shl(Word32 L_var1, Word16 var2, int *ovf)
{
    Word32 L_var_out = 0;
    *ovf = 0;
    if (var2 <= 0)
    {
        if (var2 < -32) var2 = -32;
        return ref_L_shr(L_var1, (Word16)(-var2), ovf);
    }
    for (; var2 > 0; var2--)
    {
        if (L_var1 > (Word32)0x3fffffffL) { *ovf = 1; return MAX_32; }
        if (L_var1 < (Word32)0xc0000000L) { *ovf = 1; return MIN_32; }
        L_var1 *= 2;
        L_var_out = L_var1;
    }
    return L_var_out;
}

static Word32 ref_L_shr(Word32 L_var1, Word16 var2, int *ovf)
{
    Word32 L_var_out;
    *ovf = 0;
    if (var2 < 0)
    {
        if (var2 < -32) var2 = -32;
        return ref_L_shl(L_var1, (Word16)(-var2), ovf);
    }
    if (var2 >= 31)
        L_var_out = (L_var1 < 0L) ? -1 : 0;
    else
        L_var_out = (L_var1 < 0) ? ~((~L_var1) >> var2) : (L_var1 >> var2);
    return L_var_out;
}

static Word32 ref_autocorr(Word16 *sigin, Word16 shift, Word16 scale_shift)
{
    Word16 i;
    Word32 L_sum = 0;
    for (i = 0; i < PITCH_EST_FRAME - shift; i++)
        L_sum = L_add(L_sum, L_shr(L_mult(sigin[i], sigin[i + shift]), scale_shift));
    return L_sum;
}

/* ---------------- helpers ---------------------------------------------- */

static unsigned rng_state = 123456789u;
static unsigned rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static long failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        if (failures < 20) printf("FAIL: " __VA_ARGS__); \
        if (failures < 20) printf("\n"); \
        failures++; \
    } \
} while (0)

/* ---------------- references: pe_lpf / dc_rmv / fft ---------------------- */

static const Word16 ref_lpf_coef[PE_LPF_ORD] =
{ -94,-92,185,543,288,-883,-1834,-495,3891,9141,11512,
  9141,3891,-495,-1834,-883,288,543,185,-92,-94 };

static void ref_pe_lpf(Word16 *sigin, Word16 *sigout, Word16 *mem, Word16 len)
{
    Word16 i; Word32 L_sum;
    while (len--)
    {
        for (i = 0; i < PE_LPF_ORD - 1; i++) mem[i] = mem[i+1];
        mem[PE_LPF_ORD-1] = *sigin++;
        L_sum = 0;
        for (i = 0; i < PE_LPF_ORD; i++) L_sum = L_mac(L_sum, mem[i], ref_lpf_coef[i]);
        *sigout++ = round(L_sum);
    }
}

static void ref_dc_rmv(Word16 *sigin, Word16 *sigout, Word32 *mem, Word16 len)
{
    Word32 L_tmp, L_mem = *mem;
    while (len--)
    {
        L_tmp = L_deposit_h(*sigin++);
        L_mem = L_add(L_mem, L_tmp);
        *sigout++ = round(L_mem);
        L_mem = L_mpy_ls(L_mem, 0x7EB8);
        L_mem = L_sub(L_mem, L_tmp);
    }
    *mem = L_mem;
}

#define SWAP2(a,b) do { Word16 tt = (a); (a) = (b); (b) = tt; } while (0)

static void ref_fft(const Word16 *wr_array, const Word16 *wi_array, Word16 *datam1, Word16 nn, Word16 isign)
{
    Word16 n, mmax, m, j, istep, i, wr, wi, temp1;
    Word32 L_tempr, L_tempi, L_temp1, L_temp2;
    Word16 *data = &datam1[-1];
    Word16 index, index_step;
    n = nn * 2; j = 1;
    for (i = 1; i < n; i += 2)
    {
        if (j > i) { SWAP2(data[j],data[i]); SWAP2(data[j+1],data[i+1]); }
        m = nn;
        while (m >= 2 && j > m) { j = (Word16)(j - m); m >>= 1; }
        j = (Word16)(j + m);
    }
    mmax = 2; index_step = nn;
    while (n > mmax)
    {
        istep = mmax * 2;
        index = 0; index_step >>= 1;
        wr = ONE_Q15; wi = 0;
        for (m = 1; m < mmax; m += 2)
        {
            for (i = m; i <= n; i += istep)
            {
                j = i + mmax;
                L_temp1 = L_shr(L_mult(wr, data[j]), 1);
                L_temp2 = L_shr(L_mult(wi, data[j+1]), 1);
                L_tempr = L_sub(L_temp1, L_temp2);
                L_temp1 = L_shr(L_mult(wr, data[j+1]), 1);
                L_temp2 = L_shr(L_mult(wi, data[j]), 1);
                L_tempi = L_add(L_temp1, L_temp2);
                L_temp1 = L_shr(L_deposit_h(data[i]), 1);
                data[j]   = round(L_sub(L_temp1, L_tempr));
                data[i]   = round(L_add(L_temp1, L_tempr));
                L_temp1 = L_shr(L_deposit_h(data[i+1]), 1);
                data[j+1] = round(L_sub(L_temp1, L_tempi));
                data[i+1] = round(L_add(L_temp1, L_tempi));
            }
            index = (Word16)(index + index_step);
            wr = wr_array[index];
            wi = (isign < 0) ? (Word16)(-wi_array[index]) : wi_array[index];
        }
        mmax = istep;
    }
}


/* reference: ORIGINAL e_p (pre-optimization), using original primitives */
static void ref_e_p(Word16 *sigin, Word16 *res_buf)
{
    Word16 i, j, den_part_acc, tmp;
    Word32 L_sum, L_num, L_den, L_e0, L_tmp;
    Word16 sig_wndwed[PITCH_EST_FRAME];
    Word32 corr[259];
    Word16 index_beg, index_step;
    Word16 scale_shift;

    for (i = 0; i < PITCH_EST_FRAME; i++)
        sig_wndwed[i] = mult_r(sigin[i], wi[i]);

    L_sum = 0;
    for (i = 0; i < PITCH_EST_FRAME; i++)
        L_sum = L_add(L_sum, L_mpy_ls(L_mult(sigin[i], sigin[i]), wi[i]));

    if (L_sum == MAX_32)
    {
        L_sum = 0;
        for (i = 0; i < PITCH_EST_FRAME; i++)
            L_sum = L_add(L_sum, L_mpy_ls(L_shr(L_mult(sigin[i], sigin[i]), 5), wi[i]));
        scale_shift = 5;
    }
    else
        scale_shift = 0;

    L_e0 = 0;
    for (i = 0; i < PITCH_EST_FRAME; i++)
        L_e0 = L_add(L_e0, L_shr(L_mult(sig_wndwed[i], sig_wndwed[i]), scale_shift));

    for (tmp = 21, i = 0; tmp <= 150; tmp++, i += 2)
        corr[i] = ref_autocorr(sig_wndwed, tmp, scale_shift);
    for (i = 1; i < 258; i += 2)
        corr[i] = L_shr(L_add(corr[i - 1], corr[i + 1]), 1);

    den_part_acc = CNST_0_8717_Q1_15;
    index_step = 42;
    index_beg = 0;
    L_e0 = L_shr(L_e0, 7);
    for (i = 0; i < 203; i++)
    {
        L_tmp = 0;
        j = index_beg;
        while (j <= 258)
        {
            L_tmp = L_add(L_tmp, corr[j]);
            j += index_step;
        }
        L_tmp = L_shr(L_tmp, 6);
        L_tmp = L_add(L_tmp, L_e0);
        L_tmp = L_tmp * index_step;
        L_num = L_sub(L_sum, L_tmp);

        index_beg++;
        index_step++;

        L_den = L_mpy_ls(L_sum, den_part_acc);

        if (L_num < L_den && L_den != 0)
        {
            if (L_num <= 0)
                res_buf[i] = 0;
            else
            {
                tmp = norm_l(L_den);
                tmp = div_s(extract_h(L_shl(L_num, tmp)), extract_h(L_shl(L_den, tmp)));
                res_buf[i] = shr(tmp, 3);
            }
        }
        else if (L_num >= L_den)
            res_buf[i] = CNST_1_00_Q4_12;
        else
            res_buf[i] = CNST_1_00_Q4_12;

        den_part_acc = sub(den_part_acc, CNST_0_0031_Q1_15);
    }
}

int main(void)
{
    extern Flag Overflow;

    /* norm_s: exhaustive */
    for (int x = -32768; x < 32768; x++)
        CHECK(norm_s((Word16)x) == ref_norm_s((Word16)x),
              "norm_s(%d) = %d ref %d", x, norm_s((Word16)x), ref_norm_s((Word16)x));

    /* norm_l: boundaries + random */
    const Word32 edge32[] = {0, 1, -1, (Word32)0x3fffffffL, (Word32)0x40000000L, (Word32)0x7fffffffL,
                             (Word32)0xc0000000L, (Word32)0xffffffffL, (Word32)0x80000000L, 2, -2};
    for (size_t i = 0; i < sizeof(edge32)/sizeof(edge32[0]); i++)
        CHECK(norm_l(edge32[i]) == ref_norm_l(edge32[i]),
              "norm_l(0x%08lx) = %d ref %d", (unsigned long)edge32[i],
              norm_l(edge32[i]), ref_norm_l(edge32[i]));
    for (long i = 0; i < 2000000; i++)
    {
        Word32 x = (Word32)rng() | (((Word32)rng()) << 16);
        CHECK(norm_l(x) == ref_norm_l(x), "norm_l(0x%08lx)", (unsigned long)x);
    }

    /* div_s: random valid pairs + boundaries */
    for (long i = 0; i < 3000000; i++)
    {
        Word16 v2 = (Word16)(1 + (int)(rng() % 32767));
        Word16 v1 = (Word16)(rng() % (v2 + 1));
        Word16 a = div_s(v1, v2), b = ref_div_s(v1, v2);
        CHECK(a == b, "div_s(%d,%d) = %d ref %d", v1, v2, a, b);
    }

    /* L_shl: boundaries + random (value and Overflow) */
    const Word32 edgeL[] = {0, 1, -1, (Word32)0x3fffffffL, (Word32)0x40000000L, (Word32)0x7fffffffL,
                            (Word32)0xc0000000L, (Word32)0xffffffffL, (Word32)0x80000000L,
                            2, -2, (Word32)0x7ffffffeL};
    int ovf_new, ovf_ref;
    for (int e = 0; e < 13; e++)
        for (int n = -35; n <= 35; n++)
        {
            Word32 a = L_shl(edgeL[e], (Word16)n);
            Word32 b = ref_L_shl(edgeL[e], (Word16)n, &ovf_ref);
            CHECK(a == b, "L_shl(0x%08lx,%d) = 0x%08lx ref 0x%08lx",
                  (unsigned long)edgeL[e], n, (unsigned long)a, (unsigned long)b);
        }
    for (long i = 0; i < 2000000; i++)
    {
        Word32 x = (Word32)rng() | (((Word32)rng()) << 16);
        int n = (int)(rng() % 65) - 32;
        Overflow = 0;
        Word32 a = L_shl(x, (Word16)n);
        int ov = Overflow;
        Word32 b = ref_L_shl(x, (Word16)n, &ovf_ref);
        CHECK(a == b, "L_shl(0x%08lx,%d) = 0x%08lx ref 0x%08lx",
              (unsigned long)x, n, (unsigned long)a, (unsigned long)b);
        CHECK(ov == ovf_ref, "L_shl(0x%08lx,%d) ovf %d ref %d",
              (unsigned long)x, n, ov, ovf_ref);
    }

    /* autocorr: random buffers incl. full-scale, all shifts, ss in {0,5} */
    static Word16 buf[301];
    imbe_encoder enc;
    for (long t = 0; t < 200000; t++)
    {
        int kind = t % 4;
        if (kind == 0) /* random 16-bit full range */
            for (int i = 0; i < 301; i++) buf[i] = (Word16)(int)(rng() % 65536) - 32768;
        else if (kind == 1) /* full-scale alternation */
            for (int i = 0; i < 301; i++) buf[i] = (i & 1) ? -32768 : 32767;
        else if (kind == 2) /* full-scale constant */
            for (int i = 0; i < 301; i++) buf[i] = (t & 1) ? 32767 : -32768;
        else                /* heavy low-range noise */
            for (int i = 0; i < 301; i++) buf[i] = (Word16)(int)(rng() % 2048) - 1024;

        Word16 shift = (Word16)(21 + (int)(rng() % 130));
        Word16 ss = (t % 2) ? 5 : 0;
        Overflow = 0;
        Word32 a = enc.autocorr(buf, shift, ss);
        int ov = Overflow;
        Overflow = 0;
        Word32 b = ref_autocorr(buf, shift, ss);
        int ovb = Overflow;
        CHECK(a == b, "autocorr(shift=%d,ss=%d,t=%ld) = 0x%08lx ref 0x%08lx",
              shift, ss, t, (unsigned long)a, (unsigned long)b);
        CHECK(ov == ovb, "autocorr ovf shift=%d ss=%d: %d ref %d", shift, ss, ov, ovb);
    }


    /* pe_lpf: random incl. full-scale */
    {
        static Word16 in1[301], in2[301], out1[301], out2[301], m1[PE_LPF_ORD], m2[PE_LPF_ORD];
        for (long t = 0; t < 4000; t++)
        {
            memset(m1, 0, sizeof m1); memset(m2, 0, sizeof m2);
            int kind = t % 3;
            for (int i = 0; i < 301; i++)
            {
                if (kind == 0)      { in1[i] = (Word16)(int)(rng() % 65536) - 32768; }
                else if (kind == 1) { in1[i] = (t & 1) ? 32767 : -32768; }
                else                { in1[i] = (Word16)(30000 * (((int)(rng() % 7) - 3))); }
                in2[i] = in1[i];
            }
            ref_pe_lpf(in1, out1, m1, 301);
            pe_lpf(in2, out2, m2, 301);
            for (int i = 0; i < 301; i++)
                CHECK(out1[i] == out2[i], "pe_lpf t=%ld i=%d: %d ref %d (mem %d/%d)",
                      t, i, out2[i], out1[i], m2[10], m1[10]);
            CHECK(memcmp(m1, m2, sizeof m1) == 0, "pe_lpf mem t=%ld", t);
        }
    }

    /* dc_rmv: random incl. full-scale */
    {
        static Word16 in1[301], in2[301], out1[301], out2[301];
        Word32 m1 = 0, m2 = 0;
        for (long t = 0; t < 4000; t++)
        {
            m1 = m2 = 0;
            for (int i = 0; i < 301; i++)
            {
                if (t % 2 == 0)      in1[i] = (Word16)(int)(rng() % 65536) - 32768;
                else if (t % 5 == 0) in1[i] = (Word16)(((i & 1) ? -32768 : 32767));
                else                 in1[i] = (Word16)((int)(rng() % 1000) - 500);
                in2[i] = in1[i];
            }
            ref_dc_rmv(in1, out1, &m1, 301);
            dc_rmv(in2, out2, &m2, 301);
            for (int i = 0; i < 301; i++)
                CHECK(out1[i] == out2[i], "dc_rmv t=%ld i=%d: %d ref %d", t, i, out2[i], out1[i]);
            CHECK(m1 == m2, "dc_rmv mem t=%ld: 0x%08lx ref 0x%08lx", t,
                  (unsigned long)m2, (unsigned long)m1);
        }
    }

    /* fft: random data (incl. full-scale), both isigns */
    {
        static Word16 wrt[129], wit[129];
        static Word16 d1[512], d2[512];
        fft_init(wrt, wit);
        for (long t = 0; t < 2000; t++)
        {
            int kind = t % 3;
            for (int i = 0; i < 512; i++)
            {
                if (kind == 0)      d1[i] = (Word16)(int)(rng() % 65536) - 32768;
                else if (kind == 1) d1[i] = (Word16)(((i * 13 + (int)t) % 2) ? -32768 : 32767);
                else                d1[i] = (Word16)((int)(rng() % 20000) - 10000);
                d2[i] = d1[i];
            }
            Word16 isign = (t % 2) ? -1 : 1;
            ref_fft(wrt, wit, d1, 256, isign);
            fft(wrt, wit, d2, 256, isign);
            for (int i = 0; i < 512; i++)
                CHECK(d1[i] == d2[i], "fft t=%ld isign=%d i=%d: %d ref %d",
                      t, isign, i, d2[i], d1[i]);
        }
    }

    /* e_p: random + full-scale (full-scale square wave forces the
       L_sum == MAX_32 rescale branch) */
    {
        static Word16 inbuf[301], r1[203], r2[203];
        imbe_encoder enc;   /* #define private public gives access */
        for (long t = 0; t < 600; t++)
        {
            int kind = t % 4;
            for (int i = 0; i < 301; i++)
            {
                if (kind == 0)      inbuf[i] = (Word16)(int)(rng() % 65536) - 32768;
                else if (kind == 1) inbuf[i] = (Word16)(((i & 1) ? -32768 : 32767));
                else if (kind == 2) inbuf[i] = (Word16)((int)(rng() % 4000) - 2000);
                else                inbuf[i] = (Word16)(32767 * (((int)(rng() % 5) - 2)));
            }
            ref_e_p(inbuf, r1);
            enc.e_p(inbuf, r2);
            for (int i = 0; i < 203; i++)
                CHECK(r1[i] == r2[i], "e_p t=%ld i=%d: %d ref %d", t, i, r2[i], r1[i]);
        }
    }

    if (failures == 0)
        printf("perf_check: ALL PASS (norm_s exhaustive, norm_l/div_s/L_shl/autocorr/e_p/pe_lpf/dc_rmv/fft randomised)\n");
    else
        printf("perf_check: %ld FAILURES\n", failures);
    return failures != 0;
}
