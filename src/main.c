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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "analyzer/analyzer.h"
#include "app_state.h"
#include "audio/audio_source.h"
#include "audio/pcm_ring.h"
#include "config.h"
#include "render/pixel_buffer.h"
#include "render/renderer.h"
#include "ui/controls.h"
#include "ui/overlay.h"
#include "ui/text.h"

// Phase 4: SDL window + audio source (bundled demo song, file via
// miniaudio, microphone, or system loopback) + ring buffer + FFTW
// analyzer + log-semitone bar renderer.

typedef struct CliArgs
{
  // True when the user passed an explicit source flag. False CLI values
  // must not clobber whatever config_load() restored from the previous
  // run; only an explicit flag overrides persistence.
  bool kind_set;
  AudioSourceKind kind;
  const char *file_path;
} CliArgs;

// SDL user event type used to deliver a file-picker result back to the main
// thread. SDL_ShowOpenFileDialog's callback may run on a worker thread, so
// we strdup the chosen path and push it through the event queue instead of
// touching AppState directly. ev.user.data1 owns the path string (or NULL on
// cancel) and must be SDL_free()'d by the receiver.
static Uint32 g_file_picked_event = 0;

static void print_usage(const char *prog)
{
  fprintf(stderr,
          "usage: %s [--demo | --file PATH | --mic | --system]\n"
          "  default on first launch: --demo (bundled public-domain song)\n",
          prog);
}

static bool parse_cli(int argc, char **argv, CliArgs *out)
{
  out->kind_set = false;
  out->kind = AUDIO_SOURCE_DEMO;
  out->file_path = NULL;
  for (int i = 1; i < argc; ++i)
  {
    const char *a = argv[i];
    if (strcmp(a, "--demo") == 0)
    {
      out->kind = AUDIO_SOURCE_DEMO;
      out->kind_set = true;
    }
    else if (strcmp(a, "--file") == 0)
    {
      if (i + 1 >= argc)
      {
        fprintf(stderr, "--file requires a path argument\n");
        return false;
      }
      out->kind = AUDIO_SOURCE_FILE;
      out->kind_set = true;
      out->file_path = argv[++i];
    }
    else if (strcmp(a, "--mic") == 0)
    {
      out->kind = AUDIO_SOURCE_MIC;
      out->kind_set = true;
    }
    else if (strcmp(a, "--system") == 0)
    {
      out->kind = AUDIO_SOURCE_SYSTEM;
      out->kind_set = true;
    }
    else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)
    {
      print_usage(argv[0]);
      return false;
    }
    else
    {
      fprintf(stderr, "unknown argument: %s\n", a);
      print_usage(argv[0]);
      return false;
    }
  }
  return true;
}

static const int DEFAULT_WIDTH = 1280;
static const int DEFAULT_HEIGHT = 720;

// Offscreen supersampling factor — we render into a 3x framebuffer and let SDL
// downsample bilinearly when presenting, giving anti-aliased bars.
static const int RENDER_SCALE = 3;

static const uint32_t SAMPLE_RATE = 48000;
static const uint64_t RING_CAPACITY = 65536; // ~1.36s @ 48 kHz

static const uint32_t FFT_SIZE = 32768;
// Waveform view sample window. Sized generously (~680 ms at 48 kHz) so
// that the H-zoom slider has headroom to zoom out past the display-pixel
// 1:1 anchor — at zoom = 0 we only consume `display_width` samples, but
// positive zoom values multiply that, up to this cap.
static const uint32_t WAVEFORM_SIZE = 32768;
static const float DEFAULT_MIN_DB = -70.0f;
static const float DEFAULT_MAX_DB = -12.0f;
static const float DEFAULT_SMOOTHING = 0.8f;

