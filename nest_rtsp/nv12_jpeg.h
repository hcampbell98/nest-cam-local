/*
 * nv12_jpeg.h - Minimal Baseline YUV 4:2:0 (NV12) to JPEG encoder
 * Zero external dependencies, single-threaded, ~5 KB compiled on ARMv6.
 * Encodes directly from NV12 Y + interleaved UV planes (native JPEG YCbCr 4:2:0).
 */
#ifndef NV12_JPEG_H
#define NV12_JPEG_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    FILE    *fp;
    uint32_t bit_buf;
    int      bit_cnt;
    uint8_t  out_buf[4096];
    size_t   out_pos;
} jpeg_wr_t;

static void jw_flush(jpeg_wr_t *w) {
    if (w->out_pos > 0) {
        fwrite(w->out_buf, 1, w->out_pos, w->fp);
        w->out_pos = 0;
    }
}

static inline void jw_byte(jpeg_wr_t *w, uint8_t b) {
    if (w->out_pos >= sizeof(w->out_buf)) jw_flush(w);
    w->out_buf[w->out_pos++] = b;
}

static void jw_bytes(jpeg_wr_t *w, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) jw_byte(w, p[i]);
}

static inline void jw_u16(jpeg_wr_t *w, uint16_t v) {
    jw_byte(w, (uint8_t)(v >> 8));
    jw_byte(w, (uint8_t)(v & 0xff));
}

static inline void jw_bits(jpeg_wr_t *w, uint32_t code, int nbits) {
    w->bit_buf = (w->bit_buf << nbits) | (code & ((1u << nbits) - 1u));
    w->bit_cnt += nbits;
    while (w->bit_cnt >= 8) {
        w->bit_cnt -= 8;
        uint8_t b = (uint8_t)((w->bit_buf >> w->bit_cnt) & 0xffu);
        jw_byte(w, b);
        if (b == 0xff) jw_byte(w, 0x00);
    }
}

static void jw_bits_pad(jpeg_wr_t *w) {
    if (w->bit_cnt > 0) {
        int pad = 8 - w->bit_cnt;
        jw_bits(w, (1u << pad) - 1u, pad);
    }
}

/* Standard JPEG luminance & chrominance quantization tables (Q~75) */
static const uint8_t j_qtab_y[64] = {
     8,  6,  5,  8, 12, 20, 26, 31,
     6,  6,  7, 10, 13, 29, 30, 28,
     7,  7,  8, 12, 20, 29, 35, 28,
     7,  9, 11, 15, 26, 44, 40, 31,
     9, 11, 19, 28, 34, 55, 52, 39,
    12, 18, 28, 32, 41, 52, 57, 46,
    25, 32, 39, 44, 52, 61, 60, 51,
    36, 46, 48, 49, 56, 50, 52, 50
};

static const uint8_t j_qtab_c[64] = {
     9,  9, 12, 24, 50, 50, 50, 50,
     9, 11, 13, 33, 50, 50, 50, 50,
    12, 13, 28, 50, 50, 50, 50, 50,
    24, 33, 50, 50, 50, 50, 50, 50,
    50, 50, 50, 50, 50, 50, 50, 50,
    50, 50, 50, 50, 50, 50, 50, 50,
    50, 50, 50, 50, 50, 50, 50, 50,
    50, 50, 50, 50, 50, 50, 50, 50
};

static const uint8_t j_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* Standard Annex K Huffman specs */
static const uint8_t j_dc_y_bits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t j_dc_y_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t j_dc_c_bits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t j_dc_c_vals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t j_ac_y_bits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125};
static const uint8_t j_ac_y_vals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

static const uint8_t j_ac_c_bits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,119};
static const uint8_t j_ac_c_vals[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

typedef struct {
    uint16_t code[256];
    uint8_t  len[256];
} j_huff_t;

static j_huff_t g_ht_dcy, g_ht_dcc, g_ht_acy, g_ht_acc;
static int      g_j_inited = 0;

/* Fixed-point 1D cosine matrix (Q13: round(cos((2x+1)*u*pi/16) * c(u)/2 * 8192)) */
static const int32_t j_dct_cos[8][8] = {
    {  2896,  2896,  2896,  2896,  2896,  2896,  2896,  2896 },
    {  4017,  3406,  2276,   799,  -799, -2276, -3406, -4017 },
    {  3784,  1567, -1567, -3784, -3784, -1567,  1567,  3784 },
    {  3406,  -799, -4017, -2276,  2276,  4017,   799, -3406 },
    {  2896, -2896, -2896,  2896,  2896, -2896, -2896,  2896 },
    {  2276, -4017,   799,  3406, -3406,  -799,  4017, -2276 },
    {  1567, -3784,  3784, -1567, -1567,  3784, -3784,  1567 },
    {   799, -2276,  3406, -4017,  4017, -3406,  2276,  -799 }
};

static void j_build_huff(j_huff_t *ht, const uint8_t bits[16], const uint8_t *vals) {
    memset(ht, 0, sizeof(*ht));
    uint16_t code = 0;
    int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++) {
            uint8_t sym = vals[k++];
            ht->code[sym] = code;
            ht->len[sym] = (uint8_t)len;
            code++;
        }
        code <<= 1;
    }
}

