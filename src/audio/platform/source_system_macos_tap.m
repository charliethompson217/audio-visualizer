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

// macOS system-audio source: captures the global output mix via a Core Audio
// process tap (macOS 14.2+) bundled into a private aggregate device, downmixes
// the tap's interleaved float frames to mono, and writes them to the shared
// ring. The audio still plays normally through speakers/headphones; the tap
// is a non-disruptive observer of the system mixer.
//
// References:
//   https://developer.apple.com/documentation/coreaudio/capturing-system-audio-with-core-audio-taps

// Foundation transitively drags in <hfs/hfs_format.h>, which uses
// `uuid_string_t` without including <uuid/uuid.h>. The system header expects
// it to already be in scope. We can't rely on `#include <uuid/uuid.h>`
// because Homebrew's include dir (added via -isystem for SDL3/FFTW) ships a
// conflicting libuuid that lacks this typedef and shadows the SDK's. Match
// the SDK's own guard so the real uuid.h won't re-typedef later.
#ifndef _UUID_STRING_T
#define _UUID_STRING_T
typedef char uuid_string_t[37];
#endif

#import <Foundation/Foundation.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../audio_source.h"

#define MS_MONO_SCRATCH 8192u

typedef struct MacSysSource
{
  PcmRingBuffer *ring;
  uint32_t sample_rate;

  AudioObjectID tap_id;
  AudioObjectID aggregate_id;
  AudioDeviceIOProcID proc_id;

  bool tap_created;
  bool aggregate_created;
  bool proc_created;
  _Atomic bool started;

  float mono_scratch[MS_MONO_SCRATCH];
} MacSysSource;

static OSStatus ms_io_proc(AudioObjectID inDevice, const AudioTimeStamp *inNow,
                           const AudioBufferList *inInputData, const AudioTimeStamp *inInputTime,
                           AudioBufferList *outOutputData, const AudioTimeStamp *inOutputTime,
                           void *inClientData)
{
  (void)inDevice;
  (void)inNow;
  (void)inInputTime;
  (void)outOutputData;
  (void)inOutputTime;
  MacSysSource *m = (MacSysSource *)inClientData;
  if (!inInputData)
    return noErr;
  for (UInt32 b = 0; b < inInputData->mNumberBuffers; ++b)
  {
    const AudioBuffer *buf = &inInputData->mBuffers[b];
    if (!buf->mData || buf->mDataByteSize == 0)
      continue;
    const UInt32 chans = buf->mNumberChannels ? buf->mNumberChannels : 1u;
    const UInt32 frames = buf->mDataByteSize / (chans * (UInt32)sizeof(float));
    const float *src = (const float *)buf->mData;
    if (chans == 1)
    {
      pcm_ring_write(m->ring, src, frames);
      continue;
    }
    const float inv_chans = 1.0f / (float)chans;
    UInt32 remaining = frames;
    const float *p = src;
    while (remaining > 0)
    {
      UInt32 chunk = remaining > MS_MONO_SCRATCH ? MS_MONO_SCRATCH : remaining;
      for (UInt32 i = 0; i < chunk; ++i)
      {
        float sum = 0.0f;
        for (UInt32 c = 0; c < chans; ++c)
          sum += p[i * chans + c];
        m->mono_scratch[i] = sum * inv_chans;
      }
      pcm_ring_write(m->ring, m->mono_scratch, chunk);
      p += chunk * chans;
      remaining -= chunk;
    }
  }
  return noErr;
}

static AudioObjectID ms_default_output_device(void)
{
  AudioObjectID dev = kAudioObjectUnknown;
  UInt32 size = (UInt32)sizeof(dev);
  AudioObjectPropertyAddress addr = {kAudioHardwarePropertyDefaultOutputDevice,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, &dev);
  return dev;
}

static CFStringRef ms_copy_device_uid(AudioObjectID dev)
{
  CFStringRef uid = NULL;
  UInt32 size = (UInt32)sizeof(uid);
  AudioObjectPropertyAddress addr = {kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &uid) != noErr)
  {
    return NULL;
  }
  return uid;
}

