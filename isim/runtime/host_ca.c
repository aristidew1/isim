/*
 * libisim_host Core Animation helpers: perspective (projective) image warps for CATransform3D layers and
 * blurred drop shadows (CALayer shadows, shadowPath).
 *
 *  - isim_image_draw_quad: maps a raster image onto a quadrilateral of the current user space with the
 *    homography that sends the image's corners to the quad's corners (a true perspective warp, bilinear
 *    sampling, computed per device pixel inside the quad's bounding box and the clip).
 *  - isim_gfx_pop_group_shadow: pops a group whose alpha is the shadow's shape and paints it blurred
 *    (3-pass box blur ~ a Gaussian), tinted and offset, under whatever is drawn next.
 */
#define _GNU_SOURCE
#include "guest_abi.h"
#include <cairo.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

cairo_t *isim_host_cairo(void);
cairo_surface_t *isim_image_surface(int handle);      /* host_image.c: raster surface of a handle (NULL otherwise) */

/* homography H (3x3, row-major) with H * (u, v, 1) ~ (x, y, 1) for the unit square corners (0,0) (1,0) (1,1) (0,1) */
static int square_to_quad(const double q[8], double H[9]) {
    double x0 = q[0], y0 = q[1], x1 = q[2], y1 = q[3], x2 = q[4], y2 = q[5], x3 = q[6], y3 = q[7];
    double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
    double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    double g = 0, h = 0;
    if (fabs(sx) > 1e-9 || fabs(sy) > 1e-9) {
        double den = dx1 * dy2 - dx2 * dy1;
        if (fabs(den) < 1e-12) return 0;
        g = (sx * dy2 - dx2 * sy) / den;
        h = (dx1 * sy - sx * dy1) / den;
    }
    H[0] = x1 - x0 + g * x1; H[1] = x3 - x0 + h * x3; H[2] = x0;
    H[3] = y1 - y0 + g * y1; H[4] = y3 - y0 + h * y3; H[5] = y0;
    H[6] = g; H[7] = h; H[8] = 1;
    return 1;
}
static int invert3(const double m[9], double o[9]) {
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (fabs(det) < 1e-15) return 0;
    o[0] = (m[4] * m[8] - m[5] * m[7]) / det; o[1] = (m[2] * m[7] - m[1] * m[8]) / det; o[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    o[3] = (m[5] * m[6] - m[3] * m[8]) / det; o[4] = (m[0] * m[8] - m[2] * m[6]) / det; o[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    o[6] = (m[3] * m[7] - m[4] * m[6]) / det; o[7] = (m[1] * m[6] - m[0] * m[7]) / det; o[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    return 1;
}

/* Draws raster image `handle` onto the quad (x0 y0 = image top-left, x1 y1 top-right, x2 y2 bottom-right, x3 y3
   bottom-left; current user space) with a perspective warp. */
ISIM_GUEST_ABI void isim_image_draw_quad(int handle, const double *quad, double alpha) {
    cairo_t *cr = isim_host_cairo();
    cairo_surface_t *src = isim_image_surface(handle);
    if (!cr || !src || !quad || alpha <= 0) return;
    cairo_surface_flush(src);
    int sw = cairo_image_surface_get_width(src), sh = cairo_image_surface_get_height(src), sstride = cairo_image_surface_get_stride(src) / 4;
    if (sw <= 0 || sh <= 0) return;
    const uint32_t *spx = (const uint32_t *)cairo_image_surface_get_data(src);
    /* the quad in device space */
    double d[8];
    for (int i = 0; i < 4; i++) { d[2 * i] = quad[2 * i]; d[2 * i + 1] = quad[2 * i + 1]; cairo_user_to_device(cr, &d[2 * i], &d[2 * i + 1]); }
    double H[9], Hi[9];
    if (!square_to_quad(d, H) || !invert3(H, Hi)) return;
    double bx0 = d[0], by0 = d[1], bx1 = d[0], by1 = d[1];
    for (int i = 1; i < 4; i++) { bx0 = fmin(bx0, d[2 * i]); bx1 = fmax(bx1, d[2 * i]); by0 = fmin(by0, d[2 * i + 1]); by1 = fmax(by1, d[2 * i + 1]); }
    /* limit to the clip (device space) */
    double cx0, cy0, cx1, cy1;
    cairo_save(cr); cairo_identity_matrix(cr); cairo_clip_extents(cr, &cx0, &cy0, &cx1, &cy1); cairo_restore(cr);
    bx0 = fmax(bx0, cx0); by0 = fmax(by0, cy0); bx1 = fmin(bx1, cx1); by1 = fmin(by1, cy1);
    int X0 = (int)floor(bx0), Y0 = (int)floor(by0), X1 = (int)ceil(bx1), Y1 = (int)ceil(by1);
    if (X1 <= X0 || Y1 <= Y0 || (long)(X1 - X0) * (Y1 - Y0) > 40000000L) return;
    int W = X1 - X0, Hh = Y1 - Y0;
    cairo_surface_t *dst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, Hh);
    uint32_t *dpx = (uint32_t *)cairo_image_surface_get_data(dst); int dstride = cairo_image_surface_get_stride(dst) / 4;
    for (int y = 0; y < Hh; y++) {
        double py = Y0 + y + 0.5;
        for (int x = 0; x < W; x++) {
            double px = X0 + x + 0.5;
            double w = Hi[6] * px + Hi[7] * py + Hi[8];
            if (fabs(w) < 1e-12) { dpx[y * dstride + x] = 0; continue; }
            double u = (Hi[0] * px + Hi[1] * py + Hi[2]) / w, v = (Hi[3] * px + Hi[4] * py + Hi[5]) / w;
            if (u < 0 || u > 1 || v < 0 || v > 1) { dpx[y * dstride + x] = 0; continue; }
            /* bilinear in source pixels (premultiplied ARGB) */
            double fx = u * sw - 0.5, fy = v * sh - 0.5;
            int ix = (int)floor(fx), iy = (int)floor(fy); double ax = fx - ix, ay = fy - iy;
            int x0 = ix < 0 ? 0 : ix >= sw ? sw - 1 : ix, x1 = ix + 1 < 0 ? 0 : ix + 1 >= sw ? sw - 1 : ix + 1;
            int y0 = iy < 0 ? 0 : iy >= sh ? sh - 1 : iy, y1 = iy + 1 < 0 ? 0 : iy + 1 >= sh ? sh - 1 : iy + 1;
            uint32_t p00 = spx[y0 * sstride + x0], p10 = spx[y0 * sstride + x1], p01 = spx[y1 * sstride + x0], p11 = spx[y1 * sstride + x1];
            uint32_t o = 0;
            for (int c = 0; c < 32; c += 8) {
                double v00 = (p00 >> c) & 255, v10 = (p10 >> c) & 255, v01 = (p01 >> c) & 255, v11 = (p11 >> c) & 255;
                double val = (v00 * (1 - ax) + v10 * ax) * (1 - ay) + (v01 * (1 - ax) + v11 * ax) * ay;
                o |= (uint32_t)lround(val) << c;
            }
            dpx[y * dstride + x] = o;
        }
    }
    cairo_surface_mark_dirty(dst);
    cairo_save(cr);
    cairo_identity_matrix(cr);
    cairo_set_source_surface(cr, dst, X0, Y0);
    if (alpha >= 0.999) cairo_paint(cr); else cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
    cairo_surface_destroy(dst);
}

/* separable box blur of an A8 buffer (zero outside). The vertical pass walks rows with a running sum per column, so
   it reads memory in order; the division by the window is a multiplication by its reciprocal. */
static void blur_a8(uint8_t *px, int w, int h, int stride, int r, int horizontal) {
    if (r < 1 || w <= 0 || h <= 0) return;
    long win = 2 * r + 1;
    uint64_t inv = ((1ull << 32) + win - 1) / win;       /* sum * inv >> 32 == sum / win for sum <= 255 * win */
    #define DIV(v) ((uint8_t)(((uint64_t)(v) * inv) >> 32))
    if (horizontal) {
        uint8_t *tmp = malloc((size_t)w);
        for (int y = 0; y < h; y++) {
            uint8_t *row = px + (size_t)y * stride;
            memcpy(tmp, row, (size_t)w);
            long sum = 0;
            for (int k = 0; k <= r && k < w; k++) sum += tmp[k];
            for (int i = 0; i < w; i++) {
                row[i] = DIV(sum);
                int add = i + r + 1, sub = i - r;
                if (add < w) sum += tmp[add];
                if (sub >= 0) sum -= tmp[sub];
            }
        }
        free(tmp);
    } else {
        uint8_t *src = malloc((size_t)w * h);
        uint32_t *sum = calloc((size_t)w, sizeof *sum);
        for (int y = 0; y < h; y++) memcpy(src + (size_t)y * w, px + (size_t)y * stride, (size_t)w);
        for (int k = 0; k <= r && k < h; k++) { const uint8_t *s0 = src + (size_t)k * w; for (int x = 0; x < w; x++) sum[x] += s0[x]; }
        for (int y = 0; y < h; y++) {
            uint8_t *row = px + (size_t)y * stride;
            for (int x = 0; x < w; x++) row[x] = DIV(sum[x]);
            int add = y + r + 1, sub = y - r;
            if (add < h) { const uint8_t *a = src + (size_t)add * w; for (int x = 0; x < w; x++) sum[x] += a[x]; }
            if (sub >= 0) { const uint8_t *b = src + (size_t)sub * w; for (int x = 0; x < w; x++) sum[x] -= b[x]; }
        }
        free(sum); free(src);
    }
    #undef DIV
}

static cairo_surface_t *blurred_alpha(const uint32_t *px, int stride, int w, int h, int pad, int r);
/* Pops the group pushed with isim_gfx_push_group (its alpha is the shadow shape) and paints a shadow of it:
   colour rgba, Gaussian-like blur of `radius` points (CALayer.shadowRadius), offset (dx, dy) in user space. */
ISIM_GUEST_ABI void isim_gfx_pop_group_shadow(const double *rgba, double radius, double dx, double dy) {
    cairo_t *cr = isim_host_cairo();
    cairo_pattern_t *pat = cairo_pop_group(cr);
    cairo_surface_t *s = NULL;
    if (cairo_pattern_get_surface(pat, &s) != CAIRO_STATUS_SUCCESS || cairo_surface_get_type(s) != CAIRO_SURFACE_TYPE_IMAGE) { cairo_pattern_destroy(pat); return; }
    cairo_surface_flush(s);
    double ox, oy; cairo_surface_get_device_offset(s, &ox, &oy);
    int sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s), ss = cairo_image_surface_get_stride(s) / 4;
    const uint32_t *spx = (const uint32_t *)cairo_image_surface_get_data(s);
    cairo_matrix_t m; cairo_get_matrix(cr, &m);
    double scale = sqrt(fabs(m.xx * m.yy - m.xy * m.yx)); if (scale <= 0) scale = 1;
    double rad = radius * scale;
    int r = (int)lround((sqrt(rad * rad + 1) - 1) / 2);   /* three box passes of width 2r+1 ~ a Gaussian with sigma = radius / 2 */
    int pad = 3 * r + 2;
    /* only the non-empty part of the group */
    int minx = sw, miny = sh, maxx = -1, maxy = -1;
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) if (spx[y * ss + x] >> 24) { if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; }
    if (maxx < 0) { cairo_pattern_destroy(pat); return; }
    cairo_surface_t *a8 = blurred_alpha(spx + (size_t)miny * ss + minx, ss, maxx - minx + 1, maxy - miny + 1, pad, r);
    double ddx = dx, ddy = dy; cairo_user_to_device_distance(cr, &ddx, &ddy);
    cairo_save(cr);
    cairo_identity_matrix(cr);
    cairo_set_source_rgba(cr, rgba[0], rgba[1], rgba[2], rgba[3]);
    cairo_mask_surface(cr, a8, minx - pad - ox + ddx, miny - pad - oy + ddy);
    cairo_restore(cr);
    cairo_surface_destroy(a8);
    cairo_pattern_destroy(pat);
}

