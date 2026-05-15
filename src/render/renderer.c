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

#include "renderer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "color.h"

VisualizerConfig visualizer_config_defaults(void)
{
  VisualizerConfig v = {
      .brightness_power = 1.2f,
      .length_power = 1.0f,
      .min_semitone = 12.0f,
      .max_semitone = 108.0f,
      .show_labels = false,
      .show_cursor = false,
      .show_spectrum = true,
      .show_waveform = false,
      .waveform_gain = 1.0f,
      .waveform_samples_per_px = 1.0f,
      .f0 = 8.1758f,
      .note_hues = {0, 25, 45, 75, 110, 166, 190, 210, 240, 270, 300, 330},
      .bar_width_px = 1,
  };
  return v;
}

bool renderer_init(Renderer *r, int width)
{
  if (!r || width <= 0)
    return false;
  r->column_max_energy = (float *)calloc((size_t)width, sizeof(float));
  r->width = width;
  return r->column_max_energy != NULL;
}

void renderer_destroy(Renderer *r)
{
  if (!r)
    return;
  free(r->column_max_energy);
  r->column_max_energy = NULL;
  r->width = 0;
}

bool renderer_resize(Renderer *r, int width)
{
  if (!r || width <= 0)
    return false;
  if (width == r->width)
    return true;
  renderer_destroy(r);
  return renderer_init(r, width);
}

void renderer_draw_spectrum(Renderer *r, PixelBuffer *buf, const AnalyzerOutput *output,
                            const AnalyzerConfig *acfg, const VisualizerConfig *vcfg)
{
  if (!r || !buf || !output || !acfg || !vcfg)
    return;
  if (buf->width != r->width && !renderer_resize(r, buf->width))
    return;

  const int w = buf->width;
  const int h = buf->height;
  const int mid = h / 2;
  uint32_t *px = buf->pixels;

  // 1. Reset per-column loudest-bin tracker. The caller is responsible for
  //    clearing `buf` first; this lets a waveform view be composited into
  //    the same frame without us wiping its pixels.
  for (int i = 0; i < w; ++i)
    r->column_max_energy[i] = 0.0f;

  // 2. Draw one centered line per bin, mapped to log-semitone x.
  //    When multiple bins map to the same column, the loudest one wins.
  const float sr = (float)acfg->sample_rate;
  const float fft_n = (float)acfg->fft_size;
  const float f0 = vcfg->f0 > 0.0f ? vcfg->f0 : 8.1758f;
  const float min_st = vcfg->min_semitone;
  const float max_st = vcfg->max_semitone;
  const float st_range = (max_st > min_st) ? (max_st - min_st) : 1.0f;
  const float len_p = vcfg->length_power;
  const float bright_p = vcfg->brightness_power;
  const int bar_w = vcfg->bar_width_px > 0 ? vcfg->bar_width_px : 1;
  // Position is quantized to a display-pixel grid (render-width / bar_w)
  // and then expanded into the corresponding `bar_w` render columns.
  const int display_w = w / bar_w;
  if (display_w <= 0)
    return;
  // Range over which the bin density crosses ~1 bin / display-pixel, the
  // floor() quantization makes consecutive bins land at columns that
  // differ by 1 or 2 unpredictably, leaving sporadic 1-px-wide gaps.
  // p5's anti-aliased stroke bridges those single-pixel gaps via partial
  // coverage; we don't have AA, so each bar is forward-filled up to the
  // next bin's column, capped so the discrete-bar look at low frequencies
  // (where bin spacing is several display pixels) is preserved.
  const int MAX_FILL_DISPLAY = 2;
  const float st_per_bin_scale = 12.0f / logf(2.0f);

  for (uint32_t bin = 1; bin < output->bin_count; ++bin)
  {
    // One bar per bin. Loudest bin wins when
    // multiple bins map to the same display pixel.
    const float freq = (float)bin * sr / fft_n;
    if (freq <= 0.0f)
      continue;
    const float semitone = 12.0f * log2f(freq / f0);
    if (semitone < min_st || semitone > max_st)
      continue;

    const float x_norm = (semitone - min_st) / st_range;
    const int x_display = (int)(x_norm * (float)display_w);
    if (x_display < 0 || x_display >= display_w)
      continue;

    // Forward gap to the next bin in display-pixel units. log2f((b+1)/b)
    // collapses to the closed-form below and avoids a second log per bin.
    int fill = 1;
    if (bin + 1 < output->bin_count)
    {
      const float next_st = semitone + st_per_bin_scale / (float)bin;
      const float next_x_norm = (next_st - min_st) / st_range;
      const int next_x_display = (int)(next_x_norm * (float)display_w);
      fill = next_x_display - x_display;
      if (fill < 1)
        fill = 1;
      if (fill > MAX_FILL_DISPLAY)
        fill = MAX_FILL_DISPLAY;
    }

    const float e = output->smoothed_energy[bin];

    const int closest = (int)floorf(semitone + 0.5f);
    const int idx = ((closest % 12) + 12) % 12;
    const float hue = vcfg->note_hues[idx];

    const float len_e = powf(e, len_p);
    const float bright_e = powf(e, bright_p);
    const int half = (int)(len_e * (float)mid);
    int y0 = mid - half;
    int y1 = mid + half;
    if (y0 < 0)
      y0 = 0;
    if (y1 >= h)
      y1 = h - 1;

    const uint32_t color = hsv_to_argb(hue, 1.0f, bright_e);

    // Per-display-column loudest-wins so an extended bar's tail can be
    // overwritten by a louder neighbor that lands inside it.
    for (int dx = 0; dx < fill; ++dx)
    {
      const int xd = x_display + dx;
      if (xd >= display_w)
        break;
      const int x_left = xd * bar_w;
      int x_right = x_left + bar_w - 1;
      if (x_right >= w)
        x_right = w - 1;
      if (e <= r->column_max_energy[x_left])
        continue;
      for (int x = x_left; x <= x_right; ++x)
        r->column_max_energy[x] = e;
      for (int y = y0; y <= y1; ++y)
      {
        for (int x = x_left; x <= x_right; ++x)
          px[(size_t)y * (size_t)w + (size_t)x] = color;
      }
    }
  }
}

