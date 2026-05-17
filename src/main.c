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
#include "render/pixel_buffer.h"
#include "render/renderer.h"
#include "ui/controls.h"
#include "ui/overlay.h"
#include "ui/text.h"

// Phase 4: SDL window + audio source (test-tone or file via miniaudio) +
// ring buffer + FFTW analyzer + log-semitone bar renderer.

typedef struct CliArgs
{
  AudioSourceKind kind;
  const char *file_path;
} CliArgs;

static void print_usage(const char *prog)
{
  fprintf(stderr,
          "usage: %s [--test-tone | --file PATH | --mic | --system]\n"
          "  default: --system\n",
          prog);
}

static bool parse_cli(int argc, char **argv, CliArgs *out)
{
  out->kind = AUDIO_SOURCE_SYSTEM;
  out->file_path = NULL;
  for (int i = 1; i < argc; ++i)
  {
    const char *a = argv[i];
    if (strcmp(a, "--test-tone") == 0)
    {
      out->kind = AUDIO_SOURCE_TEST_TONE;
    }
    else if (strcmp(a, "--file") == 0)
    {
      if (i + 1 >= argc)
      {
        fprintf(stderr, "--file requires a path argument\n");
        return false;
      }
      out->kind = AUDIO_SOURCE_FILE;
      out->file_path = argv[++i];
    }
    else if (strcmp(a, "--mic") == 0)
    {
      out->kind = AUDIO_SOURCE_MIC;
    }
    else if (strcmp(a, "--system") == 0)
    {
      out->kind = AUDIO_SOURCE_SYSTEM;
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

static void handle_events(AppState *app)
{
  SDL_Event ev;
  while (SDL_PollEvent(&ev))
  {
    // The settings modal gets first dibs at events: it consumes its own
    // toggle key, slider drags, and Escape-to-close while open.
    if (controls_handle_event(&app->controls, &ev, app))
      continue;

    if (ev.type == SDL_EVENT_QUIT)
    {
      app->running = false;
    }
    else if (ev.type == SDL_EVENT_KEY_DOWN)
    {
      if (ev.key.key == SDLK_ESCAPE)
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
  app.window_width = DEFAULT_WIDTH;
  app.window_height = DEFAULT_HEIGHT;
  app.render_width = DEFAULT_WIDTH * RENDER_SCALE;
  app.render_height = DEFAULT_HEIGHT * RENDER_SCALE;
  app.visualizer_config = visualizer_config_defaults();
  // Draw bars RENDER_SCALE render-pixels wide so that after the bilinear
  // 3:1 downsample each bar lands on exactly one display pixel
  app.visualizer_config.bar_width_px = RENDER_SCALE;

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

  app.analyzer_config = (AnalyzerConfig){
      .sample_rate = SAMPLE_RATE,
      .fft_size = FFT_SIZE,
      .min_db = DEFAULT_MIN_DB,
      .max_db = DEFAULT_MAX_DB,
      .smoothing = DEFAULT_SMOOTHING,
      .window_function = WINDOW_HANN,
  };
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

  app.source_config = (AudioSourceConfig){
      .kind = cli.kind,
      .file_path = cli.file_path,
      .sample_rate = SAMPLE_RATE,
      .channels = 1,
      .ring = &app.ring,
  };
  app.source = audio_source_create(&app.source_config);
  if (!app.source || !audio_source_start(app.source))
  {
    fprintf(stderr, "audio source failed to start\n");
    cleanup(&app);
    return 1;
  }

  // The system audio tap (and potentially other backends) negotiate their
  // own sample rate with the OS mixer rather than honoring our requested
  // rate. Realign the analyzer to whatever the source actually produces so
  // bin->frequency mapping (and therefore semitone X positions) stay
  // correct.
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

  fprintf(stdout, "audio: %s @ %u Hz, %u ch | fft=%u (%.1f Hz/bin, %.1f ms window) | %s\n",
          audio_source_name(app.source), audio_source_sample_rate(app.source),
          audio_source_channels(app.source), app.analyzer_config.fft_size,
          (double)app.analyzer_config.sample_rate / (double)app.analyzer_config.fft_size,
          1000.0 * (double)app.analyzer_config.fft_size / (double)app.analyzer_config.sample_rate,
          window_fn_name(app.analyzer_config.window_function));
  fflush(stdout);

  uint64_t last_diag_ms = SDL_GetTicks();

  while (app.running)
  {
    handle_events(&app);

    if (!apply_pending_resize(&app))
    {
      app.running = false;
      break;
    }

    if (app.analyzer_config_dirty)
    {
      if (!analyzer_reconfigure(&app.analyzer, &app.analyzer_config))
      {
        fprintf(stderr, "analyzer_reconfigure failed; bailing out\n");
        app.running = false;
        break;
      }
      free(app.analysis_samples);
      app.analysis_samples = (float *)calloc(app.analyzer_config.fft_size, sizeof(float));
      if (!app.analysis_samples)
      {
        app.running = false;
        break;
      }
      app.analyzer_config_dirty = false;
    }

    if (pcm_ring_copy_latest(&app.ring, app.analysis_samples, app.analyzer_config.fft_size))
    {
      analyzer_process(&app.analyzer, app.analysis_samples);
    }
    if (app.visualizer_config.show_waveform && app.waveform_samples)
    {
      pcm_ring_copy_latest(&app.ring, app.waveform_samples, app.waveform_size);
    }

    const uint64_t now_ms = SDL_GetTicks();
    if (now_ms - last_diag_ms >= 1500)
    {
      const AnalyzerOutput *o = analyzer_output(&app.analyzer);
      // Find 3 distinct peaks by suppressing a ±3-bin window after each pick.
      // Rank by raw dB; energy[] saturates inside loud lobes so it's not a
      // reliable discriminator for "loudest bin in this lobe".
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
          (float)app.analyzer_config.sample_rate / (float)app.analyzer_config.fft_size;
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
    pixel_buffer_clear(&app.pixel_buffer, 0);
    if (app.visualizer_config.show_spectrum)
    {
      renderer_draw_spectrum(&app.renderer, &app.pixel_buffer, analyzer_output(&app.analyzer),
                             analyzer_config(&app.analyzer), &app.visualizer_config);
    }
    if (app.visualizer_config.show_waveform && app.waveform_samples)
    {
      renderer_draw_waveform(&app.pixel_buffer, app.waveform_samples, (int)app.waveform_size,
                             &app.visualizer_config);
    }

    SDL_UpdateTexture((SDL_Texture *)app.sdl_texture, NULL, app.pixel_buffer.pixels,
                      app.pixel_buffer.stride);
    SDL_RenderClear((SDL_Renderer *)app.sdl_renderer);
    SDL_RenderTexture((SDL_Renderer *)app.sdl_renderer, (SDL_Texture *)app.sdl_texture, NULL, NULL);
    overlay_render((SDL_Renderer *)app.sdl_renderer, &app);
    controls_render(&app.controls, (SDL_Renderer *)app.sdl_renderer, &app);
    SDL_RenderPresent((SDL_Renderer *)app.sdl_renderer);
  }

  cleanup(&app);
  return 0;
}