/* A copy of what is on the drawing target under the rect (user space; between frames: the last frame shown),
   as an image handle (CATransition's "before" picture). */
int isim_image_from_surface(cairo_surface_t *src);
ISIM_GUEST_ABI int isim_gfx_screen_snapshot(double x, double y, double w, double h) {
    cairo_t *cr = isim_host_cairo();
    if (!cr || w <= 0 || h <= 0) return 0;
    cairo_surface_t *tgt = cairo_get_target(cr);
    if (cairo_surface_get_type(tgt) != CAIRO_SURFACE_TYPE_IMAGE) return 0;
    double x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    cairo_user_to_device(cr, &x0, &y0); cairo_user_to_device(cr, &x1, &y1);
    int W = (int)ceil(fabs(x1 - x0)), H = (int)ceil(fabs(y1 - y0));
    if (W <= 0 || H <= 0 || (long)W * H > 40000000L) return 0;
    cairo_surface_flush(tgt);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    cairo_t *c = cairo_create(s);
    cairo_set_source_surface(c, tgt, -fmin(x0, x1), -fmin(y0, y1));
    cairo_set_operator(c, CAIRO_OPERATOR_SOURCE);
    cairo_paint(c);
    cairo_destroy(c);
    int hd = isim_image_from_surface(s);
    cairo_surface_destroy(s);
    return hd;
}

