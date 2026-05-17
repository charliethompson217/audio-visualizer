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

// Demo audio source: decodes the bundled public-domain demo-song.mp3 with
// miniaudio, plays it through the default output device, loops forever, and
// writes a downmixed-to-mono copy into the shared ring for the analyzer.
// The bundled MP3 ships at:
//   - macOS:        Contents/Resources/demo-song.mp3 inside the .app
//   - Linux/Windows: alongside the executable
// SDL_GetBasePath() returns the correct directory on all three platforms
// (Resources on macOS, exe dir on Linux/Windows) with a trailing separator.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "miniaudio.h"

#include "audio_source.h"

#define DM_MONO_SCRATCH 8192u
#define DM_PLAYBACK_CHANNELS 2u
#define DM_DEMO_FILENAME "demo-song.mp3"

typedef struct DemoSource
{
  PcmRingBuffer *ring;
  uint32_t sample_rate;

  ma_decoder decoder;
  ma_device device;
  bool decoder_inited;
  bool device_inited;

  _Atomic bool started;

  float mono_scratch[DM_MONO_SCRATCH];
} DemoSource;

static void dm_data_callback(ma_device *device, void *output, const void *input,
                             ma_uint32 frame_count)
{
  (void)input;
  DemoSource *ds = (DemoSource *)device->pUserData;

  ma_uint64 frames_read = 0;
  ma_decoder_read_pcm_frames(&ds->decoder, output, frame_count, &frames_read);

  float *out = (float *)output;
  if (frames_read < frame_count)
  {
    memset(out + frames_read * DM_PLAYBACK_CHANNELS, 0,
           (frame_count - frames_read) * DM_PLAYBACK_CHANNELS * sizeof(float));
  }

  ma_uint32 remaining = frame_count;
  const float *p = out;
  while (remaining > 0)
  {
    ma_uint32 chunk = remaining > DM_MONO_SCRATCH ? DM_MONO_SCRATCH : remaining;
    for (ma_uint32 i = 0; i < chunk; ++i)
    {
      ds->mono_scratch[i] = 0.5f * (p[2 * i] + p[2 * i + 1]);
    }
    pcm_ring_write(ds->ring, ds->mono_scratch, chunk);
    p += chunk * DM_PLAYBACK_CHANNELS;
    remaining -= chunk;
  }
}

static bool dm_start(AudioSource *src)
{
  DemoSource *ds = (DemoSource *)src->impl;
  if (atomic_load_explicit(&ds->started, memory_order_acquire))
    return true;
  if (ma_device_start(&ds->device) != MA_SUCCESS)
    return false;
  atomic_store_explicit(&ds->started, true, memory_order_release);
  return true;
}

static void dm_stop(AudioSource *src)
{
  DemoSource *ds = (DemoSource *)src->impl;
  if (!atomic_exchange_explicit(&ds->started, false, memory_order_acq_rel))
    return;
  ma_device_stop(&ds->device);
}

static void dm_destroy(AudioSource *src)
{
  DemoSource *ds = (DemoSource *)src->impl;
  if (!ds)
    return;
  dm_stop(src);
  if (ds->device_inited)
    ma_device_uninit(&ds->device);
  if (ds->decoder_inited)
    ma_decoder_uninit(&ds->decoder);
  free(ds);
  src->impl = NULL;
}

static const char *dm_name(AudioSource *src)
{
  (void)src;
  return "demo";
}
static uint32_t dm_sample_rate(AudioSource *src)
{
  return ((DemoSource *)src->impl)->sample_rate;
}
static uint32_t dm_channels(AudioSource *src)
{
  (void)src;
  return 1;
}
static bool dm_is_running(AudioSource *src)
{
  return atomic_load_explicit(&((DemoSource *)src->impl)->started, memory_order_acquire);
}

static const AudioSourceVTable kDemoVTable = {
    .start = dm_start,
    .stop = dm_stop,
    .destroy = dm_destroy,
    .name = dm_name,
    .sample_rate = dm_sample_rate,
    .channels = dm_channels,
    .is_running = dm_is_running,
};

// Build the absolute path to the bundled demo MP3. SDL_GetBasePath returns
// a pointer that is owned by SDL (do not free) and is guaranteed to end in
// a path separator. NULL means SDL doesn't know where the executable lives
// on this platform — fall back to a relative lookup in the cwd.
static bool resolve_demo_path(char *out, size_t out_cap)
{
  const char *base = SDL_GetBasePath();
  int n;
  if (base && *base)
    n = snprintf(out, out_cap, "%s%s", base, DM_DEMO_FILENAME);
  else
    n = snprintf(out, out_cap, "%s", DM_DEMO_FILENAME);
  if (n < 0 || n >= (int)out_cap)
  {
    out[0] = '\0';
    return false;
  }
  return true;
}

bool audio_source_init_demo(AudioSource *src, const AudioSourceConfig *config)
{
  DemoSource *ds = (DemoSource *)calloc(1, sizeof(*ds));
  if (!ds)
    return false;

  ds->ring = config->ring;
  ds->sample_rate = config->sample_rate ? config->sample_rate : 48000u;

  char path[1024];
  if (!resolve_demo_path(path, sizeof(path)))
  {
    fprintf(stderr, "demo source: could not resolve bundled demo path\n");
    free(ds);
    return false;
  }

  ma_decoder_config dec_cfg =
      ma_decoder_config_init(ma_format_f32, DM_PLAYBACK_CHANNELS, ds->sample_rate);
  if (ma_decoder_init_file(path, &dec_cfg, &ds->decoder) != MA_SUCCESS)
  {
    fprintf(stderr, "demo source: cannot decode '%s'\n", path);
    free(ds);
    return false;
  }
  ds->decoder_inited = true;

  // Loop the demo so reviewers always hear something animated, no matter
  // how long the app sits on the demo source.
  ma_data_source_set_looping(&ds->decoder, MA_TRUE);

  ma_device_config dev_cfg = ma_device_config_init(ma_device_type_playback);
  dev_cfg.playback.format = ma_format_f32;
  dev_cfg.playback.channels = DM_PLAYBACK_CHANNELS;
  dev_cfg.sampleRate = ds->sample_rate;
  dev_cfg.dataCallback = dm_data_callback;
  dev_cfg.pUserData = ds;

  if (ma_device_init(NULL, &dev_cfg, &ds->device) != MA_SUCCESS)
  {
    fprintf(stderr, "demo source: ma_device_init failed\n");
    ma_decoder_uninit(&ds->decoder);
    free(ds);
    return false;
  }
  ds->device_inited = true;

  atomic_store_explicit(&ds->started, false, memory_order_relaxed);

  src->vtable = &kDemoVTable;
  src->impl = ds;
  return true;
}
