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

#include "pcm_ring.h"

#include <stdlib.h>
#include <string.h>

bool pcm_ring_init(PcmRingBuffer *ring, uint64_t capacity_samples)
{
  if (!ring || capacity_samples == 0)
    return false;
  ring->data = (float *)calloc((size_t)capacity_samples, sizeof(float));
  if (!ring->data)
    return false;
  ring->capacity = capacity_samples;
  atomic_store_explicit(&ring->write_index, 0, memory_order_relaxed);
  return true;
}

void pcm_ring_destroy(PcmRingBuffer *ring)
{
  if (!ring)
    return;
  free(ring->data);
  ring->data = NULL;
  ring->capacity = 0;
  atomic_store_explicit(&ring->write_index, 0, memory_order_relaxed);
}

void pcm_ring_write(PcmRingBuffer *ring, const float *samples, uint64_t count)
{
  if (!ring || !ring->data || !samples || count == 0)
    return;
  const uint64_t cap = ring->capacity;
  const uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_relaxed);

  // If the caller sends more than `cap` samples in one go, only the trailing
  // `cap` samples can possibly survive, so skip the part that would be
  // immediately overwritten.
  uint64_t start = 0;
  uint64_t to_copy = count;
  if (to_copy > cap)
  {
    start = to_copy - cap;
    to_copy = cap;
  }

  const uint64_t off = (w + start) % cap;
  const uint64_t first = (off + to_copy <= cap) ? to_copy : (cap - off);
  memcpy(&ring->data[off], &samples[start], (size_t)first * sizeof(float));
  if (first < to_copy)
  {
    memcpy(&ring->data[0], &samples[start + first], (size_t)(to_copy - first) * sizeof(float));
  }

  atomic_store_explicit(&ring->write_index, w + count, memory_order_release);
}

bool pcm_ring_copy_latest(const PcmRingBuffer *ring, float *out, uint64_t count)
{
  if (!ring || !ring->data || !out || count == 0)
    return false;
  if (count > ring->capacity)
    return false;

  const uint64_t cap = ring->capacity;
  const uint64_t w = atomic_load_explicit(&ring->write_index, memory_order_acquire);

  if (w < count)
  {
    const uint64_t have = w;
    const uint64_t pad = count - have;
    memset(out, 0, (size_t)pad * sizeof(float));
    for (uint64_t i = 0; i < have; ++i)
    {
      out[pad + i] = ring->data[i % cap];
    }
    return true;
  }

  const uint64_t start = w - count;
  const uint64_t off = start % cap;
  const uint64_t first = (off + count <= cap) ? count : (cap - off);
  memcpy(out, &ring->data[off], (size_t)first * sizeof(float));
  if (first < count)
  {
    memcpy(&out[first], &ring->data[0], (size_t)(count - first) * sizeof(float));
  }
  return true;
}

uint64_t pcm_ring_total_written(const PcmRingBuffer *ring)
{
  if (!ring)
    return 0;
  return atomic_load_explicit(&ring->write_index, memory_order_acquire);
}