/* Pops a group and paints it with each pixel multiplied by rgba (CAReplicatorLayer instance colours) and alpha. */
ISIM_GUEST_ABI void isim_gfx_pop_group_tinted(const double *rgba, double alpha) {
    cairo_t *cr = isim_host_cairo();
    cairo_pattern_t *pat = cairo_pop_group(cr);
    cairo_surface_t *s = NULL;
    if (cairo_pattern_get_surface(pat, &s) == CAIRO_STATUS_SUCCESS && cairo_surface_get_type(s) == CAIRO_SURFACE_TYPE_IMAGE) {
        cairo_surface_flush(s);
        int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s), st = cairo_image_surface_get_stride(s) / 4;
        uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
        double m[4] = { rgba[2] * rgba[3], rgba[1] * rgba[3], rgba[0] * rgba[3], rgba[3] };   /* B G R A in memory order */
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint32_t v = px[y * st + x], o = 0;
            if (!v) continue;
            for (int c = 0; c < 4; c++) { double k = fmin(1, fmax(0, m[c])); o |= (uint32_t)lround(((v >> (8 * c)) & 255) * k) << (8 * c); }
            px[y * st + x] = o;
        }
        cairo_surface_mark_dirty(s);
    }
    cairo_set_source(cr, pat);
    if (alpha >= 0.999) cairo_paint(cr); else cairo_paint_with_alpha(cr, alpha);
    cairo_pattern_destroy(pat);
}

