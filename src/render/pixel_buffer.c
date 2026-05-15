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

#include "pixel_buffer.h"

#include <stdlib.h>
#include <string.h>

bool pixel_buffer_init(PixelBuffer *buf, int width, int height)
{
  if (!buf || width <= 0 || height <= 0)
    return false;
  buf->pixels = (uint32_t *)calloc((size_t)width * (size_t)height, sizeof(uint32_t));
  if (!buf->pixels)
    return false;
  buf->width = width;
  buf->height = height;
  buf->stride = width * (int)sizeof(uint32_t);
  return true;
}

void pixel_buffer_destroy(PixelBuffer *buf)
{
  if (!buf)
    return;
  free(buf->pixels);
  buf->pixels = NULL;
  buf->width = 0;
  buf->height = 0;
  buf->stride = 0;
}

void pixel_buffer_clear(PixelBuffer *buf, uint32_t argb)
{
  if (!buf || !buf->pixels)
    return;
  const size_t count = (size_t)buf->width * (size_t)buf->height;
  if (argb == 0)
  {
    memset(buf->pixels, 0, count * sizeof(uint32_t));
    return;
  }
  for (size_t i = 0; i < count; ++i)
  {
    buf->pixels[i] = argb;
  }
}
