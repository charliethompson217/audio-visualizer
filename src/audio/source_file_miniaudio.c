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

// File audio source: decodes a file with miniaudio, plays it through the
// default output device, and writes the same audio (downmixed to mono) into
// the shared ring buffer for the analyzer.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniaudio.h"

#include "audio_source.h"

#define FM_MONO_SCRATCH 8192u
#define FM_PLAYBACK_CHANNELS 2u

typedef struct FileSource
{
  PcmRingBuffer *ring;
  char *path;
  uint32_t sample_rate;

  ma_decoder decoder;
  ma_device device;
  bool decoder_inited;
  bool device_inited;

  _Atomic bool started;

  float mono_scratch[FM_MONO_SCRATCH];
} FileSource;

static void fm_data_callback(ma_device *device, void *output, const void *input,
                             ma_uint32 frame_count)
{
  (void)input;
  FileSource *fs = (FileSource *)device->pUserData;

  ma_uint64 frames_read = 0;
  ma_decoder_read_pcm_frames(&fs->decoder, output, frame_count, &frames_read);

  float *out = (float *)output;
  if (frames_read < frame_count)
  {
    memset(out + frames_read * FM_PLAYBACK_CHANNELS, 0,
           (frame_count - frames_read) * FM_PLAYBACK_CHANNELS * sizeof(float));
  }

  // Downmix stereo to mono in chunks and append to ring.
  ma_uint32 remaining = frame_count;
  const float *p = out;
  while (remaining > 0)
  {
    ma_uint32 chunk = remaining > FM_MONO_SCRATCH ? FM_MONO_SCRATCH : remaining;
    for (ma_uint32 i = 0; i < chunk; ++i)
    {
      fs->mono_scratch[i] = 0.5f * (p[2 * i] + p[2 * i + 1]);
    }
    pcm_ring_write(fs->ring, fs->mono_scratch, chunk);
    p += chunk * FM_PLAYBACK_CHANNELS;
    remaining -= chunk;
  }
}

static bool fm_start(AudioSource *src)
{
  FileSource *fs = (FileSource *)src->impl;
  if (atomic_load_explicit(&fs->started, memory_order_acquire))
    return true;
  if (ma_device_start(&fs->device) != MA_SUCCESS)
    return false;
  atomic_store_explicit(&fs->started, true, memory_order_release);
  return true;
}

static void fm_stop(AudioSource *src)
{
  FileSource *fs = (FileSource *)src->impl;
  if (!atomic_exchange_explicit(&fs->started, false, memory_order_acq_rel))
    return;
  ma_device_stop(&fs->device);
}

static void fm_destroy(AudioSource *src)
{
  FileSource *fs = (FileSource *)src->impl;
  if (!fs)
    return;
  fm_stop(src);
  if (fs->device_inited)
    ma_device_uninit(&fs->device);
  if (fs->decoder_inited)
    ma_decoder_uninit(&fs->decoder);
  free(fs->path);
  free(fs);
  src->impl = NULL;
}

static const char *fm_name(AudioSource *src)
{
  (void)src;
  return "file";
}
static uint32_t fm_sample_rate(AudioSource *src)
{
  return ((FileSource *)src->impl)->sample_rate;
}
static uint32_t fm_channels(AudioSource *src)
{
  (void)src;
  return 1;
}
static bool fm_is_running(AudioSource *src)
{
  return atomic_load_explicit(&((FileSource *)src->impl)->started, memory_order_acquire);
}

static const AudioSourceVTable kFileMiniaudioVTable = {
    .start = fm_start,
    .stop = fm_stop,
    .destroy = fm_destroy,
    .name = fm_name,
    .sample_rate = fm_sample_rate,
    .channels = fm_channels,
    .is_running = fm_is_running,
};

bool audio_source_init_file_miniaudio(AudioSource *src, const AudioSourceConfig *config)
{
  if (!config->file_path || config->file_path[0] == '\0')
  {
    fprintf(stderr, "file source: missing --file path\n");
    return false;
  }

  FileSource *fs = (FileSource *)calloc(1, sizeof(*fs));
  if (!fs)
    return false;

  fs->ring = config->ring;
  fs->sample_rate = config->sample_rate ? config->sample_rate : 48000u;
  fs->path = strdup(config->file_path);
  if (!fs->path)
  {
    free(fs);
    return false;
  }

  ma_decoder_config dec_cfg =
      ma_decoder_config_init(ma_format_f32, FM_PLAYBACK_CHANNELS, fs->sample_rate);
  if (ma_decoder_init_file(fs->path, &dec_cfg, &fs->decoder) != MA_SUCCESS)
  {
    fprintf(stderr, "file source: cannot decode '%s'\n", fs->path);
    free(fs->path);
    free(fs);
    return false;
  }
  fs->decoder_inited = true;

  ma_device_config dev_cfg = ma_device_config_init(ma_device_type_playback);
  dev_cfg.playback.format = ma_format_f32;
  dev_cfg.playback.channels = FM_PLAYBACK_CHANNELS;
  dev_cfg.sampleRate = fs->sample_rate;
  dev_cfg.dataCallback = fm_data_callback;
  dev_cfg.pUserData = fs;

  if (ma_device_init(NULL, &dev_cfg, &fs->device) != MA_SUCCESS)
  {
    fprintf(stderr, "file source: ma_device_init failed\n");
    ma_decoder_uninit(&fs->decoder);
    free(fs->path);
    free(fs);
    return false;
  }
  fs->device_inited = true;

  atomic_store_explicit(&fs->started, false, memory_order_relaxed);

  src->vtable = &kFileMiniaudioVTable;
  src->impl = fs;
  return true;
}