static Float64 ms_nominal_sample_rate(AudioObjectID dev)
{
  Float64 rate = 0.0;
  UInt32 size = (UInt32)sizeof(rate);
  AudioObjectPropertyAddress addr = {kAudioDevicePropertyNominalSampleRate,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  AudioObjectGetPropertyData(dev, &addr, 0, NULL, &size, &rate);
  return rate;
}

static bool ms_start(AudioSource *src)
{
  MacSysSource *m = (MacSysSource *)src->impl;
  if (atomic_load_explicit(&m->started, memory_order_acquire))
    return true;
  OSStatus s = AudioDeviceStart(m->aggregate_id, m->proc_id);
  if (s != noErr)
  {
    fprintf(stderr, "macos system source: AudioDeviceStart failed (status=%d)\n", (int)s);
    return false;
  }
  atomic_store_explicit(&m->started, true, memory_order_release);
  return true;
}

static void ms_stop(AudioSource *src)
{
  MacSysSource *m = (MacSysSource *)src->impl;
  if (!atomic_exchange_explicit(&m->started, false, memory_order_acq_rel))
    return;
  AudioDeviceStop(m->aggregate_id, m->proc_id);
}

static void ms_destroy(AudioSource *src)
{
  MacSysSource *m = (MacSysSource *)src->impl;
  if (!m)
    return;
  ms_stop(src);
  if (m->proc_created)
    AudioDeviceDestroyIOProcID(m->aggregate_id, m->proc_id);
  if (m->aggregate_created)
    AudioHardwareDestroyAggregateDevice(m->aggregate_id);
  if (m->tap_created)
    AudioHardwareDestroyProcessTap(m->tap_id);
  free(m);
  src->impl = NULL;
}

static const char *ms_name(AudioSource *src)
{
  (void)src;
  return "system (macOS tap)";
}
static uint32_t ms_sample_rate(AudioSource *src)
{
  return ((MacSysSource *)src->impl)->sample_rate;
}
static uint32_t ms_channels(AudioSource *src)
{
  (void)src;
  return 1;
}
static bool ms_is_running(AudioSource *src)
{
  return atomic_load_explicit(&((MacSysSource *)src->impl)->started, memory_order_acquire);
}

static const AudioSourceVTable kMacSystemTapVTable = {
    .start = ms_start,
    .stop = ms_stop,
    .destroy = ms_destroy,
    .name = ms_name,
    .sample_rate = ms_sample_rate,
    .channels = ms_channels,
    .is_running = ms_is_running,
};

bool audio_source_init_system_macos(AudioSource *src, const AudioSourceConfig *config)
{
  if (@available(macOS 14.2, *))
  {
  }
  else
  {
    fprintf(stderr, "macos system source: requires macOS 14.2 or newer\n");
    return false;
  }

  MacSysSource *m = (MacSysSource *)calloc(1, sizeof(*m));
  if (!m)
    return false;
  m->ring = config->ring;
  m->tap_id = kAudioObjectUnknown;
  m->aggregate_id = kAudioObjectUnknown;

  // Global stereo tap of all audio reaching the system mixer. Empty exclude
  // list = capture everything. The exclude-list API takes AudioObjectIDs
  // (process objects), NOT PIDs — different ID space. Passing an empty array
  // avoids that translation entirely and is what AudioCap does.
  CATapDescription *desc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
  desc.name = @"audio-visualizer system tap";
  // Pre-assign the tap's UUID so we can reference it in the aggregate device
  // configuration below without round-tripping through kAudioTapPropertyUID.
  desc.UUID = [NSUUID UUID];
  desc.muteBehavior = CATapUnmuted;
  desc.privateTap = YES;

  OSStatus s = AudioHardwareCreateProcessTap(desc, &m->tap_id);
  if (s != noErr || m->tap_id == kAudioObjectUnknown)
  {
    fprintf(stderr,
            "macos system source: AudioHardwareCreateProcessTap failed (status=%d). "
            "Approve audio recording for this binary in System Settings > "
            "Privacy & Security if prompted.\n",
            (int)s);
    free(m);
    return false;
  }
  m->tap_created = true;

  AudioObjectID out_dev = ms_default_output_device();
  CFStringRef out_uid_cf = (out_dev != kAudioObjectUnknown) ? ms_copy_device_uid(out_dev) : NULL;
  if (!out_uid_cf)
  {
    fprintf(stderr, "macos system source: could not read default output device UID\n");
    AudioHardwareDestroyProcessTap(m->tap_id);
    free(m);
    return false;
  }
  NSString *out_uid = (__bridge_transfer NSString *)out_uid_cf;

  NSString *agg_uid = [[NSUUID UUID] UUIDString];
  NSString *tap_uid = [desc.UUID UUIDString];
  NSDictionary *agg_cfg = @{
    @(kAudioAggregateDeviceNameKey) : @"audio-visualizer-aggregate",
    @(kAudioAggregateDeviceUIDKey) : agg_uid,
    @(kAudioAggregateDeviceMainSubDeviceKey) : out_uid,
    @(kAudioAggregateDeviceIsPrivateKey) : @YES,
    @(kAudioAggregateDeviceIsStackedKey) : @NO,
    @(kAudioAggregateDeviceTapAutoStartKey) : @YES,
    @(kAudioAggregateDeviceSubDeviceListKey) : @[ @{
      @(kAudioSubDeviceUIDKey) : out_uid,
    } ],
    @(kAudioAggregateDeviceTapListKey) : @[ @{
      @(kAudioSubTapDriftCompensationKey) : @YES,
      @(kAudioSubTapUIDKey) : tap_uid,
    } ],
  };

  s = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)agg_cfg, &m->aggregate_id);
  if (s != noErr || m->aggregate_id == kAudioObjectUnknown)
  {
    fprintf(stderr, "macos system source: AudioHardwareCreateAggregateDevice failed (status=%d)\n",
            (int)s);
    AudioHardwareDestroyProcessTap(m->tap_id);
    free(m);
    return false;
  }
  m->aggregate_created = true;

  const Float64 rate = ms_nominal_sample_rate(m->aggregate_id);
  m->sample_rate =
      (rate > 0.0) ? (uint32_t)rate : (config->sample_rate ? config->sample_rate : 48000u);

  s = AudioDeviceCreateIOProcID(m->aggregate_id, ms_io_proc, m, &m->proc_id);
  if (s != noErr || !m->proc_id)
  {
    fprintf(stderr, "macos system source: AudioDeviceCreateIOProcID failed (status=%d)\n", (int)s);
    AudioHardwareDestroyAggregateDevice(m->aggregate_id);
    AudioHardwareDestroyProcessTap(m->tap_id);
    free(m);
    return false;
  }
  m->proc_created = true;

  atomic_store_explicit(&m->started, false, memory_order_relaxed);

  src->vtable = &kMacSystemTapVTable;
  src->impl = m;
  return true;
}