/* separable box blur of premultiplied ARGB pixels (transparent outside the buffer, or the edge pixel repeated
   when `pad`) */
static void blur_argb(uint32_t *px, int w, int h, int stride, int r, int horizontal, int pad) {
    if (r < 1) return;
    int n = horizontal ? w : h, lines = horizontal ? h : w;
    uint32_t *tmp = malloc(sizeof *tmp * (size_t)n);
    long win = 2 * r + 1;
    for (int l = 0; l < lines; l++) {
        #define AT(i) (*(horizontal ? &px[(size_t)l * stride + (i)] : &px[(size_t)(i) * stride + l]))
        for (int i = 0; i < n; i++) tmp[i] = AT(i);
        #define SRC(k) ((k) < 0 ? (pad ? tmp[0] : 0u) : (k) >= n ? (pad ? tmp[n - 1] : 0u) : tmp[k])
        long sum[4] = { 0 };
        for (int k = -r; k <= r; k++) { uint32_t v = SRC(k); for (int c = 0; c < 4; c++) sum[c] += (long)((v >> (8 * c)) & 255); }
        for (int i = 0; i < n; i++) {
            uint32_t o = 0; for (int c = 0; c < 4; c++) o |= (uint32_t)(sum[c] / win) << (8 * c);
            AT(i) = o;
            uint32_t add = SRC(i + r + 1), sub = SRC(i - r);
            for (int c = 0; c < 4; c++) sum[c] += (long)((add >> (8 * c)) & 255) - (long)((sub >> (8 * c)) & 255);
        }
        #undef SRC
        #undef AT
    }
    free(tmp);
}

/* ---- blurs by content ----
   A shadow or a blur is three box passes each way over its whole area, every frame it is drawn. What it blurs is
   mostly the same from one frame to the next (a card's shadow while something else animates): results are kept by
   a hash of the pixels they were made from, so the same input is blurred once. A different input (a moved edge, an
   animating shape) hashes differently and is blurred anew. Least recently used results go past BLUR_KEEP or
   BLUR_BYTES. ISIM_BLUR_CACHE=0 turns it off. */
