/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <M5GFX.h>

/**
 * @brief In-place "selective-snap" dithering of an M5Canvas to the 6 EPD inks.
 *
 * Pixels whose (error-adjusted) color lies within SNAP_THRESHOLD (linear-light
 * distance) of a pure ink are snapped flat with no error diffusion, keeping
 * large near-pure regions clean and grain-free. Only genuinely intermediate
 * colors get serpentine Floyd-Steinberg error diffusion. Every output pixel is
 * written as exactly one of the 6 panel inks, so the canvas can then be pushed
 * in epd_fastest for 1:1 reproduction (no second M5GFX dither on top).
 *
 * Requires the canvas color depth to be 24bpp (rgb888/bgr888). No-op otherwise.
 */
void epd_dither_selective(M5Canvas* canvas);
