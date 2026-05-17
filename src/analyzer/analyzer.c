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

#include "analyzer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static void zero_output(AnalyzerOutput *out)
{
  out->bin_count = 0;
  out->magnitudes = NULL;
  out->db = NULL;
  out->energy = NULL;
  out->smoothed_energy = NULL;
}

static void free_internals(Analyzer *a)
{
  if (a->plan)
  {
    fftwf_destroy_plan(a->plan);
    a->plan = NULL;
  }
  if (a->input)
  {
    fftwf_free(a->input);
    a->input = NULL;
  }
  if (a->spectrum)
  {
    fftwf_free(a->spectrum);
    a->spectrum = NULL;
  }
  free(a->window);
  a->window = NULL;
  free(a->output.magnitudes);
  free(a->output.db);
  free(a->output.energy);
  free(a->output.smoothed_energy);
  zero_output(&a->output);
}

static bool alloc_internals(Analyzer *a)
{
  const uint32_t N = a->config.fft_size;
  const uint32_t bins = N / 2u + 1u;

  a->input = (float *)fftwf_malloc(sizeof(float) * N);
  a->spectrum = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * bins);
  a->window = (float *)calloc(N, sizeof(float));
  a->output.magnitudes = (float *)calloc(bins, sizeof(float));
  a->output.db = (float *)calloc(bins, sizeof(float));
  a->output.energy = (float *)calloc(bins, sizeof(float));
  a->output.smoothed_energy = (float *)calloc(bins, sizeof(float));
  if (!a->input || !a->spectrum || !a->window || !a->output.magnitudes || !a->output.db ||
      !a->output.energy || !a->output.smoothed_energy)
  {
    return false;
  }
  a->output.bin_count = bins;
  return true;
}

bool analyzer_init(Analyzer *analyzer, const AnalyzerConfig *config)
{
  if (!analyzer || !config || config->fft_size < 2 || config->sample_rate == 0)
    return false;

  memset(analyzer, 0, sizeof(*analyzer));
  analyzer->config = *config;

  if (!alloc_internals(analyzer))
  {
    free_internals(analyzer);
    return false;
  }

  window_fn_fill(analyzer->window, analyzer->config.fft_size, analyzer->config.window_function);

  analyzer->plan = fftwf_plan_dft_r2c_1d((int)analyzer->config.fft_size, analyzer->input,
                                         analyzer->spectrum, FFTW_ESTIMATE);
  if (!analyzer->plan)
  {
    free_internals(analyzer);
    return false;
  }

  // Calibrate magnitudes so a pure full-scale sine yields ~0 dB before windowing:
  //   |X[k]| for a tone of amplitude A is roughly A * N * coherent_gain / 2.
  // We divide by that so unity-amplitude sinusoid -> magnitude 1.
  const float gain = window_fn_coherent_gain(analyzer->config.window_function);
  analyzer->mag_norm = 2.0f / ((float)analyzer->config.fft_size * gain);

  analyzer->initialized = true;
  return true;
}

void analyzer_destroy(Analyzer *analyzer)
{
  if (!analyzer || !analyzer->initialized)
    return;
  free_internals(analyzer);
  analyzer->initialized = false;
}

bool analyzer_reconfigure(Analyzer *analyzer, const AnalyzerConfig *config)
{
  if (!analyzer)
    return false;
  analyzer_destroy(analyzer);
  return analyzer_init(analyzer, config);
}

bool analyzer_process(Analyzer *analyzer, const float *mono_samples)
{
  if (!analyzer || !analyzer->initialized || !mono_samples)
    return false;

  const uint32_t N = analyzer->config.fft_size;
  const uint32_t bins = analyzer->output.bin_count;
  const float min_db = analyzer->config.min_db;
  const float max_db = analyzer->config.max_db;
  const float range = (max_db > min_db) ? (max_db - min_db) : 1.0f;
  const float smooth = analyzer->config.smoothing;
  const float inv_one_minus_smooth = 1.0f - smooth;

  for (uint32_t i = 0; i < N; ++i)
  {
    analyzer->input[i] = mono_samples[i] * analyzer->window[i];
  }

  fftwf_execute(analyzer->plan);

  const float k = analyzer->mag_norm;
  for (uint32_t i = 0; i < bins; ++i)
  {
    const float re = analyzer->spectrum[i][0];
    const float im = analyzer->spectrum[i][1];
    const float mag = sqrtf(re * re + im * im) * k;

    const float db = 20.0f * log10f(mag + 1e-12f);
    float e = (db - min_db) / range;
    if (e < 0.0f)
      e = 0.0f;
    else if (e > 1.0f)
      e = 1.0f;

    analyzer->output.magnitudes[i] = mag;
    analyzer->output.db[i] = db;
    analyzer->output.energy[i] = e;
    analyzer->output.smoothed_energy[i] =
        smooth * analyzer->output.smoothed_energy[i] + inv_one_minus_smooth * e;
  }
  return true;
}

const AnalyzerConfig *analyzer_config(const Analyzer *analyzer)
{
  return analyzer ? &analyzer->config : NULL;
}

const AnalyzerOutput *analyzer_output(const Analyzer *analyzer)
{
  return analyzer ? &analyzer->output : NULL;
}