static void j_init_tables(void) {
    if (g_j_inited) return;
    j_build_huff(&g_ht_dcy, j_dc_y_bits, j_dc_y_vals);
    j_build_huff(&g_ht_dcc, j_dc_c_bits, j_dc_c_vals);
    j_build_huff(&g_ht_acy, j_ac_y_bits, j_ac_y_vals);
    j_build_huff(&g_ht_acc, j_ac_c_bits, j_ac_c_vals);
    g_j_inited = 1;
}

static inline int j_bit_size(int v) {
    if (v < 0) v = -v;
    int s = 0;
    while (v) { v >>= 1; s++; }
    return s;
}

/* Pure-integer separable 8x8 FDCT + quantize + Huffman encode */
static void j_encode_block(jpeg_wr_t *w, const int16_t in[64], const uint8_t qtab[64],
                           const j_huff_t *hdc, const j_huff_t *hac, int *prev_dc) {
    int32_t tmp[64];
    int16_t coef[64];

    /* Row 1D DCT: input in [-128..127], cos scaled by 8192 -> shift right 10 (keeps 3 fractional bits) */
    for (int y = 0; y < 8; y++) {
        const int16_t *r = in + y * 8;
        int32_t *t = tmp + y * 8;
        for (int u = 0; u < 8; u++) {
            const int32_t *c = j_dct_cos[u];
            int32_t s = r[0]*c[0] + r[1]*c[1] + r[2]*c[2] + r[3]*c[3]
                      + r[4]*c[4] + r[5]*c[5] + r[6]*c[6] + r[7]*c[7];
            t[u] = (s + 512) >> 10;
        }
    }

    /* Col 1D DCT + Quantize: shift right 16 (total shift 26 = 13+13) */
    for (int u = 0; u < 8; u++) {
        for (int v = 0; v < 8; v++) {
            const int32_t *c = j_dct_cos[v];
            int32_t s = tmp[0*8+u]*c[0] + tmp[1*8+u]*c[1] + tmp[2*8+u]*c[2] + tmp[3*8+u]*c[3]
                      + tmp[4*8+u]*c[4] + tmp[5*8+u]*c[5] + tmp[6*8+u]*c[6] + tmp[7*8+u]*c[7];
            int32_t q = qtab[v * 8 + u];
            int32_t val = (s >= 0) ? ((s + (q << 15)) / (q << 16))
                                   : -((-s + (q << 15)) / (q << 16));
            coef[v * 8 + u] = (int16_t)val;
        }
    }

    /* DC coefficient */
    int dc = coef[0];
    int diff = dc - *prev_dc;
    *prev_dc = dc;
    int s = j_bit_size(diff);
    jw_bits(w, hdc->code[s], hdc->len[s]);
    if (s > 0) {
        int v = (diff < 0) ? (diff + ((1 << s) - 1)) : diff;
        jw_bits(w, (uint32_t)v, s);
    }

    /* AC coefficients in zigzag order */
    int zrun = 0;
    for (int k = 1; k < 64; k++) {
        int ac = coef[j_zigzag[k]];
        if (ac == 0) {
            zrun++;
        } else {
            while (zrun >= 16) {
                jw_bits(w, hac->code[0xf0], hac->len[0xf0]);
                zrun -= 16;
            }
            int as = j_bit_size(ac);
            uint8_t sym = (uint8_t)((zrun << 4) | as);
            jw_bits(w, hac->code[sym], hac->len[sym]);
            int av = (ac < 0) ? (ac + ((1 << as) - 1)) : ac;
            jw_bits(w, (uint32_t)av, as);
            zrun = 0;
        }
    }
    if (zrun > 0) {
        jw_bits(w, hac->code[0x00], hac->len[0x00]); /* EOB */
    }
}

static void j_write_dqt(jpeg_wr_t *w, uint8_t id, const uint8_t qtab[64]) {
    jw_u16(w, 0xffdb);
    jw_u16(w, 67);
    jw_byte(w, id);
    for (int i = 0; i < 64; i++) jw_byte(w, qtab[j_zigzag[i]]);
}

static void j_write_dht(jpeg_wr_t *w, uint8_t tc_th, const uint8_t bits[16], const uint8_t *vals, int nvals) {
    jw_u16(w, 0xffc4);
    jw_u16(w, (uint16_t)(19 + nvals));
    jw_byte(w, tc_th);
    jw_bytes(w, bits, 16);
    jw_bytes(w, vals, (size_t)nvals);
}

