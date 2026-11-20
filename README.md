# IMBE-embedded

IMBE (4400 bps) vocoder with a clean C API, split encoder/decoder
state and file tools, optimized for real-time encode on Cortex-M4
class MCUs. NOTE: Optimizations largely vibed with Qwen 38-27B.
Caveat for those so opposed.

Engine: Pavel Yazev's fixed-point IMBE encoder/decoder as vendored by
op25 (GPLv3+), further optimized (e_p result caching, Pattern-A
saturating-MAC rewrites, FFT and pitch-refinement work) with
byte-identical output throughout - every change is gated on golden
bitstreams.  This is the only IMBE implementation of the family that
fits the 20 ms worst-case encode budget on a 100 MHz Cortex-M4.

Features:

- **C API with caller-owned static state** (`inc/imbe2.h`): no heap
  after init, no create/destroy pair, state sizes queryable at runtime
  for static allocation.
- **Split encoder/decoder objects**: all parameter exchange happens
  through the frame bits, so each side is fully self-contained - an
  RX-only build needs 1,932 B of codec state.
- **`imbe_enc` / `imbe_dec`** file tools.
- Self-describing `.imbe` file format (8-byte `IMB1` header, version 2)
  with a `--raw` mode for headerless 11-byte frames carrying the
  standard 88-bit prioritized info stream.
- **Corrected MONO65 gain ladder**: the bottom 8 levels follow the
  reference firmware (Annex-E minus 0.25*(8-k), Q11); this alone drops
  the digital-silence decode floor from RMS 33 to 11.
- **Two encoders**: `imbe_enc` matches the reference Yazev/op25 encoder
  bit-for-bit; `imbe_enc2` adds the enhanced analysis -
  leashed-continuity pitch tracking, wider voicing hysteresis,
  stricter octave-switch gating and a min-statistics noise gate (see
  below).  Same 88-bit frames out.

## Frame format

20 ms frames: 160 samples (8 kHz mono s16le) <-> 11 bytes (88 bits,
8 x 11-bit u-vectors, word 0 in the low bits, big-endian byte order).
4400 bps.

## API

```c
#include "imbe2.h"

/* split (encoder-only / decoder-only) */
static unsigned char enc[8192], dec[8192];
imbe2_encoder_init(enc);            /* 6676 B (state sizes below)   */
imbe2_decoder_init(dec);            /* 1932 B                       */

int16_t speech[IMBE2_SAMPLES_PER_FRAME]; /* 160 */
uint8_t bits[IMBE2_BYTES_PER_FRAME];     /* 11  */
imbe2_encode(enc, bits, speech);
imbe2_decode(dec, speech, bits);

/* or combined full-duplex object (8608 bytes) with accessors */
static unsigned char both[8192];
imbe2_init(both);
imbe2_encode(imbe2_encoder_from(both), bits, speech);
imbe2_decode(imbe2_decoder_from(both), speech, bits);
```

Every object is caller-owned static allocation; no heap after init.

## State sizes

| Object | Size |
|--------|------|
| combined | 8,608 B |
| encoder-only | 6,676 B |
| decoder-only | 1,932 B |

The split grows the combined footprint slightly (each side carries
its own frame workspace, FFT twiddles and voicing-decision array);
decoder-only saves 76 % vs combined.

## Enhanced encoder (`imbe_enc2`, `imbe2_encoder_set_enh`)

Measured on a 28 s speech corpus (1399 frames):

- **Leashed-continuity pitch tracking** (expired-patent soft-decision
  tracking, US4,885,790/US5,081,681): among candidates whose e_p cost
  is within 0.024 of the raw minimum - on both the look-back and the
  3-window look-ahead paths - prefer the pitch nearest the previous
  frame.  Pitch-track oscillations 462 -> 240 (warble), with the
  spectral fit essentially unchanged.
