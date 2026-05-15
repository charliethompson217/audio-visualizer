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

#include "color.h"

#include <math.h>

uint32_t hsv_to_argb(float h, float s, float v)
{
  h = fmodf(h, 360.0f);
  if (h < 0.0f)
    h += 360.0f;
  if (s < 0.0f)
    s = 0.0f;
  else if (s > 1.0f)
    s = 1.0f;
  if (v < 0.0f)
    v = 0.0f;
  else if (v > 1.0f)
    v = 1.0f;

  const float c = v * s;
  const float hp = h / 60.0f;
  const float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
  float r = 0.0f, g = 0.0f, b = 0.0f;
  if (hp < 1.0f)
  {
    r = c;
    g = x;
  }
  else if (hp < 2.0f)
  {
    r = x;
    g = c;
  }
  else if (hp < 3.0f)
  {
    g = c;
    b = x;
  }
  else if (hp < 4.0f)
  {
    g = x;
    b = c;
  }
  else if (hp < 5.0f)
  {
    r = x;
    b = c;
  }
  else
  {
    r = c;
    b = x;
  }
  const float m = v - c;
  const uint32_t A = (uint32_t)(v * 255.0f + 0.5f);
  const uint32_t R = (uint32_t)((r + m) * 255.0f + 0.5f);
  const uint32_t G = (uint32_t)((g + m) * 255.0f + 0.5f);
  const uint32_t B = (uint32_t)((b + m) * 255.0f + 0.5f);
  return (A << 24) | (R << 16) | (G << 8) | B;
}
