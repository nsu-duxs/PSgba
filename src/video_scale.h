#ifndef VIDEO_SCALE_H
#define VIDEO_SCALE_H

/*
 * Pipeline de ampliacao SIMD (SSE2/SSSE3/SSE4.1, sem AVX2: Jaguar btver2) independente de Orbis.
 *
 *   [240x160 XBGR] -> vscale_convert_xbgr -> [240x160 ARGB]
 *                  -> vscale_build (filtro + fator interno N) -> [240N x 160N ARGB]
 *                  -> vscale_fit_rows (viewport da TV, nearest ou bilinear) -> framebuffer
 *
 * O fator interno N so decide COMO o framebuffer nativo e ampliado/filtrado.
 * O tamanho exibido na TV (viewport) e decidido fora, em video_split.c.
 * Nenhuma funcao aqui aloca memoria: todos os buffers sao fornecidos pelo chamador.
 */

#include <stdint.h>
#include <stdbool.h>

#define VSCALE_SRC_W            240
#define VSCALE_SRC_H            160
#define VSCALE_MAX_FACTOR       6
#define VSCALE_MAX_DST_W        1920

/* Tamanhos (em pixels) dos buffers que o chamador deve fornecer a vscale_build() */
#define VSCALE_BUF_FINAL_PIXELS (VSCALE_SRC_W * VSCALE_MAX_FACTOR * VSCALE_SRC_H * VSCALE_MAX_FACTOR)
#define VSCALE_BUF_MID_PIXELS   (VSCALE_SRC_W * 2 * VSCALE_SRC_H * 2)

typedef enum VScaleAlgo {
    VSCALE_NEAREST = 0, /* replicacao inteira (pixel puro)                         */
    VSCALE_SCALEX  = 1, /* Scale2x/Scale3x (amadvance/scale2x), sem desfoque        */
    VSCALE_HQ      = 2, /* mesma deteccao de arestas, cantos mesclados 75/25 (HQ-like) */
    VSCALE_XBR     = 3  /* arestas com tolerancia de cor + mescla 50/50 (xBR-lite)  */
} VScaleAlgo;

/* Tabela horizontal pre-computada (uma vez por mudanca de configuracao, nunca por frame) */
typedef struct VScaleMap {
    int      src_w;
    int      dst_w;
    bool     smooth;
    bool     valid;
    uint16_t x0[VSCALE_MAX_DST_W];
    uint8_t  fx[VSCALE_MAX_DST_W];
} VScaleMap;

/* 240x160 XBGR (mGBA) -> ARGB. 8 pixels por iteracao (pshufb). */
void vscale_convert_xbgr(const uint32_t* src_xbgr, uint32_t* dst_argb);

/*
 * Amplia src1x (240x160 ARGB) por `factor` (1..6). Retorna o ponteiro da imagem final
 * (240*factor x 160*factor): src1x se factor == 1, senao final_buf.
 * final_buf precisa de 240*factor*160*factor pixels; mid_buf de VSCALE_BUF_MID_PIXELS.
 */
const uint32_t* vscale_build(VScaleAlgo algo, int factor, const uint32_t* src1x,
                             uint32_t* final_buf, uint32_t* mid_buf);

/* Replicacao inteira generica (k >= 2). SIMD para k = 2, 3, 4, 6. */
void vscale_nearest(const uint32_t* src, int w, int h, uint32_t* dst, int k);

void vscale_map_build(VScaleMap* map, int src_w, int dst_w, bool smooth);

/*
 * Ajusta src (sw x sh) ao viewport dw x dh e escreve as linhas [row0, row1) em dst
 * (dst = origem do viewport no framebuffer, dst_stride em pixels).
 * Nunca le do framebuffer (memoria write-combining): usa scratch_a/scratch_b (>= dw pixels).
 * Linhas disjuntas podem ser processadas em threads distintas (scratch distintos).
 */
void vscale_fit_rows(const uint32_t* src, int sw, int sh,
                     uint32_t* dst, int dst_stride, int dw, int dh,
                     int row0, int row1, const VScaleMap* map,
                     uint32_t* scratch_a, uint32_t* scratch_b);

#endif /* VIDEO_SCALE_H */