- **Wider voicing hysteresis** (0.40/0.62 vs 0.45/0.5625) and
  stricter sub-multiple (octave) switch evidence (0.75 vs 0.588):
  octave errors 145 -> 122, v/uv flap 10.5 % -> 9.5 %.
- **Noise gate** (time domain): min-statistics floor (3 s windowed
  minimum of frame power), asymmetric hysteresis (open at E > 25*Fmin,
  close at E < 4*Fmin), closed frames muted to true digital silence
  before analysis.  Settled-pause decode RMS 88 -> 36, speech
  untouched.  Gating must be a real zero - synthesised partial gain
  steps ring the predictive log2-DCT SA quantizer.

## Tools

```
imbe_enc  input.raw output.imbe         # standard encoder (bit-exact imbe-mod)
imbe_enc2 input.raw output.imbe         # enhanced encoder
... --raw                               # headerless 11-byte frames either way
imbe_dec  output.imbe out.raw           # auto-detects the header
imbe_dec  output.imbe out.raw --raw     # force headerless
```

## Build and test

```
cmake -B build && cmake --build build
cd build && ctest
```

`imbe2test` runs an in-process synthetic round-trip test (sane
energy, no clipping) and - when a golden oracle is placed in
`testdata/` - a bit-exactness check of every encoded frame against
it.  The goldens shipped in `testdata/` (one `.frames` file per
corpus, enc and encenh) are regenerated with the bundled `mkgolden`
tool; any change to encoder or decoder must keep them byte-exact.

## Performance

Host (x86-64, testdata corpora, 30 passes, in-process `imbebench`):
encode 56-58 us avg / ~109 us worst per 20 ms frame, decode 15 us avg.
The enhancements add ~0.5 % over the standard encoder (the noise gate
makes pauses cheap: muted frames encode faster than they would with
noise present).

Encoder first-frame behaviour: the e_p result cache is pre-warmed at
`imbe2_encoder_init` (two e_p evaluations over the zeroed pitch
buffer), so frame 0 costs roughly a steady-state frame instead of
~1.5x that; the two extra e_p evaluations are paid once at init.

### Cortex-M4 / M7 measured 

Replaces the earlier 2.17 c/Ir projection (calibrated from a different
codec): these are exact retired-ARM-instruction counts of the actual
`-O3 -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard`
build (gcc 16.2 arm-none-eabi, Thumb-2), counted per frame inside
QEMU 11 via a TCG plugin (exact icount + PC markers; harness in
~/Projects/imbe-showdown/arm).  Corpus: 28 s LibriVox speech,
1399 frames, interleaved encode+decode.  Encode bitstream checksum
bit-identical to the x86 build.

| Workload | avg | p95 | worst | worst frame |
|---|---:|---:|---:|---|
| encode | 579,220 | 619,082 | 653,350 | #108 (voiced->unvoiced) |
| encode (enhanced, `set_enh`) | 586,319 | 627,455 | 662,421 | #108 (same frame) |
| decode | 187,321 | - | 431,988 | #585 |

Frame 0 costs 596,247 (~avg), confirming the e_p cache pre-warm.
The enhanced encoder (`imbe2_encoder_set_enh(enc, 1)`, re-measured
2026-11-20 on the reconciled tree: standard encode still counts
579,240 avg / CHK 0x15ec5f6a) costs +1.2 % instructions on M4:
586,319 avg / 662,421 worst, and 584,289 avg / 658,254 worst on the
cortex-m7 build; its worst frame is the same #108 as the standard
encoder.  Enhanced bitstreams carry their own CHK 0x47715f7f (the
enhanced path intentionally changes decisions, not format).
Instruction counts are identical for the cortex-m7 build.

| | worst enc @100 MHz | @168 MHz | worst enc @400 MHz (M7) |
|---|---:|---:|---:|
| @ IPC 1.0 | 6.53 ms | 3.89 ms | 1.63 ms |

