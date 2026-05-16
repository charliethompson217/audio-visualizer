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

#include "audio_source.h"

#include <stdio.h>
#include <stdlib.h>

// Implementation-private initializers. Each lives in its source file and
// installs `vtable` and `impl` into the supplied AudioSource shell.
bool audio_source_init_test_tone(AudioSource *src, const AudioSourceConfig *config);
bool audio_source_init_file_miniaudio(AudioSource *src, const AudioSourceConfig *config);
bool audio_source_init_mic_miniaudio(AudioSource *src, const AudioSourceConfig *config);
#ifdef __APPLE__
bool audio_source_init_system_macos(AudioSource *src, const AudioSourceConfig *config);
#else
bool audio_source_init_system_miniaudio(AudioSource *src, const AudioSourceConfig *config);
#endif

AudioSource *audio_source_create(const AudioSourceConfig *config)
{
  if (!config || !config->ring)
    return NULL;

  AudioSource *src = (AudioSource *)calloc(1, sizeof(*src));
  if (!src)
    return NULL;

  bool ok = false;
  switch (config->kind)
  {
  case AUDIO_SOURCE_TEST_TONE:
    ok = audio_source_init_test_tone(src, config);
    break;
  case AUDIO_SOURCE_FILE:
    ok = audio_source_init_file_miniaudio(src, config);
    break;
  case AUDIO_SOURCE_MIC:
    ok = audio_source_init_mic_miniaudio(src, config);
    break;
  case AUDIO_SOURCE_SYSTEM:
#ifdef __APPLE__
    ok = audio_source_init_system_macos(src, config);
#else
    ok = audio_source_init_system_miniaudio(src, config);
#endif
    break;
  default:
    fprintf(stderr, "audio_source: kind %d not implemented yet\n", (int)config->kind);
    break;
  }

  if (!ok)
  {
    free(src);
    return NULL;
  }
  return src;
}

bool audio_source_start(AudioSource *s)
{
  return (s && s->vtable && s->vtable->start) ? s->vtable->start(s) : false;
}

void audio_source_stop(AudioSource *s)
{
  if (s && s->vtable && s->vtable->stop)
    s->vtable->stop(s);
}

void audio_source_destroy(AudioSource *s)
{
  if (!s)
    return;
  if (s->vtable && s->vtable->destroy)
    s->vtable->destroy(s);
  free(s);
}

const char *audio_source_name(AudioSource *s)
{
  return (s && s->vtable && s->vtable->name) ? s->vtable->name(s) : "";
}

uint32_t audio_source_sample_rate(AudioSource *s)
{
  return (s && s->vtable && s->vtable->sample_rate) ? s->vtable->sample_rate(s) : 0;
}

uint32_t audio_source_channels(AudioSource *s)
{
  return (s && s->vtable && s->vtable->channels) ? s->vtable->channels(s) : 0;
}

bool audio_source_is_running(AudioSource *s)
{
  return (s && s->vtable && s->vtable->is_running) ? s->vtable->is_running(s) : false;
}
