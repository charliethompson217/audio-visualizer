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

#ifndef RENDERER_H
#define RENDERER_H

#include <stdbool.h>
#include <stdint.h>

#include "analyzer/analyzer.h"
#include "pixel_buffer.h"

typedef struct VisualizerConfig
{
  float brightness_power;
  float length_power;

  float min_semitone;
  float max_semitone;

  bool show_labels;
  bool show_cursor;

  // View toggles. When both are on, the spectrum is drawn first and the
  // waveform line is composited on top into the same framebuffer; when
  // exactly one is on it occupies the full canvas. When both are off the
  // framebuffer is just cleared to black.
  bool show_spectrum;
  bool show_waveform;

  // Waveform-only scale controls. `waveform_gain` multiplies sample
  // amplitude (clamped to [-1, 1] after scaling, so values >1 emphasize
  // quiet signals at the cost of clipping loud ones).
  //
  // `waveform_samples_per_px` is the horizontal zoom expressed in real
  // units: 1.0 means exactly one input sample per display pixel (the
  // "pure" 1:1 view), values < 1 zoom in (multiple pixels per sample),
  // values > 1 zoom out. The number of samples shown across the canvas
  // is `display_width * waveform_samples_per_px`, clamped to the
  // available buffer size.
  float waveform_gain;
  float waveform_samples_per_px;

  // C-1 frequency, semitone-0 anchor.
  float f0;

  // Hue (degrees) for each pitch class:
  // C, C#, D, D#, E, F, F#, G, G#, A, A#, B
  float note_hues[12];

  // Bar width in render-buffer pixels. With the supersampled pipeline this
  // should be set to RENDER_SCALE so each bar covers exactly one display
  // pixel after bilinear downsampling (matching p5 pixelDensity(N) +
  // stroke-weight-1 line semantics). Default 1 = literally one pixel in
  // the render buffer (which is sub-display-pixel under SSAA and will look
  // washed out / disappear after the downsample).
  int bar_width_px;
} VisualizerConfig;

VisualizerConfig visualizer_config_defaults(void);

// Renderer state. `column_max_energy` tracks the loudest bin energy that has
// already been drawn into each pixel column this frame, so that when multiple
// bins map to the same column the loudest one wins (without needing a second
// pass over the bins).
typedef struct Renderer
{
  float *column_max_energy;
  int width;
} Renderer;

bool renderer_init(Renderer *r, int width);
void renderer_destroy(Renderer *r);
bool renderer_resize(Renderer *r, int width);

// Draws the spectrum into `buf`. Does NOT clear the buffer first; callers
// that want a clean background should clear `buf` themselves (so multiple
// views can compose into the same framebuffer in a single frame).
void renderer_draw_spectrum(Renderer *r, PixelBuffer *buf, const AnalyzerOutput *output,
                            const AnalyzerConfig *acfg, const VisualizerConfig *vcfg);

// Draws a time-domain waveform line into `buf`. `samples` is interpreted
// as mono PCM in chronological order (oldest first). Does NOT clear the
// buffer first.
void renderer_draw_waveform(PixelBuffer *buf, const float *samples, int sample_count,
                            const VisualizerConfig *vcfg);

#endif
