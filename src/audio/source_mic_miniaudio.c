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

// Microphone audio source: opens the default capture device via miniaudio,
// requests mono float frames, and forwards them straight into the shared
// ring buffer. miniaudio handles native-format -> f32/mono conversion in
// its internal pre-buffer, so the data callback does no allocation or
// heavy work.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniaudio.h"

#include "audio_source.h"

#define MM_CAPTURE_CHANNELS 1u

typedef struct MicSource
{
  PcmRingBuffer *ring;
  uint32_t sample_rate;

  ma_device device;
  bool device_inited;

  _Atomic bool started;
} MicSource;

static void mm_data_callback(ma_device *device, void *output, const void *input,
                             ma_uint32 frame_count)
{
  (void)output;
  MicSource *ms = (MicSource *)device->pUserData;
  if (!input || frame_count == 0)
    return;
  // input is already mono float in our requested format.
  pcm_ring_write(ms->ring, (const float *)input, frame_count);
}

static bool mm_start(AudioSource *src)
{
  MicSource *ms = (MicSource *)src->impl;
  if (atomic_load_explicit(&ms->started, memory_order_acquire))
    return true;
  if (ma_device_start(&ms->device) != MA_SUCCESS)
  {
    fprintf(stderr, "mic source: ma_device_start failed\n");
    return false;
  }
  atomic_store_explicit(&ms->started, true, memory_order_release);
  return true;
}

static void mm_stop(AudioSource *src)
{
  MicSource *ms = (MicSource *)src->impl;
  if (!atomic_exchange_explicit(&ms->started, false, memory_order_acq_rel))
    return;
  ma_device_stop(&ms->device);
}

static void mm_destroy(AudioSource *src)
{
  MicSource *ms = (MicSource *)src->impl;
  if (!ms)
    return;
  mm_stop(src);
  if (ms->device_inited)
    ma_device_uninit(&ms->device);
  free(ms);
  src->impl = NULL;
}

static const char *mm_name(AudioSource *src)
{
  (void)src;
  return "mic";
}
static uint32_t mm_sample_rate(AudioSource *src)
{
  return ((MicSource *)src->impl)->sample_rate;
}
static uint32_t mm_channels(AudioSource *src)
{
  (void)src;
  return 1;
}
static bool mm_is_running(AudioSource *src)
{
  return atomic_load_explicit(&((MicSource *)src->impl)->started, memory_order_acquire);
}

static const AudioSourceVTable kMicMiniaudioVTable = {
    .start = mm_start,
    .stop = mm_stop,
    .destroy = mm_destroy,
    .name = mm_name,
    .sample_rate = mm_sample_rate,
    .channels = mm_channels,
    .is_running = mm_is_running,
};

bool audio_source_init_mic_miniaudio(AudioSource *src, const AudioSourceConfig *config)
{
  MicSource *ms = (MicSource *)calloc(1, sizeof(*ms));
  if (!ms)
    return false;

  ms->ring = config->ring;
  ms->sample_rate = config->sample_rate ? config->sample_rate : 48000u;

  ma_device_config dev_cfg = ma_device_config_init(ma_device_type_capture);
  dev_cfg.capture.format = ma_format_f32;
  dev_cfg.capture.channels = MM_CAPTURE_CHANNELS;
  dev_cfg.sampleRate = ms->sample_rate;
  dev_cfg.dataCallback = mm_data_callback;
  dev_cfg.pUserData = ms;

  if (ma_device_init(NULL, &dev_cfg, &ms->device) != MA_SUCCESS)
  {
    fprintf(stderr, "mic source: ma_device_init failed (check microphone permissions in "
                    "System Settings > Privacy & Security > Microphone)\n");
    free(ms);
    return false;
  }
  ms->device_inited = true;

  atomic_store_explicit(&ms->started, false, memory_order_relaxed);

  src->vtable = &kMicMiniaudioVTable;
  src->impl = ms;
  return true;
}
