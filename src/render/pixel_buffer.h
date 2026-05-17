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

#ifndef PIXEL_BUFFER_H
#define PIXEL_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

// Software pixel framebuffer.
// Packed 0xAARRGGBB per pixel (ARGB8888 on little-endian).
// `stride` is in bytes per row and equals width * sizeof(uint32_t).
typedef struct PixelBuffer
{
  uint32_t *pixels;
  int width;
  int height;
  int stride;
} PixelBuffer;

bool pixel_buffer_init(PixelBuffer *buf, int width, int height);
void pixel_buffer_destroy(PixelBuffer *buf);

void pixel_buffer_clear(PixelBuffer *buf, uint32_t argb);

#endif
