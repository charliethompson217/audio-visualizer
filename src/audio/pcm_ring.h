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

#ifndef PCM_RING_H
#define PCM_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Single-writer / single-reader ring buffer of mono float PCM samples.
//
// The audio source (one writer thread) appends samples with pcm_ring_write().
// The main/render thread snapshots the latest N samples with
// pcm_ring_copy_latest(). `write_index` is the monotonically increasing total
// count of samples ever written; samples physically live at
// `data[write_index % capacity]`.
typedef struct PcmRingBuffer
{
  float *data;
  uint64_t capacity;
  _Atomic uint64_t write_index;
} PcmRingBuffer;

bool pcm_ring_init(PcmRingBuffer *ring, uint64_t capacity_samples);
void pcm_ring_destroy(PcmRingBuffer *ring);

// Append `count` samples. Safe to call from a real-time audio thread:
// no allocation, no locks. `count` may exceed `capacity` (older samples are
// silently overwritten as the ring wraps).
void pcm_ring_write(PcmRingBuffer *ring, const float *samples, uint64_t count);

// Copy the latest `count` samples into `out` in chronological order.
// If fewer than `count` samples have ever been written, the oldest entries of
// `out` are zero-padded. Returns false only when `count > capacity`.
bool pcm_ring_copy_latest(const PcmRingBuffer *ring, float *out, uint64_t count);

// Total number of samples ever written (monotonic). For diagnostics.
uint64_t pcm_ring_total_written(const PcmRingBuffer *ring);

#endif