#define BLUR_KEEP 48
#define BLUR_BYTES (96u << 20)
struct blur_mem { uint64_t key; int w, h, r, kind; cairo_surface_t *a8; uint32_t *argb; size_t bytes; unsigned long last; };
static struct blur_mem blur_mem[BLUR_KEEP]; static unsigned long blur_clock; static size_t blur_total; static int blur_on = -1;
static int blur_cache_on(void) { if (blur_on < 0) { const char *e = getenv("ISIM_BLUR_CACHE"); blur_on = !(e && !strcmp(e, "0")); } return blur_on; }
static inline uint64_t blur_mix(uint64_t h, uint64_t v) { h ^= v; h *= 0x9e3779b97f4a7c15ull; return h ^ (h >> 31); }
/* the alpha bytes (kind 1) or the whole pixels (kind 2) of a region of premultiplied ARGB pixels */
static uint64_t blur_hash(const uint32_t *px, int stride, int w, int h, int kind, int r) {
    uint64_t hsh = blur_mix(blur_mix(blur_mix(0x51ed270b27f3a1c9ull, (uint64_t)w << 32 | (uint32_t)h), (uint64_t)r), (uint64_t)kind);
    for (int y = 0; y < h; y++) {
        const uint32_t *row = px + (size_t)y * stride;
        int x = 0;
        if (kind == 1) {
            for (; x + 4 <= w; x += 4) hsh = blur_mix(hsh, (uint64_t)(row[x] >> 24) | (uint64_t)(row[x + 1] >> 24) << 8 | (uint64_t)(row[x + 2] >> 24) << 16 | (uint64_t)(row[x + 3] >> 24) << 24);
            for (; x < w; x++) hsh = blur_mix(hsh, row[x] >> 24);
        } else {
            for (; x + 2 <= w; x += 2) hsh = blur_mix(hsh, (uint64_t)row[x] | (uint64_t)row[x + 1] << 32);
            for (; x < w; x++) hsh = blur_mix(hsh, row[x]);
        }
    }
    return hsh;
}
static struct blur_mem *blur_find(uint64_t key, int w, int h, int r, int kind) {
    for (int i = 0; i < BLUR_KEEP; i++) {
        struct blur_mem *e = &blur_mem[i];
        if (e->bytes && e->key == key && e->w == w && e->h == h && e->r == r && e->kind == kind) { e->last = ++blur_clock; return e; }
    }
    return NULL;
}
static void blur_forget(struct blur_mem *e) {
    if (e->a8) cairo_surface_destroy(e->a8);
    free(e->argb);
    blur_total -= e->bytes;
    memset(e, 0, sizeof *e);
}
/* a free entry for a result of `bytes`, making room among the least recently used ones */
static struct blur_mem *blur_slot(size_t bytes) {
    if (bytes > BLUR_BYTES / 4) return NULL;
    for (;;) {
        struct blur_mem *victim = NULL, *empty = NULL;
        for (int i = 0; i < BLUR_KEEP; i++) {
            struct blur_mem *e = &blur_mem[i];
            if (!e->bytes) { if (!empty) empty = e; continue; }
            if (!victim || e->last < victim->last) victim = e;
        }
        if (empty && blur_total + bytes <= BLUR_BYTES) { empty->bytes = bytes; empty->last = ++blur_clock; blur_total += bytes; return empty; }
        if (!victim) return NULL;
        blur_forget(victim);
    }
}
/* A blurred A8 mask of the alpha of a region (w x h at px) put `pad` pixels into a (w + 2 pad) x (h + 2 pad) mask,
   r: the box radius (three passes each way). Returns a new reference. */