static bool init_sdl(AppState *app)
{
  if (!SDL_Init(SDL_INIT_VIDEO))
  {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_WindowFlags win_flags = SDL_WINDOW_RESIZABLE;
#ifdef __APPLE__
  // Ask Cocoa for a backing store at the display's native pixel density
  // so UI text can be drawn at native resolution on Retina. Layout and
  // mouse events stay in logical points; only the renderer's output
  // surface is high-resolution.
  win_flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
  SDL_Window *win =
      SDL_CreateWindow("audiovisualizer", app->window_width, app->window_height, win_flags);
  if (!win)
  {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return false;
  }
  // Lower bound on the window size: keep the settings modal usable
  // (modal_w needs >= 420 + 2*pad) and stop the UI from collapsing into
  // an unreadable sliver. Height floor is intentionally low so the user
  // can squeeze the window into a thin strip if they want.
  SDL_SetWindowMinimumSize(win, 480, 240);
  // SDL disables the screensaver by default while a window is open. For a
  // visualizer that's wrong — we have no reason to inhibit display sleep or
  // the OS power-management daemon, so let the OS behave normally.
  SDL_EnableScreenSaver();

#ifdef __APPLE__
  // SDL3's default renderer on macOS is Metal, which puts a CAMetalLayer-
  // backed NSView on the window. That view hooks into the display
  // compositor in a way that fights the brightness daemon: while the app
  // is running, brightness adjustments jump and get stuck (most painfully,
  // turning brightness all the way down can lock it there until the app
  // quits). The software renderer is affected too because it presents
  // through the same CAMetalLayer.
  //
  // The OpenGL backend uses a legacy NSOpenGLView instead and doesn't
  // exhibit the bug. Our rendering is trivial enough (one streaming
  // texture + a few overlay primitives) that the perf difference is
  // negligible. OpenGL is deprecated on macOS but still ships in Tahoe;
  // revisit if Apple actually removes it.
  SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
  SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
  if (!ren)
  {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                       app->render_width, app->render_height);
  if (!tex)
  {
    fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
  // The spectrum texture is conceptually opaque: it fully owns the
  // framebuffer for the spectrum pass. We clear the pixel buffer to
  // 0x00000000 each frame, so its alpha channel is mostly 0; the default
  // ARGB8888 blend mode (BLEND) would let the renderer's clear color show
  // through those transparent pixels. Force NONE so the texture
  // overwrites everything, independent of whatever draw color/blend the
  // UI overlay left behind.
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);

  // On Retina macOS the window's backing store is at the display's native
  // pixel density (typically 2x logical). Tell the renderer to accept draw
  // calls in logical points and rasterize them at the physical resolution
  // — this is what lets the font atlas, baked at scale*size_px, land 1:1
  // on the physical pixel grid. On 1:1 displays (and on Windows/Linux,
  // where the HIGH_PIXEL_DENSITY flag isn't set) pixel density is 1.0 and
  // we leave the renderer in its default coordinate space.
  float pixel_scale = SDL_GetWindowPixelDensity(win);
  if (!(pixel_scale > 0.0f))
    pixel_scale = 1.0f;
  app->ui_pixel_scale = pixel_scale;
  if (pixel_scale > 1.0f)
  {
    SDL_SetRenderLogicalPresentation(ren, app->window_width, app->window_height,
                                     SDL_LOGICAL_PRESENTATION_STRETCH);
  }

  app->sdl_window = win;
  app->sdl_renderer = ren;
  app->sdl_texture = tex;
  return true;
}

static void shutdown_sdl(AppState *app)
{
  ui_text_shutdown();
  if (app->sdl_texture)
    SDL_DestroyTexture((SDL_Texture *)app->sdl_texture);
  if (app->sdl_renderer)
    SDL_DestroyRenderer((SDL_Renderer *)app->sdl_renderer);
  if (app->sdl_window)
    SDL_DestroyWindow((SDL_Window *)app->sdl_window);
  SDL_Quit();
}

// SDL_ShowOpenFileDialog callback. May fire on a worker thread, so we only
// strdup the path and post it through the event queue.
static void SDLCALL on_file_picked(void *userdata, const char *const *filelist, int filter)
{
  (void)userdata;
  (void)filter;
  SDL_Event ev;
  SDL_zero(ev);
  ev.type = g_file_picked_event;
  // filelist == NULL on dialog error; filelist[0] == NULL on user cancel.
  ev.user.data1 = (filelist && filelist[0]) ? SDL_strdup(filelist[0]) : NULL;
  SDL_PushEvent(&ev);
}

static void request_open_file_dialog(AppState *app)
{
  if (!app || app->file_picker_open)
    return;
  app->file_picker_open = true;
  // Single audio-files filter; semicolon-separated extensions per SDL3 docs.
  static const SDL_DialogFileFilter filters[] = {
      {"Audio files", "wav;mp3;flac;ogg;m4a;aac;aiff;opus"},
  };
  SDL_ShowOpenFileDialog(on_file_picked, app, (SDL_Window *)app->sdl_window, filters,
                         (int)(sizeof(filters) / sizeof(filters[0])), NULL, false);
}

// Tear down the current source and start one configured per
// app->requested_source_kind. On failure, populates app->ui_error_msg and
// leaves the app without an active source — the user can pick another
// option from the SOURCE dropdown. Returns true unconditionally; the
// callers treat any source error as a recoverable UI condition.
static bool apply_pending_source_change(AppState *app)
{
  if (!app->source_change_pending)
    return true;
  app->source_change_pending = false;

  const AudioSourceKind new_kind = app->requested_source_kind;

  // The FILE option in the dropdown opens the picker if we have nothing to
  // play yet; the actual source switch happens once a path is in hand.
  if (new_kind == AUDIO_SOURCE_FILE && app->current_file_path[0] == '\0')
  {
    request_open_file_dialog(app);
    return true;
  }

  if (app->source)
  {
    audio_source_stop(app->source);
    audio_source_destroy(app->source);
    app->source = NULL;
  }

  AudioSourceConfig new_cfg = app->source_config;
  new_cfg.kind = new_kind;
  new_cfg.file_path = (new_kind == AUDIO_SOURCE_FILE) ? app->current_file_path : NULL;
  new_cfg.ring = &app->ring;

  AudioSource *new_src = audio_source_create(&new_cfg);
  if (new_src && audio_source_start(new_src))
  {
    app->source = new_src;
    app->source_config = new_cfg;
    app->ui_error_msg[0] = '\0';
    const uint32_t actual_rate = audio_source_sample_rate(app->source);
    if (actual_rate != 0 && actual_rate != app->analyzer_config.sample_rate)
    {
      app->analyzer_config.sample_rate = actual_rate;
      app->analyzer_config_dirty = true;
    }
    fprintf(stdout, "audio: switched to %s @ %u Hz\n", audio_source_name(app->source), actual_rate);
    fflush(stdout);
    return true;
  }
  if (new_src)
    audio_source_destroy(new_src);

  const char *msg = "Audio source failed to start.";
  switch (new_kind)
  {
  case AUDIO_SOURCE_MIC:
    msg = "Microphone access denied or unavailable. Enable it in "
          "System Settings > Privacy & Security > Microphone, then try again.";
    break;
  case AUDIO_SOURCE_SYSTEM:
    msg = "System audio capture failed. Check that a system audio "
          "device is available and that the app has permission to record it.";
    break;
  case AUDIO_SOURCE_FILE:
    msg = "Could not open the selected audio file. Pick another file from the SOURCE dropdown.";
    // Clear the stale path so reselecting FILE from the dropdown opens
    // the picker again (write_value gates the picker on an empty path,
    // and a same-kind reselection is otherwise a no-op).
    app->current_file_path[0] = '\0';
    break;
  case AUDIO_SOURCE_DEMO:
    msg = "Could not load the bundled demo song.";
    break;
  }
  snprintf(app->ui_error_msg, sizeof(app->ui_error_msg), "%s", msg);
  fprintf(stderr, "audio: %s\n", msg);
  // Leave the app with no active source; the user can pick another option
  // from the SOURCE dropdown. The settings modal surfaces ui_error_msg so
  // the failure isn't silent.
  return true;
}

static void handle_events(AppState *app)
{
  SDL_Event ev;
  while (SDL_PollEvent(&ev))
  {
    // The settings modal gets first dibs at events: it consumes its own
    // toggle key, slider drags, and Escape-to-close while open. Escape
    // with no modal open is intentionally a no-op so an accidental tap
    // can't quit the app while audio is playing.
    if (controls_handle_event(&app->controls, &ev, app))
      continue;

    if (ev.type == SDL_EVENT_QUIT)
    {
      app->running = false;
    }
    else if (ev.type == SDL_EVENT_WINDOW_RESIZED)
    {
      // Coalesce multiple resize events per frame into a single pending
      // size; apply_pending_resize() handles reallocation before the next
      // draw.
      const int w = ev.window.data1;
      const int h = ev.window.data2;
      if (w > 0 && h > 0)
      {
        app->pending_window_width = w;
        app->pending_window_height = h;
        app->resize_pending = true;
      }
    }
    else if (ev.type == SDL_EVENT_MOUSE_MOTION)
    {
      app->mouse_x = ev.motion.x;
      app->mouse_y = ev.motion.y;
      app->mouse_in_window = true;
    }
    else if (ev.type == SDL_EVENT_WINDOW_MOUSE_ENTER)
    {
      app->mouse_in_window = true;
    }
    else if (ev.type == SDL_EVENT_WINDOW_MOUSE_LEAVE)
    {
      app->mouse_in_window = false;
    }
    else if (g_file_picked_event != 0 && ev.type == g_file_picked_event)
    {
      // Async result from SDL_ShowOpenFileDialog. ev.user.data1 is the
      // picked path (owned by us, free with SDL_free) or NULL on cancel.
      app->file_picker_open = false;
      char *path = (char *)ev.user.data1;
      if (path && path[0])
      {
        snprintf(app->current_file_path, sizeof(app->current_file_path), "%s", path);
        app->requested_source_kind = AUDIO_SOURCE_FILE;
        app->source_change_pending = true;
        app->ui_error_msg[0] = '\0';
      }
      // On cancel/error we leave source_config.kind untouched; the SOURCE
      // dropdown reads from it directly so it just reverts visually.
      if (path)
        SDL_free(path);
    }
  }
}

// Reallocates the supersampled pixel buffer + SDL streaming texture +
// renderer column scratch to match the latest window size. Returns false
// if any allocation fails, in which case the caller should treat the app
// as no longer renderable.
static bool apply_pending_resize(AppState *app)
{
  if (!app->resize_pending)
    return true;
  app->resize_pending = false;

  const int new_window_w = app->pending_window_width;
  const int new_window_h = app->pending_window_height;
  if (new_window_w <= 0 || new_window_h <= 0)
    return true;
  if (new_window_w == app->window_width && new_window_h == app->window_height)
    return true;

  const int new_render_w = new_window_w * RENDER_SCALE;
  const int new_render_h = new_window_h * RENDER_SCALE;

  pixel_buffer_destroy(&app->pixel_buffer);
  if (!pixel_buffer_init(&app->pixel_buffer, new_render_w, new_render_h))
  {
    fprintf(stderr, "resize: pixel_buffer_init(%dx%d) failed\n", new_render_w, new_render_h);
    return false;
  }

  if (app->sdl_texture)
    SDL_DestroyTexture((SDL_Texture *)app->sdl_texture);
  SDL_Texture *tex = SDL_CreateTexture((SDL_Renderer *)app->sdl_renderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING, new_render_w, new_render_h);
  if (!tex)
  {
    fprintf(stderr, "resize: SDL_CreateTexture(%dx%d) failed: %s\n", new_render_w, new_render_h,
            SDL_GetError());
    app->sdl_texture = NULL;
    return false;
  }
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
  app->sdl_texture = tex;

  if (!renderer_resize(&app->renderer, new_render_w))
  {
    fprintf(stderr, "resize: renderer_resize(%d) failed\n", new_render_w);
    return false;
  }

  app->window_width = new_window_w;
  app->window_height = new_window_h;
  app->render_width = new_render_w;
  app->render_height = new_render_h;

  // Re-query pixel density: dragging the window between a Retina laptop
  // panel and a 1x external monitor changes it. Refresh logical
  // presentation (logical size follows the new window size) and rebake
  // font atlases if the scale changed.
  float scale = SDL_GetWindowPixelDensity((SDL_Window *)app->sdl_window);
  if (!(scale > 0.0f))
    scale = 1.0f;
  if (scale > 1.0f)
  {
    SDL_SetRenderLogicalPresentation((SDL_Renderer *)app->sdl_renderer, new_window_w, new_window_h,
                                     SDL_LOGICAL_PRESENTATION_STRETCH);
  }
  else
  {
    SDL_SetRenderLogicalPresentation((SDL_Renderer *)app->sdl_renderer, 0, 0,
                                     SDL_LOGICAL_PRESENTATION_DISABLED);
  }
  app->ui_pixel_scale = scale;
  ui_text_set_pixel_scale(scale);
  return true;
}

// One frame of the main loop, minus event handling. Pulls fresh PCM,
// runs FFT/waveform analysis, composites the pixel buffer, and presents.
// Factored out so it can also be driven from a resize event watcher (on
// macOS, AppKit runs a modal tracking loop during live window resize
// that blocks the main loop's SDL_PollEvent; calling draw_frame from
// SDL_AddEventWatch keeps the visualizer ticking through the resize).
static void draw_frame(AppState *app)
{
  if (app->analyzer_config_dirty)
  {
    if (!analyzer_reconfigure(&app->analyzer, &app->analyzer_config))
    {
      fprintf(stderr, "analyzer_reconfigure failed; bailing out\n");
      app->running = false;
      return;
    }
    free(app->analysis_samples);
    app->analysis_samples = (float *)calloc(app->analyzer_config.fft_size, sizeof(float));
    if (!app->analysis_samples)
    {
      app->running = false;
      return;
    }
    app->analyzer_config_dirty = false;
  }

  if (pcm_ring_copy_latest(&app->ring, app->analysis_samples, app->analyzer_config.fft_size))
  {
    analyzer_process(&app->analyzer, app->analysis_samples);
  }
  if (app->visualizer_config.show_waveform && app->waveform_samples)
  {
    pcm_ring_copy_latest(&app->ring, app->waveform_samples, app->waveform_size);
  }

  // Periodic peak-bin diagnostic. Static state so the cadence is shared
  // whether we're driven by the main loop or the resize watcher.
  static uint64_t last_diag_ms = 0;
  const uint64_t now_ms = SDL_GetTicks();
  if (now_ms - last_diag_ms >= 1500)
  {
    const AnalyzerOutput *o = analyzer_output(&app->analyzer);
    const int K = 3, SUPP = 3;
    uint32_t top[3] = {0, 0, 0};
    float topv[3] = {-1e30f, -1e30f, -1e30f};
    for (int k = 0; k < K; ++k)
    {
      for (uint32_t i = 1; i < o->bin_count; ++i)
      {
        bool blocked = false;
        for (int j = 0; j < k && !blocked; ++j)
        {
          int d = (int)i - (int)top[j];
          if (d < 0)
            d = -d;
          if (d <= SUPP)
            blocked = true;
        }
        if (blocked)
          continue;
        const float v = o->db[i];
        if (v > topv[k])
        {
          topv[k] = v;
          top[k] = i;
        }
      }
    }
    const float hz_per_bin =
        (float)app->analyzer_config.sample_rate / (float)app->analyzer_config.fft_size;
    fprintf(stdout, "[fft] peaks: ");
    for (int k = 0; k < K; ++k)
    {
      const float freq = (float)top[k] * hz_per_bin;
      fprintf(stdout, "bin=%u(%.1fHz, db=%.1f, e=%.2f)%s", top[k], (double)freq,
              (double)o->db[top[k]], (double)o->smoothed_energy[top[k]], k < K - 1 ? "  " : "\n");
    }
    fflush(stdout);
    last_diag_ms = now_ms;
  }

  // Clear once, then composite each enabled view into the same buffer.
  // When both are on the spectrum sits behind the waveform; when only
  // one is enabled it occupies the full canvas naturally.
  pixel_buffer_clear(&app->pixel_buffer, 0);
  if (app->visualizer_config.show_spectrum)
  {
    renderer_draw_spectrum(&app->renderer, &app->pixel_buffer, analyzer_output(&app->analyzer),
                           analyzer_config(&app->analyzer), &app->visualizer_config);
  }
  if (app->visualizer_config.show_waveform && app->waveform_samples)
  {
    renderer_draw_waveform(&app->pixel_buffer, app->waveform_samples, (int)app->waveform_size,
                           &app->visualizer_config);
  }

  SDL_UpdateTexture((SDL_Texture *)app->sdl_texture, NULL, app->pixel_buffer.pixels,
                    app->pixel_buffer.stride);
  SDL_RenderClear((SDL_Renderer *)app->sdl_renderer);
  SDL_RenderTexture((SDL_Renderer *)app->sdl_renderer, (SDL_Texture *)app->sdl_texture, NULL, NULL);
  overlay_render((SDL_Renderer *)app->sdl_renderer, app);
  controls_render(&app->controls, (SDL_Renderer *)app->sdl_renderer, app);
  SDL_RenderPresent((SDL_Renderer *)app->sdl_renderer);
}

// Event watcher invoked synchronously from SDL_PumpEvents — including
// from the AppKit modal tracking loop that runs during macOS live window
// resize. We forward resize/expose events into the normal pending-resize
// path and then drive one extra frame so the spectrum keeps animating
// while the user is dragging the window edge.
static bool SDLCALL on_resize_watch(void *userdata, SDL_Event *ev)
{
  AppState *app = (AppState *)userdata;
  if (!app)
    return true;
  if (ev->type == SDL_EVENT_WINDOW_RESIZED || ev->type == SDL_EVENT_WINDOW_EXPOSED)
  {
    if (ev->type == SDL_EVENT_WINDOW_RESIZED)
    {
      const int w = ev->window.data1;
      const int h = ev->window.data2;
      if (w > 0 && h > 0)
      {
        app->pending_window_width = w;
        app->pending_window_height = h;
        app->resize_pending = true;
      }
    }
    if (apply_pending_resize(app))
      draw_frame(app);
  }
  return true;
}

static void cleanup(AppState *app)
{
  if (app->source)
  {
    audio_source_stop(app->source);
    audio_source_destroy(app->source);
    app->source = NULL;
  }
  renderer_destroy(&app->renderer);
  analyzer_destroy(&app->analyzer);
  free(app->analysis_samples);
  app->analysis_samples = NULL;
  free(app->waveform_samples);
  app->waveform_samples = NULL;
  if (app->sdl_window)
    shutdown_sdl(app);
  pcm_ring_destroy(&app->ring);
  pixel_buffer_destroy(&app->pixel_buffer);
}

int main(int argc, char **argv)
{
  CliArgs cli = {0};
  if (!parse_cli(argc, argv, &cli))
    return 2;

  AppState app = {0};
  app.running = true;

  // Persistable defaults. config_load() overwrites individual fields on top
  // of these; CLI flags then override the initial source. Render dimensions
  // and the audio-source completion fields (sample_rate/channels/ring) are
  // derived later, once window size and audio kind are final.
  app.window_width = DEFAULT_WIDTH;
  app.window_height = DEFAULT_HEIGHT;
  app.visualizer_config = visualizer_config_defaults();
  // Draw bars RENDER_SCALE render-pixels wide so that after the bilinear
  // 3:1 downsample each bar lands on exactly one display pixel
  app.visualizer_config.bar_width_px = RENDER_SCALE;
  app.analyzer_config = (AnalyzerConfig){
      .sample_rate = SAMPLE_RATE,
      .fft_size = FFT_SIZE,
      .min_db = DEFAULT_MIN_DB,
      .max_db = DEFAULT_MAX_DB,
      .smoothing = DEFAULT_SMOOTHING,
      .window_function = WINDOW_HANN,
  };
  // First-launch default: the bundled public-domain demo song. config_load
  // overrides this on subsequent launches with whatever the user last had
  // selected. The demo source doesn't auto-start when the hint banner is
  // visible — the user clicks PLAY DEMO SONG in the hint to begin.
  app.source_config.kind = AUDIO_SOURCE_DEMO;

  // Default the hint to visible. config_load() will overwrite this to false
  // if the user previously checked "Don't show again". The default must be
  // set before config_load so the persisted value isn't clobbered by the
  // controls_init() call that follows (which intentionally leaves show_hint
  // untouched so the persisted value survives).
  app.controls.show_hint = true;

  // Layer persisted settings on top of defaults. Missing config file is a
  // no-op — the app boots with built-in defaults on first launch.
  config_load(&app);

  // FILE is never restored across restarts: a renamed/moved/deleted file
  // would lock the app in an error state with no in-UI recovery (the
  // dropdown ignores reselecting the current kind). Force DEMO and wipe
  // the remembered path so each launch starts with a known-good source
  // and a clean file picker. Old settings.conf files from prior versions
  // are migrated here; config_save no longer writes either field.
  if (app.source_config.kind == AUDIO_SOURCE_FILE)
    app.source_config.kind = AUDIO_SOURCE_DEMO;
  app.current_file_path[0] = '\0';

  // CLI flags are initial-source overrides only (see handover.md). Without
  // an explicit flag we keep whatever config_load restored.
  if (cli.kind_set)
    app.source_config.kind = cli.kind;
  if (cli.file_path)
    snprintf(app.current_file_path, sizeof(app.current_file_path), "%s", cli.file_path);

  app.render_width = app.window_width * RENDER_SCALE;
  app.render_height = app.window_height * RENDER_SCALE;

  if (!pixel_buffer_init(&app.pixel_buffer, app.render_width, app.render_height))
  {
    fprintf(stderr, "pixel_buffer_init failed\n");
    return 1;
  }
  if (!pcm_ring_init(&app.ring, RING_CAPACITY))
  {
    fprintf(stderr, "pcm_ring_init failed\n");
    cleanup(&app);
    return 1;
  }
  if (!init_sdl(&app))
  {
    cleanup(&app);
    return 1;
  }

  if (!analyzer_init(&app.analyzer, &app.analyzer_config))
  {
    fprintf(stderr, "analyzer_init failed\n");
    cleanup(&app);
    return 1;
  }
  app.analysis_samples = (float *)calloc(app.analyzer_config.fft_size, sizeof(float));
  if (!app.analysis_samples)
  {
    fprintf(stderr, "analysis_samples alloc failed\n");
    cleanup(&app);
    return 1;
  }
  app.waveform_size = WAVEFORM_SIZE;
  app.waveform_samples = (float *)calloc(app.waveform_size, sizeof(float));
  if (!app.waveform_samples)
  {
    fprintf(stderr, "waveform_samples alloc failed\n");
    cleanup(&app);
    return 1;
  }
  if (!renderer_init(&app.renderer, app.render_width))
  {
    fprintf(stderr, "renderer_init failed\n");
    cleanup(&app);
    return 1;
  }
  controls_init(&app.controls);
  ui_text_set_pixel_scale(app.ui_pixel_scale);
  if (!ui_text_init((SDL_Renderer *)app.sdl_renderer))
    fprintf(stderr, "warn: no system font found; UI text disabled\n");

  // Register the SDL user event used by the file-picker callback to hand
  // the chosen path back to the main thread.
  g_file_picked_event = SDL_RegisterEvents(1);

  // Finalize source config from whatever survived config_load + CLI.
  app.source_config.file_path =
      (app.source_config.kind == AUDIO_SOURCE_FILE) ? app.current_file_path : NULL;
  app.source_config.sample_rate = SAMPLE_RATE;
  app.source_config.channels = 1;
  app.source_config.ring = &app.ring;

  // On first launch the hint banner is up and the default source is the
  // bundled demo. Don't auto-start audio in that case — we create the
  // source so it's ready to go, but wait for the user to click PLAY DEMO
  // SONG (which sets app.start_audio_request) before calling start.
  const bool defer_initial_start =
      (app.source_config.kind == AUDIO_SOURCE_DEMO) && app.controls.show_hint;

  app.source = audio_source_create(&app.source_config);
  if (!app.source || (!defer_initial_start && !audio_source_start(app.source)))
  {
    // A persisted source (mic with revoked permission, file deleted, etc.)
    // shouldn't brick startup. Tear it down and leave the app sourceless;
    // the user picks another option from the SOURCE dropdown.
    if (app.source)
    {
      audio_source_destroy(app.source);
      app.source = NULL;
    }
    fprintf(stderr, "audio: initial source failed; no audio active\n");
    snprintf(app.ui_error_msg, sizeof(app.ui_error_msg),
             "Saved audio source failed to start. Open Settings (Tab) "
             "and pick another source.");
  }

  // The system audio tap (and potentially other backends) negotiate their
  // own sample rate with the OS mixer rather than honoring our requested
  // rate. Realign the analyzer to whatever the source actually produces so
  // bin->frequency mapping (and therefore semitone X positions) stay
  // correct.
  if (app.source)
  {
    const uint32_t actual_rate = audio_source_sample_rate(app.source);
    if (actual_rate != 0 && actual_rate != app.analyzer_config.sample_rate)
    {
      app.analyzer_config.sample_rate = actual_rate;
      if (!analyzer_reconfigure(&app.analyzer, &app.analyzer_config))
      {
        fprintf(stderr, "analyzer_reconfigure(sample_rate=%u) failed\n", actual_rate);
        cleanup(&app);
        return 1;
      }
    }

    fprintf(stdout, "audio: %s @ %u Hz, %u ch | fft=%u (%.1f Hz/bin, %.1f ms window) | %s%s\n",
            audio_source_name(app.source), audio_source_sample_rate(app.source),
            audio_source_channels(app.source), app.analyzer_config.fft_size,
            (double)app.analyzer_config.sample_rate / (double)app.analyzer_config.fft_size,
            1000.0 * (double)app.analyzer_config.fft_size / (double)app.analyzer_config.sample_rate,
            window_fn_name(app.analyzer_config.window_function),
            defer_initial_start ? " (waiting for PLAY DEMO SONG)" : "");
    fflush(stdout);
  }

  // Register the resize watcher only after every subsystem the frame
  // touches (analyzer, renderer, audio source) is initialized — the
  // watcher can fire as soon as it's installed.
  SDL_AddEventWatch(on_resize_watch, &app);

  while (app.running)
  {
    handle_events(&app);

    if (!apply_pending_resize(&app))
    {
      app.running = false;
      break;
    }

    if (!apply_pending_source_change(&app))
    {
      app.running = false;
      break;
    }

    if (app.open_file_picker_request)
    {
      app.open_file_picker_request = false;
      request_open_file_dialog(&app);
    }

    if (app.start_audio_request)
    {
      app.start_audio_request = false;
      if (app.source && !audio_source_is_running(app.source))
      {
        if (!audio_source_start(app.source))
        {
          fprintf(stderr, "audio: start failed for deferred source\n");
          snprintf(app.ui_error_msg, sizeof(app.ui_error_msg),
                   "Could not start the bundled demo song.");
        }
      }
    }

    draw_frame(&app);
  }

  SDL_RemoveEventWatch(on_resize_watch, &app);
  // Persist user-modifiable settings on graceful exit. Failures are
  // non-fatal — the next launch will just fall back to defaults.
  config_save(&app);
  cleanup(&app);
  return 0;
}
