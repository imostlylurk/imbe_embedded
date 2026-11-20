/* mkgolden: encode a raw 8kHz s16le file and write bare 11-byte frames.
 * usage: mkgolden [enc|encenh] input.raw output.frames
 * Fresh encoder state; identical pipeline to imbe2_encode().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imbe2.h"

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "usage: mkgolden enc|encenh in.raw out.frames\n"); return 1; }
    int enh = !strcmp(argv[1], "encenh");
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    int frames = n / 320;
    int16_t *pcm = malloc((size_t)frames * 320);
    if (fread(pcm, 320, (size_t)frames, f) != (size_t)frames) { fprintf(stderr, "short read\n"); return 1; }
    fclose(f);

    static unsigned char state[16384];
    imbe2_encoder_init(state);
    if (enh) imbe2_encoder_set_enh(state, 1);
    FILE *o = fopen(argv[3], "wb");
    if (!o) { perror(argv[3]); return 1; }
    uint8_t bits[IMBE2_BYTES_PER_FRAME];
    for (int i = 0; i < frames; i++)
    {
        imbe2_encode(state, bits, &pcm[i * 160]);
        fwrite(bits, 1, sizeof(bits), o);
    }
    fclose(o);
    free(pcm);
    return 0;
}
