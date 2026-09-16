/* the emulator's MSA decoder (atari_fdd.c msa_open_decoded), verbatim logic,
 * as a file-to-file tool: msachk in.msa out.st */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    uint8_t *raw = NULL, *img = NULL;
    uint8_t magic[2];
    if (!f || fread(magic, 1, 2, f) != 2 || magic[0] != 0x0E || magic[1] != 0x0F) return 1;
    fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsz < 12 || fsz > 2 * 1024 * 1024) return 2;
    raw = malloc(fsz);
    if (fread(raw, 1, fsz, f) != (size_t)fsz) return 2;
    fclose(f);
    uint16_t spt = (raw[2] << 8) | raw[3];
    uint16_t sides = ((raw[4] << 8) | raw[5]) + 1;
    uint16_t tstart = (raw[6] << 8) | raw[7];
    uint16_t tend = (raw[8] << 8) | raw[9];
    uint32_t tlen = 512u * spt;
    uint32_t total = tlen * sides * (uint32_t)(tend - tstart + 1);
    if (!spt || spt > 12 || sides > 2 || tend < tstart || total > 2048u * 1024) return 2;
    img = calloc(1, total);
    uint32_t src = 10, dst = 0;
    for (uint32_t t = 0; t < (uint32_t)(tend - tstart + 1) * sides; t++) {
        if (src + 2 > (uint32_t)fsz) return 3;
        uint32_t dlen = (raw[src] << 8) | raw[src + 1];
        src += 2;
        if (src + dlen > (uint32_t)fsz || dst + tlen > total) return 3;
        if (dlen == tlen) memcpy(img + dst, raw + src, tlen);
        else {
            uint32_t o = dst, e = src + dlen, i = src;
            while (i < e && o < dst + tlen) {
                uint8_t b = raw[i++];
                if (b != 0xE5) { img[o++] = b; continue; }
                if (i + 3 > e) return 3;
                uint8_t v = raw[i];
                uint32_t n = (raw[i + 1] << 8) | raw[i + 2];
                i += 3;
                while (n-- && o < dst + tlen) img[o++] = v;
            }
        }
        src += dlen; dst += tlen;
    }
    FILE *o = fopen(argv[2], "wb"); fwrite(img, 1, total, o); fclose(o);
    return 0;
}
