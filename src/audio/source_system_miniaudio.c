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

// System-audio source for non-Apple platforms, implemented via miniaudio:
//   - Windows: WASAPI loopback on the default render endpoint
//     (`ma_device_type_loopback`). The OS exposes the playback stream as a
//     capture stream, no aggregate device or kernel driver needed.
//   - Linux:   capture from the default sink's PulseAudio monitor source
//     (`<sink>.monitor`). Works against either a real PulseAudio server or
//     PipeWire's pulse compatibility layer.
//
// miniaudio handles native-format -> f32/mono conversion internally, so the
// data callback just appends frames to the shared ring buffer.

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniaudio.h"

#include "audio_source.h"

typedef struct SysLoopbackSource
{
  PcmRingBuffer *ring;
  uint32_t sample_rate;

#ifndef _WIN32
  ma_context context;
  bool context_inited;
#endif
  ma_device device;
  bool device_inited;

  _Atomic bool started;
} SysLoopbackSource;

static void sls_data_callback(ma_device *device, void *output, const void *input,
                              ma_uint32 frame_count)
{
  (void)output;
  SysLoopbackSource *s = (SysLoopbackSource *)device->pUserData;
  if (!input || frame_count == 0)
    return;
  // miniaudio downmixes to our requested mono float layout for us.
  pcm_ring_write(s->ring, (const float *)input, frame_count);
}

static bool sls_start(AudioSource *src)
{
  SysLoopbackSource *s = (SysLoopbackSource *)src->impl;
  if (atomic_load_explicit(&s->started, memory_order_acquire))
    return true;
  if (ma_device_start(&s->device) != MA_SUCCESS)
  {
    fprintf(stderr, "system source: ma_device_start failed\n");
    return false;
  }
  atomic_store_explicit(&s->started, true, memory_order_release);
  return true;
}

static void sls_stop(AudioSource *src)
{
  SysLoopbackSource *s = (SysLoopbackSource *)src->impl;
  if (!atomic_exchange_explicit(&s->started, false, memory_order_acq_rel))
    return;
  ma_device_stop(&s->device);
}

static void sls_destroy(AudioSource *src)
{
  SysLoopbackSource *s = (SysLoopbackSource *)src->impl;
  if (!s)
    return;
  sls_stop(src);
  if (s->device_inited)
    ma_device_uninit(&s->device);
#ifndef _WIN32
  if (s->context_inited)
    ma_context_uninit(&s->context);
#endif
  free(s);
  src->impl = NULL;
}

static const char *sls_name(AudioSource *src)
{
  (void)src;
#ifdef _WIN32
  return "system (WASAPI loopback)";
#else
  return "system (PulseAudio monitor)";
#endif
}
static uint32_t sls_sample_rate(AudioSource *src)
{
  return ((SysLoopbackSource *)src->impl)->sample_rate;
}
static uint32_t sls_channels(AudioSource *src)
{
  (void)src;
  return 1;
}
static bool sls_is_running(AudioSource *src)
{
  return atomic_load_explicit(&((SysLoopbackSource *)src->impl)->started, memory_order_acquire);
}

static const AudioSourceVTable kSysLoopbackVTable = {
    .start = sls_start,
    .stop = sls_stop,
    .destroy = sls_destroy,
    .name = sls_name,
    .sample_rate = sls_sample_rate,
    .channels = sls_channels,
    .is_running = sls_is_running,
};

#ifndef _WIN32
// Resolve the default sink's monitor source name. PulseAudio (and PipeWire's
// pulse compat layer) name the monitor of a sink as `<sink_name>.monitor`.
// We query the default playback device through miniaudio (which uses pulse
// introspection under the hood) and append the suffix.
static bool sls_resolve_default_monitor(ma_context *ctx, ma_device_id *out_id)
{
  ma_device_info sink = {0};
  if (ma_context_get_device_info(ctx, ma_device_type_playback, NULL, &sink) != MA_SUCCESS)
  {
    fprintf(stderr, "system source: ma_context_get_device_info(default sink) failed\n");
    return false;
  }
  if (sink.id.pulse[0] == '\0')
  {
    fprintf(stderr, "system source: default sink has no pulse name\n");
    return false;
  }
  memset(out_id, 0, sizeof(*out_id));
  int n = snprintf(out_id->pulse, sizeof(out_id->pulse), "%s.monitor", sink.id.pulse);
  if (n < 0 || (size_t)n >= sizeof(out_id->pulse))
  {
    fprintf(stderr, "system source: monitor name too long for default sink '%s'\n", sink.id.pulse);
    return false;
  }
  return true;
}
#endif

bool audio_source_init_system_miniaudio(AudioSource *src, const AudioSourceConfig *config)
{
  SysLoopbackSource *s = (SysLoopbackSource *)calloc(1, sizeof(*s));
  if (!s)
    return false;

  s->ring = config->ring;
  s->sample_rate = config->sample_rate ? config->sample_rate : 48000u;

#ifdef _WIN32
  // WASAPI loopback: capture the audio currently being mixed for the default
  // render endpoint. NULL device ID selects the default output device.
  ma_device_config dev_cfg = ma_device_config_init(ma_device_type_loopback);
  dev_cfg.capture.format = ma_format_f32;
  dev_cfg.capture.channels = 1;
  dev_cfg.sampleRate = s->sample_rate;
  dev_cfg.dataCallback = sls_data_callback;
  dev_cfg.pUserData = s;

  if (ma_device_init(NULL, &dev_cfg, &s->device) != MA_SUCCESS)
  {
    fprintf(stderr, "system source: ma_device_init (WASAPI loopback) failed. "
                    "Ensure a default output device is active.\n");
    free(s);
    return false;
  }
#else
  // Pin the backend to PulseAudio so we can address devices by pulse name and
  // talk to PipeWire's pulse compat layer uniformly. ALSA does not expose a
  // standard monitor-of-default-sink concept.
  ma_backend backends[] = {ma_backend_pulseaudio};
  if (ma_context_init(backends, 1, NULL, &s->context) != MA_SUCCESS)
  {
    fprintf(stderr, "system source: ma_context_init(pulseaudio) failed. "
                    "Install/run PulseAudio or PipeWire's pulse compat layer.\n");
    free(s);
    return false;
  }
  s->context_inited = true;

  ma_device_id monitor_id;
  if (!sls_resolve_default_monitor(&s->context, &monitor_id))
  {
    ma_context_uninit(&s->context);
    free(s);
    return false;
  }

  ma_device_config dev_cfg = ma_device_config_init(ma_device_type_capture);
  dev_cfg.capture.pDeviceID = &monitor_id;
  dev_cfg.capture.format = ma_format_f32;
  dev_cfg.capture.channels = 1;
  dev_cfg.sampleRate = s->sample_rate;
  dev_cfg.dataCallback = sls_data_callback;
  dev_cfg.pUserData = s;

  if (ma_device_init(&s->context, &dev_cfg, &s->device) != MA_SUCCESS)
  {
    fprintf(stderr, "system source: ma_device_init failed for monitor '%s'\n", monitor_id.pulse);
    ma_context_uninit(&s->context);
    free(s);
    return false;
  }
#endif
  s->device_inited = true;

  // miniaudio may negotiate a different sample rate than we asked for; surface
  // the actual rate so the analyzer can realign its bin->frequency map.
  if (s->device.sampleRate != 0)
    s->sample_rate = s->device.sampleRate;

  atomic_store_explicit(&s->started, false, memory_order_relaxed);

  src->vtable = &kSysLoopbackVTable;
  src->impl = s;
  return true;
}
