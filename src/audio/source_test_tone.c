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

// Test-tone audio source: layered sine waves generated on a worker thread.
// Produces mono float samples in [-1, 1] and writes them into the shared ring.
// Does not play to speakers; its purpose is verifying ring + analyzer.

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

#include "audio_source.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TT_NUM_TONES 3
#define TT_CHUNK 1024

typedef struct TestTone
{
  PcmRingBuffer *ring;
  uint32_t sample_rate;

  double freq[TT_NUM_TONES];
  float amp[TT_NUM_TONES];
  double phase[TT_NUM_TONES];

  pthread_t thread;
  _Atomic bool running;
  _Atomic bool thread_started;
} TestTone;

static void *tt_worker(void *arg)
{
  TestTone *tt = (TestTone *)arg;
  const uint32_t sr = tt->sample_rate;
  const double dt = 1.0 / (double)sr;
  const double two_pi = 2.0 * M_PI;

  float buf[TT_CHUNK];

  // Sleep just under one chunk's wall-time per iteration. Generation is
  // sample-accurate via the phase accumulators; pacing only limits CPU.
  const long sleep_ns = (long)((double)TT_CHUNK / (double)sr * 1e9 * 0.9);

  while (atomic_load_explicit(&tt->running, memory_order_acquire))
  {
    for (size_t i = 0; i < TT_CHUNK; ++i)
    {
      float s = 0.0f;
      for (int k = 0; k < TT_NUM_TONES; ++k)
      {
        s += tt->amp[k] * (float)sin(tt->phase[k]);
        tt->phase[k] += two_pi * tt->freq[k] * dt;
        if (tt->phase[k] >= two_pi)
          tt->phase[k] -= two_pi;
      }
      buf[i] = s;
    }
    pcm_ring_write(tt->ring, buf, TT_CHUNK);

    struct timespec ts = {.tv_sec = 0, .tv_nsec = sleep_ns};
    nanosleep(&ts, NULL);
  }
  return NULL;
}

static bool tt_start(AudioSource *src)
{
  TestTone *tt = (TestTone *)src->impl;
  if (atomic_load_explicit(&tt->thread_started, memory_order_acquire))
    return true;
  atomic_store_explicit(&tt->running, true, memory_order_release);
  if (pthread_create(&tt->thread, NULL, tt_worker, tt) != 0)
  {
    atomic_store_explicit(&tt->running, false, memory_order_release);
    return false;
  }
  atomic_store_explicit(&tt->thread_started, true, memory_order_release);
  return true;
}

static void tt_stop(AudioSource *src)
{
  TestTone *tt = (TestTone *)src->impl;
  if (!atomic_exchange_explicit(&tt->thread_started, false, memory_order_acq_rel))
    return;
  atomic_store_explicit(&tt->running, false, memory_order_release);
  pthread_join(tt->thread, NULL);
}

static void tt_destroy(AudioSource *src)
{
  TestTone *tt = (TestTone *)src->impl;
  if (!tt)
    return;
  tt_stop(src);
  free(tt);
  src->impl = NULL;
}

static const char *tt_name(AudioSource *src)
{
  (void)src;
  return "test-tone";
}
static uint32_t tt_sample_rate(AudioSource *src)
{
  return ((TestTone *)src->impl)->sample_rate;
}
static uint32_t tt_channels(AudioSource *src)
{
  (void)src;
  return 1;
}
static bool tt_is_running(AudioSource *src)
{
  return atomic_load_explicit(&((TestTone *)src->impl)->running, memory_order_acquire);
}

static const AudioSourceVTable kTestToneVTable = {
    .start = tt_start,
    .stop = tt_stop,
    .destroy = tt_destroy,
    .name = tt_name,
    .sample_rate = tt_sample_rate,
    .channels = tt_channels,
    .is_running = tt_is_running,
};

bool audio_source_init_test_tone(AudioSource *src, const AudioSourceConfig *config)
{
  TestTone *tt = (TestTone *)calloc(1, sizeof(*tt));
  if (!tt)
    return false;

  tt->ring = config->ring;
  tt->sample_rate = config->sample_rate ? config->sample_rate : 48000u;

  tt->freq[0] = 440.0;
  tt->amp[0] = 0.30f;
  tt->freq[1] = 880.0;
  tt->amp[1] = 0.20f;
  tt->freq[2] = 1760.0;
  tt->amp[2] = 0.10f;

  atomic_store_explicit(&tt->running, false, memory_order_relaxed);
  atomic_store_explicit(&tt->thread_started, false, memory_order_relaxed);

  src->vtable = &kTestToneVTable;
  src->impl = tt;
  return true;
}