Times assume IPC 1.0 (no wait states); flash-resident code with ART
typically lands 0.8-1.0, so these are mildly optimistic but the
worst-case budget (20 ms = 2M insns @100 MHz) holds by >3x either
way.  Hardware timing remains the open item.

### Cross-codec benchmark (QEMU harness, 2026-11-19)

Encode instructions/frame, exact retired ARM insns, same corpus and
flags as above (MBE row is mbelib-neo, float, SIMD unavailable; its
M7 row is the cortex-m7 build with hardware doubles):

| Encoder | M4 avg | M4 p95 | M4 worst | M7 avg | M7 worst | worst @100M | worst @168M | worst @400M(M7) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| **IMBE-embedded (this repo)** | **579k** | **619k** | **653k** | **577k** | **649k** | **6.5 ms** | 3.9 ms | 1.6 ms |
| IMBE-embedded enhanced encoder | 586k | 627k | 662k | 584k | 658k | 6.6 ms | 3.9 ms | 1.7 ms |
| Yazev v1.0 (op25-lineage, unmodified) | 3402k | 4107k | 4237k | 3405k | 4245k | 42.4 ms | 25.2 ms | 10.6 ms |
| mbelib-neo (float) | 754k | 1169k | 1565k* | 395k | 506k | 15.6 ms | 9.3 ms | 1.3 ms |
| blip25-mbe (Rust, float)** | 268,170k | 353,140k | 454,150k | 8893k | 14,600k | 4542 ms | 2703 ms | 36.5 ms |

\* MBE M4 worst is frame 0 (pffft plan setup); steady-state worst
1510k.  mbelib-neo has no fixed-point path and its text does not fit
F405 RAM; on M7 its insn count drops 1.9x (hardware doubles).
\** blip25-mbe @ fc693ba, no_std port (2026-11-20 sweep), measured on
mps2-an500; 286 steady-state frames (frame 0 and a one-time 1.8 G
arena, first-touch excluded); M4 row is a -Ctarget-feature=-fp64 build
(hardware f32 kept, f64 via soft libcalls to provide a valid stm32f4 test).
Decoder side: 406k insns/frame on M7 (349k with its fixed-point
decoder), and -fp64 does not inflate decode at all (397k) - the
soft-double disaster is encoder-only (f64-heavy analysis).  Its encoder
CHK was identical across all three ARM builds; the working set is the
other disqualifier: ~8 MB heap packed (host glibc RSS), so BLIP25
needs SDRAM-class memory regardless of compute.  A large share of its
M7 encoder cost is buffer zeroing through byte-wise compiler_builtins
memset; a word-optimized memset could plausibly cut 50-70 % of that
encoder cost, but has not been measured.
Worst-case real-time at 20 ms/frame on a 100 MHz M4: only
IMBE-embedded fits with headroom; unmodified Yazev needs ~4x more
clock than an F405 gives, and BLIP25 misses the M7/400 MHz budget
too (23 ms/frame average even with hardware doubles, before its
memory requirement).

An earlier version of this section compared imbe-mod/imbe-mod2 with host-x86
instruction counts scaled by the 2.17 c/Ir estimate; those are dropped
rather than mixed with direct counts (check git history if the projection
comparison interests you).  Related ARM facts from the same sweep:
unmodified Yazev's encoder is additionally not reproducible from a fresh
constructor (file-scope state leaks across instances); both fixed-point
codecs' worst-case frame was the same corpus frame.

### Cortex-M4 static footprint (`tools/size_report.sh`, -Os + newlib-nano)

| Image | Flash (text+rodata) | RAM (data+bss) |
|---|---:|---:|
| encoder (+8,608 B codec state) | 19.0 KiB | 17.1 KiB |
| decoder (+1,932 B codec state) | 13.7 KiB | 16.8 KiB |

(The RAM figures include the oversized 16 KiB state buffers of the
demo images; size the buffers to `imbe2_encoder_state_size()` /
`imbe2_decoder_state_size()` in a real build.)