static cairo_surface_t *blurred_alpha(const uint32_t *px, int stride, int w, int h, int pad, int r) {
    int W = w + 2 * pad, H = h + 2 * pad;
    uint64_t key = 0; int cached = blur_cache_on();
    if (cached) {
        key = blur_mix(blur_hash(px, stride, w, h, 1, r), (uint64_t)pad);
        struct blur_mem *e = blur_find(key, W, H, r, 1);
        if (e) return cairo_surface_reference(e->a8);
    }
    cairo_surface_t *a8 = cairo_image_surface_create(CAIRO_FORMAT_A8, W, H);
    uint8_t *apx = cairo_image_surface_get_data(a8); int as = cairo_image_surface_get_stride(a8);
    memset(apx, 0, (size_t)as * H);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) apx[(size_t)(y + pad) * as + x + pad] = px[(size_t)y * stride + x] >> 24;
    for (int pass = 0; pass < 3; pass++) { blur_a8(apx, W, H, as, r, 1); blur_a8(apx, W, H, as, r, 0); }
    cairo_surface_mark_dirty(a8);
    struct blur_mem *e = cached ? blur_slot((size_t)as * H) : NULL;
    if (e) { e->key = key; e->w = W; e->h = H; e->r = r; e->kind = 1; e->a8 = cairo_surface_reference(a8); }
    return a8;
}
/* blurs a region of premultiplied ARGB pixels in place (three box passes each way; edge: the edge pixels repeat) */
static void blur_argb_region(uint32_t *px, int stride, int w, int h, int r, int edge) {
    uint64_t key = 0; int cached = blur_cache_on();
    if (cached) {
        key = blur_mix(blur_hash(px, stride, w, h, 2, r), (uint64_t)edge);
        struct blur_mem *e = blur_find(key, w, h, r, 2);
        if (e) { for (int y = 0; y < h; y++) memcpy(px + (size_t)y * stride, e->argb + (size_t)y * w, sizeof *px * (size_t)w); return; }
    }
    for (int pass = 0; pass < 3; pass++) { blur_argb(px, w, h, stride, r, 1, edge); blur_argb(px, w, h, stride, r, 0, edge); }
    struct blur_mem *e = cached ? blur_slot(sizeof *px * (size_t)w * h) : NULL;
    if (e && (e->argb = malloc(e->bytes))) {
        e->key = key; e->w = w; e->h = h; e->r = r; e->kind = 2;
        for (int y = 0; y < h; y++) memcpy(e->argb + (size_t)y * w, px + (size_t)y * stride, sizeof *px * (size_t)w);
    } else if (e) blur_forget(e);
}

/* a guest pixel shader over a group pushed with isim_gfx_push_group (SwiftUI's colorEffect, layerEffect and
   distortionEffect, swift/overlays/SwiftUI): pops the group, calls fn with the premultiplied ARGB pixels of `rect`
   (user space) widened by `margin` points and clipped to the surface (x0, y0: the user-space offset of the first
   pixel's corner from the rect's origin; scale: pixels per point), then paints the group with alpha */
typedef void (ISIM_GUEST_CALLBACK *isim_shade_fn)(void *ctx, uint32_t *px, int w, int h, int stride, double x0, double y0, double scale);
ISIM_GUEST_ABI void isim_gfx_pop_group_shaded(isim_shade_fn fn, void *ctx, double margin, double alpha, double x, double y, double w, double h);
ISIM_GUEST_ABI void isim_gfx_pop_group_shaded(isim_shade_fn fn, void *ctx, double margin, double alpha, double x, double y, double w, double h) {
    cairo_t *cr = isim_host_cairo();
    cairo_pattern_t *pat = cairo_pop_group(cr);
    cairo_surface_t *s = NULL;
    if (fn && cairo_pattern_get_surface(pat, &s) == CAIRO_STATUS_SUCCESS && cairo_surface_get_type(s) == CAIRO_SURFACE_TYPE_IMAGE) {
        cairo_surface_flush(s);
        double ox, oy; cairo_surface_get_device_offset(s, &ox, &oy);
        int sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s), ss = cairo_image_surface_get_stride(s) / 4;
        uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
        cairo_matrix_t m; cairo_get_matrix(cr, &m);
        double scale = sqrt(fabs(m.xx * m.yy - m.xy * m.yx)); if (scale <= 0) scale = 1;
        double xs[4] = { x - margin, x + w + margin, x - margin, x + w + margin }, ys[4] = { y - margin, y - margin, y + h + margin, y + h + margin };
        double bx0 = 1e18, by0 = 1e18, bx1 = -1e18, by1 = -1e18;
        for (int i = 0; i < 4; i++) { cairo_user_to_device(cr, &xs[i], &ys[i]); bx0 = fmin(bx0, xs[i]); by0 = fmin(by0, ys[i]); bx1 = fmax(bx1, xs[i]); by1 = fmax(by1, ys[i]); }
        int X0 = (int)floor(bx0 + ox), Y0 = (int)floor(by0 + oy), X1 = (int)ceil(bx1 + ox), Y1 = (int)ceil(by1 + oy);
        if (X0 < 0) X0 = 0;
        if (Y0 < 0) Y0 = 0;
        if (X1 > sw) X1 = sw;
        if (Y1 > sh) Y1 = sh;
        if (X1 > X0 && Y1 > Y0) {
            double ux = X0 - ox, uy = Y0 - oy;
            cairo_device_to_user(cr, &ux, &uy);
            fn(ctx, px + (size_t)Y0 * ss + X0, X1 - X0, Y1 - Y0, ss, ux - x, uy - y, scale);
            cairo_surface_mark_dirty(s);
        }
    }
    cairo_save(cr);
    cairo_set_source(cr, pat);
    if (alpha >= 0.999) cairo_paint(cr); else cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
    cairo_pattern_destroy(pat);
}

