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

#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>

#include "analyzer/analyzer.h"
#include "audio/audio_source.h"
#include "audio/pcm_ring.h"
#include "render/pixel_buffer.h"
#include "render/renderer.h"
#include "ui/controls.h"

// AppState is the central, mutable shared state for the application.
// It grows phase-by-phase.
typedef struct AppState
{
  bool running;

  int window_width;
  int window_height;

  // Supersampled offscreen render target dimensions; equal to window
  // dimensions multiplied by RENDER_SCALE. SDL downsamples the texture
  // bilinearly when presenting it to the window, giving free horizontal
  // anti-aliasing for the bars.
  int render_width;
  int render_height;

  // Pending window dimensions captured from SDL resize events. Coalesced
  // and applied once per frame (before drawing) so a single drag doesn't
  // reallocate the pixel buffer / texture per event.
  bool resize_pending;
  int pending_window_width;
  int pending_window_height;

  // Ratio of physical pixels to logical points for the current window.
  // 1.0 on standard displays / Windows / Linux; 2.0 on Retina macOS. The
  // UI keeps all layout in logical points and uses this scale only to
  // bake font atlases at native resolution.
  float ui_pixel_scale;

  PixelBuffer pixel_buffer;

  // Audio pipeline.
  PcmRingBuffer ring;
  AudioSource *source;
  AudioSourceConfig source_config;

  // Pending source change requested by the UI or by the file picker
  // callback. The main loop applies it between events and rendering so
  // teardown never happens mid-frame or off-thread.
  bool source_change_pending;
  AudioSourceKind requested_source_kind;

  // Active file path (UTF-8). Populated from --file at startup and from
  // SDL_ShowOpenFileDialog's callback. Empty when no file has ever been
  // chosen.
  char current_file_path[1024];

  // True while a file picker dialog is in flight, to suppress reopening.
  bool file_picker_open;

  // Set by the UI when the user clicks BROWSE FILE… so the main loop opens
  // SDL_ShowOpenFileDialog on the main thread. Cleared after the dialog is
  // requested.
  bool open_file_picker_request;

  // Last user-visible source error, rendered in the settings modal.
  // Cleared on the next successful source change.
  char ui_error_msg[256];

  // Analyzer pipeline.
  Analyzer analyzer;
  AnalyzerConfig analyzer_config;
  bool analyzer_config_dirty;
  float *analysis_samples; // length = analyzer_config.fft_size

  // Latest mono PCM snapshot used by the time-domain (waveform) view.
  // Independent of the FFT window because the waveform looks best with a
  // shorter time slice than the analyzer needs.
  float *waveform_samples;
  uint32_t waveform_size;

  // Render pipeline.
  Renderer renderer;
  VisualizerConfig visualizer_config;

  // UI overlay (settings modal).
  Controls controls;

  // Last known mouse position in window coordinates. Updated from SDL
  // motion / enter / leave events. The in-canvas overlay uses these to
  // render the cursor readout when VisualizerConfig.show_cursor is on.
  float mouse_x;
  float mouse_y;
  bool mouse_in_window;

  // Opaque SDL handles. Kept as void* so that audio/analyzer/renderer
  // headers do not need to include SDL.
  void *sdl_window;
  void *sdl_renderer;
  void *sdl_texture;
} AppState;

#endif
