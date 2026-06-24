/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "epd_dither.h"

#include <M5GFX.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "EpdDither";

// ---- Approach (v3, matches the web "smooth" default) ---------------------
// Serpentine Floyd-Steinberg error diffusion in LINEAR light onto the 6 panel
// inks, with PURE-INK PROTECTION matching the browser "smooth" mode's default:
// near ANY of the 6 pure inks we suppress error diffusion so the region stays a
// clean flat ink instead of being speckled; mid-tones get full diffusion for
// smooth gradients. Targets "less grain than native, especially in near-pure
// areas, while staying smooth overall".
//
// Tunables (web defaults): PICK_RANGE = linear-distance radius around each ink
// inside which dithering fades out; PICK_STRENGTH = how strongly (0..1) it is
// suppressed at the ink. The fade is a smoothstep, so there is no hard seam.
static constexpr float PICK_RANGE    = 0.25f;
static constexpr float PICK_STRENGTH = 0.85f;

// 6 measured panel inks (sRGB), matching Panel_ED2208 epd_palette.
static const uint8_t INK_SRGB[6][3] = {
    {0, 0, 0},        // BLACK
    {255, 255, 255},  // WHITE
    {255, 243, 56},   // YELLOW
    {191, 0, 0},      // RED
    {100, 64, 255},   // BLUE
    {67, 138, 28},    // GREEN
};

static float s_srgb2lin[256];
static float s_pal_lin[6][3];
static bool s_lut_ready = false;

static void ensure_lut()
{
    if (s_lut_ready) return;
    for (int i = 0; i < 256; ++i) {
        float c       = i / 255.0f;
        s_srgb2lin[i] = (c <= 0.04045f) ? (c / 12.92f) : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    for (int p = 0; p < 6; ++p) {
        for (int k = 0; k < 3; ++k) s_pal_lin[p][k] = s_srgb2lin[INK_SRGB[p][k]];
    }
    s_lut_ready = true;
}

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// 0 at x<=edge0, 1 at x>=edge1, smooth in between.
static inline float smoothstep(float edge0, float edge1, float x)
{
    if (edge1 <= edge0) return x >= edge1 ? 1.0f : 0.0f;
    float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

void epd_dither_selective(M5Canvas *canvas)
{
    if (!canvas) return;
    if (canvas->getColorDepth() != lgfx::color_depth_t::rgb888_3Byte) {
        ESP_LOGW(TAG, "canvas not 24bpp, skipping dither");
        return;
    }
    const int w = canvas->width();
    const int h = canvas->height();
    auto *buf   = static_cast<lgfx::bgr888_t *>(canvas->getBuffer());
    if (!buf || w <= 0 || h <= 0) return;

    ensure_lut();

    const size_t row_floats = (size_t)w * 3;
    float *err_curr         = (float *)calloc(row_floats, sizeof(float));
    float *err_next         = (float *)calloc(row_floats, sizeof(float));
    if (!err_curr || !err_next) {
        free(err_curr);
        free(err_next);
        ESP_LOGE(TAG, "error-buffer alloc failed");
        return;
    }

    uint32_t protectedpx = 0;

    for (int y = 0; y < h; ++y) {
        memset(err_next, 0, row_floats * sizeof(float));
        const bool l2r    = ((y & 1) == 0);
        const int x_start = l2r ? 0 : (w - 1);
        const int x_end   = l2r ? w : -1;
        const int fwd     = l2r ? 1 : -1;

        for (int x = x_start; x != x_end; x += fwd) {
            lgfx::bgr888_t &px = buf[x + y * w];
            const int xi3      = x * 3;
            float work[3]      = {
                clamp01(s_srgb2lin[px.r] + err_curr[xi3 + 0]),
                clamp01(s_srgb2lin[px.g] + err_curr[xi3 + 1]),
                clamp01(s_srgb2lin[px.b] + err_curr[xi3 + 2]),
            };

            // Nearest ink (linear Euclidean) AND pure-ink protection, one pass.
            // Protection = strongest (nearest) ink's fade, matching the web
            // "smooth" default which protects all 6 inks.
            int best     = 0;
            float best_d = 1e30f;
            float pm     = 0.0f;  // max over inks of (1 - smoothstep(0, PICK_RANGE, dist))
            for (int p = 0; p < 6; ++p) {
                float dr  = work[0] - s_pal_lin[p][0];
                float dg  = work[1] - s_pal_lin[p][1];
                float dbb = work[2] - s_pal_lin[p][2];
                float d2  = dr * dr + dg * dg + dbb * dbb;
                if (d2 < best_d) {
                    best_d = d2;
                    best   = p;
                }
                float m = 1.0f - smoothstep(0.0f, PICK_RANGE, sqrtf(d2));
                if (m > pm) pm = m;
            }

            const float res[3] = {
                work[0] - s_pal_lin[best][0],
                work[1] - s_pal_lin[best][1],
                work[2] - s_pal_lin[best][2],
            };

            const float factor = 1.0f - PICK_STRENGTH * pm;  // in [1-PICK_STRENGTH, 1]
            if (factor < 0.5f) ++protectedpx;

            // Output is always an exact ink (near-white -> white, etc.).
            px.r = INK_SRGB[best][0];
            px.g = INK_SRGB[best][1];
            px.b = INK_SRGB[best][2];

            if (factor > 0.0f) {
                const float er = res[0] * factor;
                const float eg = res[1] * factor;
                const float eb = res[2] * factor;
                const int xf   = x + fwd;
                const int xb   = x - fwd;
                if (xf >= 0 && xf < w) {
                    err_curr[xf * 3 + 0] += er * (7.0f / 16.0f);
                    err_curr[xf * 3 + 1] += eg * (7.0f / 16.0f);
                    err_curr[xf * 3 + 2] += eb * (7.0f / 16.0f);
                }
                if (xb >= 0 && xb < w) {
                    err_next[xb * 3 + 0] += er * (3.0f / 16.0f);
                    err_next[xb * 3 + 1] += eg * (3.0f / 16.0f);
                    err_next[xb * 3 + 2] += eb * (3.0f / 16.0f);
                }
                err_next[xi3 + 0] += er * (5.0f / 16.0f);
                err_next[xi3 + 1] += eg * (5.0f / 16.0f);
                err_next[xi3 + 2] += eb * (5.0f / 16.0f);
                if (xf >= 0 && xf < w) {
                    err_next[xf * 3 + 0] += er * (1.0f / 16.0f);
                    err_next[xf * 3 + 1] += eg * (1.0f / 16.0f);
                    err_next[xf * 3 + 2] += eb * (1.0f / 16.0f);
                }
            }
        }

        float *tmp = err_curr;
        err_curr   = err_next;
        err_next   = tmp;
    }

    free(err_curr);
    free(err_next);

    const uint32_t total = (uint32_t)w * (uint32_t)h;
    ESP_LOGI(TAG, "dither v3 %dx%d ink-protected %.1f%% (range=%.2f strength=%.2f)", w, h,
             total ? (100.0f * (float)protectedpx / (float)total) : 0.0f, (double)PICK_RANGE, (double)PICK_STRENGTH);
}
