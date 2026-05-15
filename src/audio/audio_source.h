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

#ifndef AUDIO_SOURCE_H
#define AUDIO_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

#include "pcm_ring.h"

typedef enum AudioSourceKind
{
  AUDIO_SOURCE_TEST_TONE = 0,
  AUDIO_SOURCE_FILE,
  AUDIO_SOURCE_MIC,
  AUDIO_SOURCE_SYSTEM
} AudioSourceKind;

typedef struct AudioSourceConfig
{
  AudioSourceKind kind;

  // File source only.
  const char *file_path;

  // Desired output format. Sources convert to this when they can.
  uint32_t sample_rate;
  uint32_t channels;

  // Optional backend-specific device identifier.
  const char *device_id;

  // Ring buffer that the source writes mono float samples into.
  // Owned by the caller; must outlive the AudioSource.
  PcmRingBuffer *ring;
} AudioSourceConfig;

typedef struct AudioSource AudioSource;

typedef struct AudioSourceVTable
{
  bool (*start)(AudioSource *source);
  void (*stop)(AudioSource *source);
  void (*destroy)(AudioSource *source);

  const char *(*name)(AudioSource *source);
  uint32_t (*sample_rate)(AudioSource *source);
  uint32_t (*channels)(AudioSource *source);

  bool (*is_running)(AudioSource *source);
} AudioSourceVTable;

struct AudioSource
{
  const AudioSourceVTable *vtable;
  void *impl;
};

AudioSource *audio_source_create(const AudioSourceConfig *config);

bool audio_source_start(AudioSource *source);
void audio_source_stop(AudioSource *source);
void audio_source_destroy(AudioSource *source);

const char *audio_source_name(AudioSource *source);
uint32_t audio_source_sample_rate(AudioSource *source);
uint32_t audio_source_channels(AudioSource *source);
bool audio_source_is_running(AudioSource *source);

#endif