/* SwiftUI-style visual effects on a group pushed with isim_gfx_push_group (the content of one view):
   v[0..19]  4x5 colour matrix, rows R G B A, columns r g b a bias, on unpremultiplied 0..1 values (v[29] flag 1)
   v[20]     blur radius in points (Gaussian-like: three box passes); v[29] flag 4 = opaque (edges repeat, no fade)
   v[21]     blend mode: 0 normal, 1 multiply, 2 screen, 3 overlay, 4 darken, 5 lighten, 6 colour dodge, 7 colour
             burn, 8 soft light, 9 hard light, 10 difference, 11 exclusion, 12 hue, 13 saturation, 14 colour,
             15 luminosity, 16 source atop, 17 destination over, 18 destination out, 19 plus darker, 20 plus lighter
   v[22..25] shadow colour (rgba; alpha 0 = no shadow), v[26] shadow radius, v[27] v[28] shadow offset (points)
   The effects apply inside `rect` (the view's bounds, user space) widened by what the blur and shadow need, then
   the group is painted with `alpha` and the blend mode. */
ISIM_GUEST_ABI void isim_gfx_pop_group_filtered(const double *v, double alpha, double x, double y, double w, double h) {
    cairo_t *cr = isim_host_cairo();
    cairo_pattern_t *pat = cairo_pop_group(cr);
    cairo_surface_t *s = NULL;
    static const cairo_operator_t ops[] = { CAIRO_OPERATOR_OVER, CAIRO_OPERATOR_MULTIPLY, CAIRO_OPERATOR_SCREEN, CAIRO_OPERATOR_OVERLAY,
        CAIRO_OPERATOR_DARKEN, CAIRO_OPERATOR_LIGHTEN, CAIRO_OPERATOR_COLOR_DODGE, CAIRO_OPERATOR_COLOR_BURN, CAIRO_OPERATOR_SOFT_LIGHT,
        CAIRO_OPERATOR_HARD_LIGHT, CAIRO_OPERATOR_DIFFERENCE, CAIRO_OPERATOR_EXCLUSION, CAIRO_OPERATOR_HSL_HUE, CAIRO_OPERATOR_HSL_SATURATION,
        CAIRO_OPERATOR_HSL_COLOR, CAIRO_OPERATOR_HSL_LUMINOSITY, CAIRO_OPERATOR_ATOP, CAIRO_OPERATOR_DEST_OVER, CAIRO_OPERATOR_DEST_OUT,
        CAIRO_OPERATOR_MULTIPLY, CAIRO_OPERATOR_ADD };
    int blend = v ? (int)v[21] : 0;
    if (blend < 0 || blend > 20) blend = 0;
    int flags = v ? (int)v[29] : 0;
    double shadowA = v ? v[25] : 0;
    if (v && cairo_pattern_get_surface(pat, &s) == CAIRO_STATUS_SUCCESS && cairo_surface_get_type(s) == CAIRO_SURFACE_TYPE_IMAGE) {
        cairo_surface_flush(s);
        double ox, oy; cairo_surface_get_device_offset(s, &ox, &oy);
        int sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s), ss = cairo_image_surface_get_stride(s) / 4;
        uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
        cairo_matrix_t m; cairo_get_matrix(cr, &m);
        double scale = sqrt(fabs(m.xx * m.yy - m.xy * m.yx)); if (scale <= 0) scale = 1;
        double blur = v[20] > 0 ? v[20] * scale : 0, srad = shadowA > 0 ? v[26] * scale : 0;
        double margin = 3 * blur + 3 * srad + (shadowA > 0 ? (fabs(v[27]) + fabs(v[28])) * scale : 0) + 2;
        /* the view's rect in surface pixels */
        double xs[4] = { x, x + w, x, x + w }, ys[4] = { y, y, y + h, y + h }, bx0 = 1e18, by0 = 1e18, bx1 = -1e18, by1 = -1e18;
        for (int i = 0; i < 4; i++) { cairo_user_to_device(cr, &xs[i], &ys[i]); bx0 = fmin(bx0, xs[i]); by0 = fmin(by0, ys[i]); bx1 = fmax(bx1, xs[i]); by1 = fmax(by1, ys[i]); }
        int vx0 = (int)floor(bx0 + ox), vy0 = (int)floor(by0 + oy), vx1 = (int)ceil(bx1 + ox), vy1 = (int)ceil(by1 + oy);
        int X0 = (int)floor(bx0 + ox - margin), Y0 = (int)floor(by0 + oy - margin), X1 = (int)ceil(bx1 + ox + margin), Y1 = (int)ceil(by1 + oy + margin);
        if (X0 < 0) X0 = 0;
        if (Y0 < 0) Y0 = 0;
        if (X1 > sw) X1 = sw;
        if (Y1 > sh) Y1 = sh;
        if (X1 > X0 && Y1 > Y0) {
            int W = X1 - X0, H = Y1 - Y0;
            if (flags & 1) {                           /* colour matrix */
                for (int yy = Y0; yy < Y1; yy++) for (int xx = X0; xx < X1; xx++) {
                    uint32_t p = px[yy * ss + xx];
                    double a = (p >> 24) / 255.0, r = 0, g = 0, b = 0;
                    if (a > 0) { r = ((p >> 16) & 255) / 255.0 / a; g = ((p >> 8) & 255) / 255.0 / a; b = (p & 255) / 255.0 / a; }
                    double o[4];
                    for (int k = 0; k < 4; k++) {
                        const double *row = v + 5 * k;
                        double val = row[0] * r + row[1] * g + row[2] * b + row[3] * a + row[4];
                        o[k] = val < 0 ? 0 : val > 1 ? 1 : val;
                    }
                    uint32_t A = (uint32_t)lround(o[3] * 255);
                    px[yy * ss + xx] = A << 24 | (uint32_t)lround(o[0] * o[3] * 255) << 16 | (uint32_t)lround(o[1] * o[3] * 255) << 8 | (uint32_t)lround(o[2] * o[3] * 255);
                }
            }
            if (blur > 0) {                            /* three box passes ~ a Gaussian with sigma = radius */
                int r = (int)lround(blur * 0.9);
                if (flags & 4) {                       /* opaque: inside the view only, edges repeated */
                    int ax0 = vx0 < 0 ? 0 : vx0, ay0 = vy0 < 0 ? 0 : vy0, ax1 = vx1 > sw ? sw : vx1, ay1 = vy1 > sh ? sh : vy1;
                    if (ax1 > ax0 && ay1 > ay0) blur_argb_region(px + ay0 * ss + ax0, ss, ax1 - ax0, ay1 - ay0, r, 1);
                } else blur_argb_region(px + Y0 * ss + X0, ss, W, H, r, 0);
            }
            cairo_surface_mark_dirty(s);
            if (shadowA > 0) {                         /* drop shadow of the content's alpha, under it */
                int r = (int)lround((sqrt(srad * srad + 1) - 1) / 2);
                cairo_surface_t *a8 = blurred_alpha(px + (size_t)Y0 * ss + X0, ss, W, H, 0, r);
                double ddx = v[27], ddy = v[28]; cairo_user_to_device_distance(cr, &ddx, &ddy);
                cairo_save(cr);
                cairo_identity_matrix(cr);
                cairo_set_operator(cr, ops[blend]);
                cairo_set_source_rgba(cr, v[22], v[23], v[24], v[25] * alpha);
                cairo_mask_surface(cr, a8, X0 - ox + ddx, Y0 - oy + ddy);
                cairo_restore(cr);
                cairo_surface_destroy(a8);
            }
        }
    }
    cairo_save(cr);
    cairo_set_operator(cr, ops[blend]);
    cairo_set_source(cr, pat);
    if (alpha >= 0.999) cairo_paint(cr); else cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
    cairo_pattern_destroy(pat);
}
