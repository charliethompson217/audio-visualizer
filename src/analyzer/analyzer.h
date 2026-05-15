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

#ifndef ANALYZER_H
#define ANALYZER_H

#include <stdbool.h>
#include <stdint.h>

#include <fftw3.h>

#include "window_fn.h"

typedef struct AnalyzerConfig
{
  uint32_t sample_rate;

  // Power of two. Allowed: 512, 1024, 2048, 4096, 8192, 16384, 32768.
  uint32_t fft_size;

  // WebAudio-style magnitude mapping.
  float min_db;
  float max_db;

  // 0.0 = no smoothing, 0.99 = very slow.
  float smoothing;

  WindowFunction window_function;
} AnalyzerConfig;

typedef struct AnalyzerOutput
{
  uint32_t bin_count;     // fft_size / 2 + 1
  float *magnitudes;      // linear magnitude (calibrated by window gain)
  float *db;              // dB values
  float *energy;          // 0..1 after dB scale
  float *smoothed_energy; // 0..1 after smoothing
} AnalyzerOutput;

typedef struct Analyzer
{
  AnalyzerConfig config;
  AnalyzerOutput output;

  // Internal FFTW state.
  float *input;            // length fft_size (windowed samples)
  fftwf_complex *spectrum; // length fft_size/2 + 1
  fftwf_plan plan;
  float *window;  // length fft_size
  float mag_norm; // multiplicative magnitude normalization

  bool initialized;
} Analyzer;

bool analyzer_init(Analyzer *analyzer, const AnalyzerConfig *config);
void analyzer_destroy(Analyzer *analyzer);

// Tears down internal buffers/plan and rebuilds for the new config.
// Returns false on allocation/plan failure; the analyzer is left destroyed
// in that case.
bool analyzer_reconfigure(Analyzer *analyzer, const AnalyzerConfig *config);

// `mono_samples` must contain exactly fft_size samples (chronological order,
// oldest first).
bool analyzer_process(Analyzer *analyzer, const float *mono_samples);

const AnalyzerConfig *analyzer_config(const Analyzer *analyzer);
const AnalyzerOutput *analyzer_output(const Analyzer *analyzer);

#endif
