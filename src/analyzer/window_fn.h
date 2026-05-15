/*
 * audio-visualizer - A real-time audio visualizer.
 * Copyright (C) 2026  Charles Thompson
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef WINDOW_FN_H
#define WINDOW_FN_H

#include <stdint.h>

typedef enum WindowFunction
{
  WINDOW_RECTANGULAR = 0,
  WINDOW_HANN,
  WINDOW_HAMMING,
  WINDOW_BLACKMAN
} WindowFunction;

// Fills `w[0..n-1]` with the named window. `n` must be >= 2 for symmetric
// windows; n==1 just yields w[0]=1.
void window_fn_fill(float *w, uint32_t n, WindowFunction kind);

// Coherent gain (sum(w)/n) of the named window for amplitude calibration.
float window_fn_coherent_gain(WindowFunction kind);

const char *window_fn_name(WindowFunction kind);

#endif