static bool nv12_save_jpeg(const char *path, const uint8_t *y_plane, const uint8_t *uv_plane,
                           int width, int height, int pitch) {
    if (!y_plane || width < 16 || height < 16 || pitch < width) return false;
    j_init_tables();

    FILE *fp = fopen(path, "wb");
    if (!fp) return false;

    jpeg_wr_t w;
    memset(&w, 0, sizeof(w));
    w.fp = fp;

    /* SOI + APP0 (JFIF 1.01) */
    jw_u16(&w, 0xffd8);
    static const uint8_t app0[] = {
        0xff,0xe0, 0x00,0x10, 'J','F','I','F',0x00, 0x01,0x01, 0x00, 0x00,0x01, 0x00,0x01, 0x00,0x00
    };
    jw_bytes(&w, app0, sizeof(app0));

    /* DQT */
    j_write_dqt(&w, 0, j_qtab_y);
    j_write_dqt(&w, 1, j_qtab_c);

    /* SOF0: Baseline DCT, 8-bit, YCbCr 4:2:0 (Y:2x2, Cb:1x1, Cr:1x1) */
    jw_u16(&w, 0xffc0);
    jw_u16(&w, 17);
    jw_byte(&w, 8);
    jw_u16(&w, (uint16_t)height);
    jw_u16(&w, (uint16_t)width);
    jw_byte(&w, 3);
    jw_byte(&w, 1); jw_byte(&w, 0x22); jw_byte(&w, 0); /* Y */
    jw_byte(&w, 2); jw_byte(&w, 0x11); jw_byte(&w, 1); /* Cb */
    jw_byte(&w, 3); jw_byte(&w, 0x11); jw_byte(&w, 1); /* Cr */

    /* DHT */
    j_write_dht(&w, 0x00, j_dc_y_bits, j_dc_y_vals, 12);
    j_write_dht(&w, 0x10, j_ac_y_bits, j_ac_y_vals, 162);
    j_write_dht(&w, 0x01, j_dc_c_bits, j_dc_c_vals, 12);
    j_write_dht(&w, 0x11, j_ac_c_bits, j_ac_c_vals, 162);

    /* SOS */
    jw_u16(&w, 0xffda);
    jw_u16(&w, 12);
    jw_byte(&w, 3);
    jw_byte(&w, 1); jw_byte(&w, 0x00);
    jw_byte(&w, 2); jw_byte(&w, 0x11);
    jw_byte(&w, 3); jw_byte(&w, 0x11);
    jw_byte(&w, 0); jw_byte(&w, 63); jw_byte(&w, 0);

    int prev_y = 0, prev_cb = 0, prev_cr = 0;
    int16_t blk[64];

    for (int my = 0; my < height; my += 16) {
        for (int mx = 0; mx < width; mx += 16) {
            /* 4 Y blocks (TL, TR, BL, BR) */
            for (int by = 0; by < 2; by++) {
                for (int bx = 0; bx < 2; bx++) {
                    for (int r = 0; r < 8; r++) {
                        int sy = my + by * 8 + r;
                        if (sy >= height) sy = height - 1;
                        const uint8_t *row = y_plane + sy * pitch;
                        for (int c = 0; c < 8; c++) {
                            int sx = mx + bx * 8 + c;
                            if (sx >= width) sx = width - 1;
                            blk[r * 8 + c] = (int16_t)row[sx] - 128;
                        }
                    }
                    j_encode_block(&w, blk, j_qtab_y, &g_ht_dcy, &g_ht_acy, &prev_y);
                }
            }
            /* Cb (U) block */
            for (int r = 0; r < 8; r++) {
                int sy = (my / 2) + r;
                if (sy >= height / 2) sy = (height / 2) - 1;
                const uint8_t *uvrow = uv_plane ? (uv_plane + sy * pitch) : NULL;
                for (int c = 0; c < 8; c++) {
                    int sx = (mx / 2) + c;
                    if (sx >= width / 2) sx = (width / 2) - 1;
                    blk[r * 8 + c] = uvrow ? ((int16_t)uvrow[sx * 2] - 128) : 0;
                }
            }
            j_encode_block(&w, blk, j_qtab_c, &g_ht_dcc, &g_ht_acc, &prev_cb);

            /* Cr (V) block */
            for (int r = 0; r < 8; r++) {
                int sy = (my / 2) + r;
                if (sy >= height / 2) sy = (height / 2) - 1;
                const uint8_t *uvrow = uv_plane ? (uv_plane + sy * pitch) : NULL;
                for (int c = 0; c < 8; c++) {
                    int sx = (mx / 2) + c;
                    if (sx >= width / 2) sx = (width / 2) - 1;
                    blk[r * 8 + c] = uvrow ? ((int16_t)uvrow[sx * 2 + 1] - 128) : 0;
                }
            }
            j_encode_block(&w, blk, j_qtab_c, &g_ht_dcc, &g_ht_acc, &prev_cr);
        }
    }

    jw_bits_pad(&w);
    jw_u16(&w, 0xffd9); /* EOI */
    jw_flush(&w);
    fclose(fp);
    return true;
}

#endif /* NV12_JPEG_H */