void renderer_draw_waveform(PixelBuffer *buf, const float *samples, int sample_count,
                            const VisualizerConfig *vcfg)
{
  if (!buf || !samples || sample_count <= 1 || !vcfg)
    return;

  const int w = buf->width;
  const int h = buf->height;
  if (w <= 1 || h <= 0)
    return;
  const int mid = h / 2;
  uint32_t *px = buf->pixels;

  // Vertical line thickness in render-pixels. Sized to the bar width so a
  // 1-display-pixel-thick line survives the bilinear downsample, matching
  // the spectrum's stroke-weight semantics.
  const int bar_w = vcfg->bar_width_px > 0 ? vcfg->bar_width_px : 1;
  const int half_thick = bar_w / 2;
  const uint32_t color = 0xFFFFFFFFu;

  // Amplitudes in [-1, 1] map to y in [thickness, h-1-thickness], so a
  // full-scale sample lands one stroke-width inside the canvas edges.
  const int amp_px = mid - bar_w;
  if (amp_px <= 0)
    return;

  // Horizontal zoom is expressed directly in samples-per-display-pixel,
  // so 1.0 displays exactly one sample per pixel (the "pure data" view).
  // The display-pixel width is recovered from buf->width / bar_w because
  // the framebuffer is supersampled by `bar_w` (= RENDER_SCALE).
  const int display_w = w / bar_w;
  const float sp_per_px =
      vcfg->waveform_samples_per_px > 0.0f ? vcfg->waveform_samples_per_px : 1.0f;
  int window_n = (int)((float)display_w * sp_per_px + 0.5f);
  if (window_n < 2)
    window_n = 2;
  if (window_n > sample_count)
    window_n = sample_count;
  const int window_start = sample_count - window_n;

  // Vertical gain: applied before clamping so quiet signals can be
  // amplified; clipping at ±1 keeps loud peaks pinned to the canvas edges
  // rather than wrapping around.
  const float gain = vcfg->waveform_gain > 0.0f ? vcfg->waveform_gain : 1.0f;

  int prev_y = -1;
  // Iterate over DISPLAY pixels (not render pixels) so that each sample's
  // connecting span fills all bar_w render columns for that pixel.  When the
  // framebuffer is supersampled (bar_w = RENDER_SCALE > 1) the connecting
  // span is bar_w render pixels wide and is guaranteed to be captured by
  // SDL's bilinear downsample regardless of where the sample boundary lands
  // within the render block.  Iterating over render pixels produced a 1-px
  // wide span that was only captured when it happened to land on the center
  // render column of the block (~right 25 % of the display), making that
  // region appear thicker than the left portion.
  for (int d = 0; d < display_w; ++d)
  {
    // Oldest sample on the left, newest on the right.
    const float t = (display_w > 1) ? (float)d / (float)(display_w - 1) : 0.0f;
    int idx = window_start + (int)(t * (float)(window_n - 1) + 0.5f);
    if (idx < 0)
      idx = 0;
    if (idx >= sample_count)
      idx = sample_count - 1;
    float s = samples[idx] * gain;
    if (s > 1.0f)
      s = 1.0f;
    if (s < -1.0f)
      s = -1.0f;
    const int y = mid - (int)(s * (float)amp_px);

    int y0 = y;
    int y1 = y;
    if (prev_y >= 0)
    {
      if (prev_y < y0)
        y0 = prev_y;
      if (prev_y > y1)
        y1 = prev_y;
    }
    y0 -= half_thick;
    y1 += half_thick;
    if (y0 < 0)
      y0 = 0;
    if (y1 >= h)
      y1 = h - 1;

    // Fill all bar_w render columns for this display pixel.
    const int rx_start = d * bar_w;
    const int rx_end = rx_start + bar_w;
    for (int yy = y0; yy <= y1; ++yy)
      for (int rx = rx_start; rx < rx_end; ++rx)
        px[(size_t)yy * (size_t)w + (size_t)rx] = color;
    prev_y = y;
  }
}
