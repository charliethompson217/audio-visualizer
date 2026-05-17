/*
 * audiovisualizer - A real-time audio visualizer.
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

#include "window_fn.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void window_fn_fill(float *w, uint32_t n, WindowFunction kind)
{
  if (!w || n == 0)
    return;
  if (n == 1)
  {
    w[0] = 1.0f;
    return;
  }

  const double two_pi = 2.0 * M_PI;
  const double four_pi = 4.0 * M_PI;
  const double denom = (double)(n - 1);

  switch (kind)
  {
  case WINDOW_RECTANGULAR:
    for (uint32_t i = 0; i < n; ++i)
      w[i] = 1.0f;
    break;
  case WINDOW_HANN:
    for (uint32_t i = 0; i < n; ++i)
    {
      w[i] = (float)(0.5 * (1.0 - cos(two_pi * (double)i / denom)));
    }
    break;
  case WINDOW_HAMMING:
    for (uint32_t i = 0; i < n; ++i)
    {
      w[i] = (float)(0.54 - 0.46 * cos(two_pi * (double)i / denom));
    }
    break;
  case WINDOW_BLACKMAN:
    for (uint32_t i = 0; i < n; ++i)
    {
      const double x = (double)i / denom;
      w[i] = (float)(0.42 - 0.50 * cos(two_pi * x) + 0.08 * cos(four_pi * x));
    }
    break;
  default:
    for (uint32_t i = 0; i < n; ++i)
      w[i] = 1.0f;
    break;
  }
}

float window_fn_coherent_gain(WindowFunction kind)
{
  switch (kind)
  {
  case WINDOW_RECTANGULAR:
    return 1.00f;
  case WINDOW_HANN:
    return 0.50f;
  case WINDOW_HAMMING:
    return 0.54f;
  case WINDOW_BLACKMAN:
    return 0.42f;
  default:
    return 1.00f;
  }
}

const char *window_fn_name(WindowFunction kind)
{
  switch (kind)
  {
  case WINDOW_RECTANGULAR:
    return "rectangular";
  case WINDOW_HANN:
    return "hann";
  case WINDOW_HAMMING:
    return "hamming";
  case WINDOW_BLACKMAN:
    return "blackman";
  default:
    return "?";
  }
}
