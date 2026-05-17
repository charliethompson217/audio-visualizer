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

#include "controls.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "app_state.h"
#include "text.h"
#include "../analyzer/window_fn.h"
#include "../config.h"

// Version shown in the About panel. Keep in sync with Info.plist
// CFBundleShortVersionString.
#define APP_VERSION_STRING "0.1"
#define APP_GITHUB_URL "https://github.com/charliethompson217/audio-visualizer"

// Third-party library homepages shown in the About panel.
#define LIB_SDL3_URL "https://github.com/libsdl-org/SDL"
#define LIB_FFTW3_URL "https://www.fftw.org"
#define LIB_MINIAUDIO_URL "https://miniaud.io"
#define LIB_STB_TT_URL "https://github.com/nothings/stb"

// ---------------------------------------------------------------------------
// Slider table. Each entry binds a label to a field in AppState. Discrete
// sliders (fft_size) snap to one of `discrete` values; the slider position
// is the index into that list.
// ---------------------------------------------------------------------------

typedef enum SliderId
{
  // Source selection rows live at the top of the panel. BROWSE_FILE and
  // SOURCE_ERROR are conditionally visible (see row_visible() below).
  SID_SOURCE = 0,
  SID_BROWSE_FILE,
  SID_SOURCE_ERROR,
  SID_SHOW_SPECTRUM,
  SID_SHOW_WAVEFORM,
  SID_WAVEFORM_V_SCALE,
  SID_WAVEFORM_H_SCALE,
  SID_BRIGHTNESS_POWER,
  SID_LENGTH_POWER,
  SID_MIN_SEMITONE,
  SID_MAX_SEMITONE,
  SID_SHOW_LABELS,
  SID_SHOW_CURSOR,
  SID_FFT_SIZE,
  SID_WINDOW_FUNCTION,
  SID_MIN_DB,
  SID_MAX_DB,
  SID_SMOOTHING,
  SID_ABOUT,
  SID_COUNT,
} SliderId;

typedef enum SliderKind
{
  SK_FLOAT,
  SK_BOOL,
  SK_DROPDOWN_UINT,
  // Clickable row that fires an action (no associated value). Currently
  // used for the BROWSE FILE… row.
  SK_BUTTON,
  // Read-only row that renders a dynamic text label (no widget). Used for
  // the in-modal source error message.
  SK_LABEL,
} SliderKind;

typedef struct SliderDef
{
  const char *label;
  SliderKind kind;
  float min;
  float max;
  const uint32_t *discrete;
  int discrete_count;
  int decimals; // for SK_FLOAT value display
  // When true, the slider position maps to its value on a log scale so
  // ranges that span several orders of magnitude (e.g. 0.25..128) stay
  // usable. Requires min > 0.
  bool logarithmic;
  // Optional display labels for SK_DROPDOWN_UINT. When non-NULL, each
  // entry corresponds to the discrete[] value at the same index and is
  // shown instead of the raw integer.
  const char **labels;
} SliderDef;

static const uint32_t FFT_SIZES[] = {512, 1024, 2048, 4096, 8192, 16384, 32768};

static const uint32_t WINDOW_FN_VALUES[] = {WINDOW_RECTANGULAR, WINDOW_HANN, WINDOW_HAMMING,
                                            WINDOW_BLACKMAN};
static const char *WINDOW_FN_LABELS[] = {"RECTANGULAR", "HANN", "HAMMING", "BLACKMAN"};

static const uint32_t SOURCE_KINDS[] = {AUDIO_SOURCE_DEMO, AUDIO_SOURCE_FILE, AUDIO_SOURCE_MIC,
                                        AUDIO_SOURCE_SYSTEM};
static const char *SOURCE_LABELS[] = {"DEMO SONG", "FILE", "MIC", "SYSTEM"};

static const SliderDef SLIDERS[SID_COUNT] = {
    [SID_SOURCE] = {"SOURCE", SK_DROPDOWN_UINT, 0.0f, 0.0f, SOURCE_KINDS,
                    (int)(sizeof(SOURCE_KINDS) / sizeof(SOURCE_KINDS[0])), 0, false, SOURCE_LABELS},
    [SID_BROWSE_FILE] = {"FILE", SK_BUTTON, 0.0f, 0.0f, NULL, 0, 0, false, NULL},
    [SID_SOURCE_ERROR] = {"", SK_LABEL, 0.0f, 0.0f, NULL, 0, 0, false, NULL},
    [SID_SHOW_SPECTRUM] = {"SHOW SPECTRUM", SK_BOOL, 0.0f, 1.0f, NULL, 0, 0, false, NULL},
    [SID_SHOW_WAVEFORM] = {"SHOW WAVEFORM", SK_BOOL, 0.0f, 1.0f, NULL, 0, 0, false, NULL},
    [SID_WAVEFORM_V_SCALE] = {"WAVEFORM V SCALE", SK_FLOAT, 0.1f, 10.0f, NULL, 0, 2, false, NULL},
    // Real-units horizontal zoom in samples per display pixel: 1.00 is the
    // 1:1 "pure data" view; <1 zooms in (multiple pixels per sample), >1
    // zooms out (more samples per pixel). Log-scaled so the useful low
    // end isn't crammed against the left edge of the track.
    [SID_WAVEFORM_H_SCALE] = {"WAVEFORM SAMPLES/PX", SK_FLOAT, 0.25f, 128.0f, NULL, 0, 2, true,
                              NULL},
    [SID_BRIGHTNESS_POWER] = {"BRIGHTNESS POWER", SK_FLOAT, 0.1f, 3.0f, NULL, 0, 2, false, NULL},
    [SID_LENGTH_POWER] = {"LENGTH POWER", SK_FLOAT, 0.1f, 3.0f, NULL, 0, 2, false, NULL},
    [SID_MIN_SEMITONE] = {"MIN SEMITONE", SK_FLOAT, 0.0f, 120.0f, NULL, 0, 0, false, NULL},
    [SID_MAX_SEMITONE] = {"MAX SEMITONE", SK_FLOAT, 0.0f, 132.0f, NULL, 0, 0, false, NULL},
    [SID_SHOW_LABELS] = {"SHOW LABELS", SK_BOOL, 0.0f, 1.0f, NULL, 0, 0, false, NULL},
    [SID_SHOW_CURSOR] = {"SHOW CURSOR", SK_BOOL, 0.0f, 1.0f, NULL, 0, 0, false, NULL},
    [SID_FFT_SIZE] = {"FFT SIZE", SK_DROPDOWN_UINT, 0.0f, 0.0f, FFT_SIZES,
                      (int)(sizeof(FFT_SIZES) / sizeof(FFT_SIZES[0])), 0, false, NULL},
    [SID_WINDOW_FUNCTION] = {"WINDOW FUNCTION", SK_DROPDOWN_UINT, 0.0f, 0.0f, WINDOW_FN_VALUES,
                             (int)(sizeof(WINDOW_FN_VALUES) / sizeof(WINDOW_FN_VALUES[0])), 0,
                             false, WINDOW_FN_LABELS},
    [SID_MIN_DB] = {"MIN DB", SK_FLOAT, -150.0f, 0.0f, NULL, 0, 0, false, NULL},
    [SID_MAX_DB] = {"MAX DB", SK_FLOAT, -150.0f, 0.0f, NULL, 0, 0, false, NULL},
    [SID_SMOOTHING] = {"SMOOTHING", SK_FLOAT, 0.0f, 0.99f, NULL, 0, 2, false, NULL},
    [SID_ABOUT] = {"ABOUT", SK_BUTTON, 0.0f, 0.0f, NULL, 0, 0, false, NULL},
};

// ---------------------------------------------------------------------------
// Value accessors. Sliders read/write directly into AppState. Cheap analyzer
// fields (min_db/max_db/smoothing) are mirrored into the live analyzer
// config so changes take effect without rebuilding the FFTW plan. Changing
// fft_size sets analyzer_config_dirty so the main loop performs a full
// reconfigure on the next iteration.
// ---------------------------------------------------------------------------

static float clamp_f(float v, float lo, float hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

static float read_value(const AppState *app, int id)
{
  switch (id)
  {
  case SID_SOURCE:
    return (float)app->source_config.kind;
  case SID_SHOW_SPECTRUM:
    return app->visualizer_config.show_spectrum ? 1.0f : 0.0f;
  case SID_SHOW_WAVEFORM:
    return app->visualizer_config.show_waveform ? 1.0f : 0.0f;
  case SID_WAVEFORM_V_SCALE:
    return app->visualizer_config.waveform_gain;
  case SID_WAVEFORM_H_SCALE:
    return app->visualizer_config.waveform_samples_per_px;
  case SID_BRIGHTNESS_POWER:
    return app->visualizer_config.brightness_power;
  case SID_LENGTH_POWER:
    return app->visualizer_config.length_power;
  case SID_MIN_SEMITONE:
    return app->visualizer_config.min_semitone;
  case SID_MAX_SEMITONE:
    return app->visualizer_config.max_semitone;
  case SID_SHOW_LABELS:
    return app->visualizer_config.show_labels ? 1.0f : 0.0f;
  case SID_SHOW_CURSOR:
    return app->visualizer_config.show_cursor ? 1.0f : 0.0f;
  case SID_FFT_SIZE:
    return (float)app->analyzer_config.fft_size;
  case SID_WINDOW_FUNCTION:
    return (float)app->analyzer_config.window_function;
  case SID_MIN_DB:
    return app->analyzer_config.min_db;
  case SID_MAX_DB:
    return app->analyzer_config.max_db;
  case SID_SMOOTHING:
    return app->analyzer_config.smoothing;
  default:
    return 0.0f;
  }
}

static void write_value(AppState *app, int id, float v)
{
  switch (id)
  {
  case SID_SOURCE:
  {
    // The UI only stages a request; the main loop tears down + restarts
    // the audio source on the next iteration. Re-selecting FILE without a
    // path opens the file picker (apply_pending_source_change handles
    // that branch).
    const AudioSourceKind new_kind = (AudioSourceKind)(uint32_t)v;
    const bool need_file_pick =
        (new_kind == AUDIO_SOURCE_FILE && app->current_file_path[0] == '\0');
    if (new_kind != app->source_config.kind || need_file_pick)
    {
      app->requested_source_kind = new_kind;
      app->source_change_pending = true;
    }
    break;
  }
  case SID_SHOW_SPECTRUM:
    app->visualizer_config.show_spectrum = (v >= 0.5f);
    break;
  case SID_SHOW_WAVEFORM:
    app->visualizer_config.show_waveform = (v >= 0.5f);
    break;
  case SID_WAVEFORM_V_SCALE:
    app->visualizer_config.waveform_gain = v;
    break;
  case SID_WAVEFORM_H_SCALE:
    app->visualizer_config.waveform_samples_per_px = v;
    break;
  case SID_BRIGHTNESS_POWER:
    app->visualizer_config.brightness_power = v;
    break;
  case SID_LENGTH_POWER:
    app->visualizer_config.length_power = v;
    break;
  case SID_MIN_SEMITONE:
    app->visualizer_config.min_semitone = v;
    break;
  case SID_MAX_SEMITONE:
    app->visualizer_config.max_semitone = v;
    break;
  case SID_SHOW_LABELS:
    app->visualizer_config.show_labels = (v >= 0.5f);
    break;
  case SID_SHOW_CURSOR:
    app->visualizer_config.show_cursor = (v >= 0.5f);
    break;
  case SID_FFT_SIZE:
  {
    const uint32_t n = (uint32_t)v;
    if (n != app->analyzer_config.fft_size)
    {
      app->analyzer_config.fft_size = n;
      app->analyzer_config_dirty = true;
    }
    break;
  }
  case SID_WINDOW_FUNCTION:
  {
    const WindowFunction wf = (WindowFunction)(uint32_t)v;
    if (wf != app->analyzer_config.window_function)
    {
      app->analyzer_config.window_function = wf;
      app->analyzer_config_dirty = true;
    }
    break;
  }
  case SID_MIN_DB:
    app->analyzer_config.min_db = v;
    app->analyzer.config.min_db = v;
    break;
  case SID_MAX_DB:
    app->analyzer_config.max_db = v;
    app->analyzer.config.max_db = v;
    break;
  case SID_SMOOTHING:
    app->analyzer_config.smoothing = v;
    app->analyzer.config.smoothing = v;
    break;
  default:
    break;
  }
}

// 0..1 slider position -> value (continuous SK_FLOAT only). Log-scaled
// sliders interpolate min/max in log-space so several decades of range
// stay readable; linear sliders fall through to the obvious mapping.
static float pos_to_value(const SliderDef *def, float t)
{
  t = clamp_f(t, 0.0f, 1.0f);
  if (def->logarithmic && def->min > 0.0f && def->max > def->min)
  {
    const float lo = logf(def->min);
    const float hi = logf(def->max);
    return expf(lo + t * (hi - lo));
  }
  return def->min + t * (def->max - def->min);
}

static float value_to_pos(const SliderDef *def, float v)
{
  if (def->logarithmic && def->min > 0.0f && def->max > def->min && v > 0.0f)
  {
    const float lo = logf(def->min);
    const float hi = logf(def->max);
    return clamp_f((logf(v) - lo) / (hi - lo), 0.0f, 1.0f);
  }
  const float range = def->max - def->min;
  if (range <= 0.0f)
    return 0.0f;
  return clamp_f((v - def->min) / range, 0.0f, 1.0f);
}

// Index in the discrete list closest to `v`. -1 if list is empty.
static int discrete_index_for_value(const SliderDef *def, float v)
{
  const int n = def->discrete_count;
  if (n <= 0 || !def->discrete)
    return -1;
  const uint32_t uv = (uint32_t)v;
  int best = 0;
  for (int i = 1; i < n; ++i)
  {
    const uint32_t a = def->discrete[i];
    const uint32_t b = def->discrete[best];
    const uint32_t da = a > uv ? a - uv : uv - a;
    const uint32_t db = b > uv ? b - uv : uv - b;
    if (da < db)
      best = i;
  }
  return best;
}

static void format_value(const SliderDef *def, float v, char *out, size_t cap)
{
  if (def->kind == SK_DROPDOWN_UINT)
  {
    if (def->labels)
    {
      const int idx = discrete_index_for_value(def, v);
      if (idx >= 0 && idx < def->discrete_count)
      {
        snprintf(out, cap, "%s", def->labels[idx]);
        return;
      }
    }
    snprintf(out, cap, "%u", (unsigned)v);
    return;
  }
  if (def->kind == SK_BOOL)
  {
    snprintf(out, cap, "%s", v >= 0.5f ? "ON" : "OFF");
    return;
  }
  switch (def->decimals)
  {
  case 0:
    snprintf(out, cap, "%d", (int)(v >= 0.0f ? v + 0.5f : v - 0.5f));
    break;
  case 1:
    snprintf(out, cap, "%.1f", (double)v);
    break;
  default:
    snprintf(out, cap, "%.2f", (double)v);
    break;
  }
}

// ---------------------------------------------------------------------------
// Layout. The modal is centered in the window with fixed padding and a
// per-slider row height. Track + value-text geometry is recomputed each
// frame from the current window size so resizes don't need extra state.
// ---------------------------------------------------------------------------

typedef struct Layout
{
  int modal_x, modal_y, modal_w, modal_h;
  int row_h;
  // Pre-computed top-of-row y for each SliderId, post-scroll. -1 means
  // the row is hidden (filtered by row_visible) and widget rects must not
  // be computed for it. Visible rows are packed contiguously: the n-th
  // visible row sits at viewport_y0 + n * row_h - scroll_offset.
  int row_top_y[SID_COUNT];
  int visible_count;
  int label_x;
  int label_w;
  int track_x;
  int track_w;
  int value_x;
  int value_w;
  int text_size_px;  // body text size for labels/values
  int title_size_px; // modal title size

  // Scrollable rows region. Rendering clips to (viewport_y0, viewport_y1)
  // and click hit-testing rejects ys outside that band.
  int viewport_y0;
  int viewport_y1;
  int max_scroll;  // 0 when all rows fit; otherwise content_h - viewport_h
  int scrollbar_x; // 0 when scrollbar_w == 0
  int scrollbar_w; // 0 when not scrollable
} Layout;

static int imin(int a, int b)
{
  return a < b ? a : b;
}
static int imax(int a, int b)
{
  return a > b ? a : b;
}

// Conditional row visibility. BROWSE FILE only shows when the active
// source is FILE; the error label only when there's a message to show.
static bool row_visible(const AppState *app, int id)
{
  if (id == SID_BROWSE_FILE)
    return app && app->source_config.kind == AUDIO_SOURCE_FILE;
  if (id == SID_SOURCE_ERROR)
    return app && app->ui_error_msg[0] != '\0';
  return true;
}

// Builds the per-frame layout from the current window size and persistent
// controls state. Clamps `c->scroll_offset` to the current valid range so
// callers can read it back safely. `app` is consulted for per-row
// visibility so hidden rows are excluded from the rows table.
static void compute_layout(int win_w, int win_h, Controls *c, const AppState *app, Layout *L)
{
  const int pad = 28;
  const int title_h = 44;
  const int row_h = 40;

  L->row_h = row_h;
  L->text_size_px = UI_TEXT_MEDIUM;
  L->title_size_px = UI_TEXT_TITLE;

  // Count visible rows up front so modal sizing reflects what's actually
  // going to be drawn (the BROWSE FILE / error rows come and go).
  int visible_count = 0;
  for (int i = 0; i < SID_COUNT; ++i)
    if (row_visible(app, i))
      ++visible_count;
  L->visible_count = visible_count;

  // Natural height needed to show every visible row without scrolling.
  const int natural_h = title_h + visible_count * row_h + 2 * pad;
  // Available height: leave a `pad` margin top and bottom, but enforce a
  // minimum that still shows the title plus a couple of rows.
  const int min_modal_h = title_h + 2 * row_h + 2 * pad;
  const int max_modal_h = imax(min_modal_h, win_h - 2 * pad);
  L->modal_h = imin(natural_h, max_modal_h);

  L->modal_w = imin(720, imax(420, win_w - 2 * pad));
  L->modal_x = (win_w - L->modal_w) / 2;
  L->modal_y = (win_h - L->modal_h) / 2;
  if (L->modal_x < 0)
    L->modal_x = 0;
  if (L->modal_y < 0)
    L->modal_y = 0;

  const int inner_x = L->modal_x + pad;
  const int inner_w = L->modal_w - 2 * pad;

  L->viewport_y0 = L->modal_y + pad + title_h;
  L->viewport_y1 = L->modal_y + L->modal_h - pad;
  const int viewport_h = imax(0, L->viewport_y1 - L->viewport_y0);
  const int content_h = visible_count * row_h;
  L->max_scroll = imax(0, content_h - viewport_h);

  if (c)
    c->scroll_offset = clamp_f(c->scroll_offset, 0.0f, (float)L->max_scroll);
  const int scroll = c ? (int)c->scroll_offset : 0;

  // Pack visible rows contiguously starting at viewport_y0 - scroll.
  // Hidden rows get -1 so widget_rect() and friends can refuse to compute
  // geometry for them.
  int vis_i = 0;
  for (int i = 0; i < SID_COUNT; ++i)
  {
    if (row_visible(app, i))
    {
      L->row_top_y[i] = L->viewport_y0 + vis_i * row_h - scroll;
      ++vis_i;
    }
    else
    {
      L->row_top_y[i] = -1;
    }
  }

  // Scrollbar lives in the right padding so it doesn't eat into the
  // value column. Width fixed; gap to the modal edge is half the pad.
  L->scrollbar_w = (L->max_scroll > 0) ? 6 : 0;
  L->scrollbar_x = L->modal_x + L->modal_w - pad / 2 - L->scrollbar_w;

  L->label_w = 220;
  L->value_w = 90;
  const int gap = 16;
  L->label_x = inner_x;
  L->track_x = inner_x + L->label_w + gap;
  L->track_w = inner_w - L->label_w - L->value_w - 2 * gap;
  L->value_x = inner_x + inner_w - L->value_w;
}

// Widget rect for a row. Geometry depends on the kind: continuous sliders
// use the full track width; checkboxes are a small square; dropdowns are a
// fixed-width button; the file-picker button spans the full track area;
// labels span everything from label_x to the value column for full-width
// text rendering. Returns false (and a zeroed rect) when the row is hidden
// or absent — callers should bail out in that case.
static bool widget_rect(const Layout *L, int slider_index, SDL_FRect *out)
{
  out->x = 0.0f;
  out->y = 0.0f;
  out->w = 0.0f;
  out->h = 0.0f;
  const int top = L->row_top_y[slider_index];
  if (top < 0)
    return false;
  const SliderKind k = SLIDERS[slider_index].kind;
  const int cy = top + L->row_h / 2;
  switch (k)
  {
  case SK_FLOAT:
    out->x = (float)L->track_x;
    out->y = (float)(cy - 4);
    out->w = (float)L->track_w;
    out->h = 8.0f;
    break;
  case SK_BOOL:
    out->x = (float)L->track_x;
    out->y = (float)(cy - 10);
    out->w = 20.0f;
    out->h = 20.0f;
    break;
  case SK_BUTTON:
    // Wide clickable bar spanning the full track + value columns.
    out->x = (float)L->track_x;
    out->y = (float)(cy - 11);
    out->w = (float)(L->value_x + L->value_w - L->track_x);
    out->h = 22.0f;
    break;
  case SK_LABEL:
    // Spans the entire row so wrapping math has the widest possible band.
    out->x = (float)L->label_x;
    out->y = (float)top;
    out->w = (float)(L->value_x + L->value_w - L->label_x);
    out->h = (float)L->row_h;
    break;
  case SK_DROPDOWN_UINT:
  default:
    out->x = (float)L->track_x;
    out->y = (float)(cy - 11);
    out->w = 140.0f;
    out->h = 22.0f;
    break;
  }
  return true;
}

// Per-option rect for an open dropdown popup. Options stack vertically
// directly below the dropdown button.
static void dropdown_option_rect(const Layout *L, int slider_index, int option_index,
                                 SDL_FRect *out)
{
  SDL_FRect btn = {0};
  (void)widget_rect(L, slider_index, &btn);
  out->x = btn.x;
  out->y = btn.y + btn.h + (float)option_index * btn.h;
  out->w = btn.w;
  out->h = btn.h;
}

// Scrollbar thumb rect for the current scroll offset, or zero-w rect when
// the modal isn't scrollable. Thumb height is proportional to the visible
// fraction of the rows; minimum 24 px so it stays grabbable.
static void scrollbar_thumb_rect(const Layout *L, float scroll_offset, SDL_FRect *out)
{
  out->x = 0.0f;
  out->y = 0.0f;
  out->w = 0.0f;
  out->h = 0.0f;
  if (L->scrollbar_w <= 0 || L->max_scroll <= 0)
    return;
  const float viewport_h = (float)(L->viewport_y1 - L->viewport_y0);
  const float content_h = (float)(L->visible_count * L->row_h);
  float thumb_h = viewport_h * (viewport_h / content_h);
  if (thumb_h < 24.0f)
    thumb_h = 24.0f;
  if (thumb_h > viewport_h)
    thumb_h = viewport_h;
  const float travel = viewport_h - thumb_h;
  const float t = (L->max_scroll > 0) ? (scroll_offset / (float)L->max_scroll) : 0.0f;
  out->x = (float)L->scrollbar_x;
  out->y = (float)L->viewport_y0 + t * travel;
  out->w = (float)L->scrollbar_w;
  out->h = thumb_h;
}

// ---------------------------------------------------------------------------
// Hit testing + event handling.
// ---------------------------------------------------------------------------

static bool point_in_rect(const SDL_FRect *r, float mx, float my)
{
  return mx >= r->x && mx < r->x + r->w && my >= r->y && my < r->y + r->h;
}

// Row index whose widget rect contains (mx, my), or -1. Clicks landing
// outside the scrollable viewport are rejected so rows scrolled out of
// sight stay un-clickable. Label rows are non-interactive even when hit
// by the cursor.
static int hit_test_widget(const Layout *L, float mx, float my)
{
  if (my < (float)L->viewport_y0 || my >= (float)L->viewport_y1)
    return -1;
  for (int i = 0; i < SID_COUNT; ++i)
  {
    if (SLIDERS[i].kind == SK_LABEL)
      continue;
    SDL_FRect r;
    if (!widget_rect(L, i, &r))
      continue;
    if (point_in_rect(&r, mx, my))
      return i;
  }
  return -1;
}

// If a dropdown is open, returns the option index under (mx, my), else -1.
static int hit_test_dropdown_option(const Layout *L, int open_dropdown, float mx, float my)
{
  if (open_dropdown < 0)
    return -1;
  const SliderDef *def = &SLIDERS[open_dropdown];
  for (int i = 0; i < def->discrete_count; ++i)
  {
    SDL_FRect r;
    dropdown_option_rect(L, open_dropdown, i, &r);
    if (point_in_rect(&r, mx, my))
      return i;
  }
  return -1;
}

static bool point_in_modal(const Layout *L, float mx, float my)
{
  return mx >= (float)L->modal_x && mx < (float)(L->modal_x + L->modal_w) &&
         my >= (float)L->modal_y && my < (float)(L->modal_y + L->modal_h);
}

static void set_slider_from_x(AppState *app, const Layout *L, int slider_index, float mx)
{
  SDL_FRect r;
  if (!widget_rect(L, slider_index, &r) || r.w <= 0.0f)
    return;
  const float t = (mx - r.x) / r.w;
  const float v = pos_to_value(&SLIDERS[slider_index], t);
  write_value(app, slider_index, v);
}

// ---------------------------------------------------------------------------
// Public API.
// ---------------------------------------------------------------------------

void controls_init(Controls *c)
{
  if (!c)
    return;
  c->visible = false;
  c->hovered = -1;
  c->dragging = -1;
  c->open_dropdown = -1;
  c->scroll_offset = 0.0f;
  c->scrollbar_dragging = false;
  c->scrollbar_grab_y = 0.0f;
  c->scrollbar_grab_scroll = 0.0f;
  // show_hint is set to true in main before config_load so that the
  // persisted value isn't clobbered here; we only reset the transient
  // interactive fields.
  c->hint_dont_show_again = false;
  c->about_visible = false;
}

void controls_toggle(Controls *c)
{
  if (!c)
    return;
  c->visible = !c->visible;
  c->dragging = -1;
  c->hovered = -1;
  c->open_dropdown = -1;
  c->scrollbar_dragging = false;
  // Close the about panel whenever the modal closes so it doesn't
  // re-open stale on the next toggle.
  if (!c->visible)
    c->about_visible = false;
}

// ---------------------------------------------------------------------------
// Hint banner and about panel geometry helpers.
// Defined here — before controls_handle_event and controls_render — so
// both the event handler and the renderer share identical rects.
// ---------------------------------------------------------------------------

// True when the hint banner should expose a "PLAY DEMO SONG" button: the
// current source is the bundled demo and it isn't currently producing
// audio. Other sources (mic/system) auto-start without a prompt, so the
// button is irrelevant for them.
static bool hint_show_play(const AppState *app)
{
  if (!app || app->source_config.kind != AUDIO_SOURCE_DEMO)
    return false;
  if (!app->source)
    return true;
  return !audio_source_is_running(app->source);
}

// Floating panel anchored to the bottom-centre of the window. Grows
// taller when the PLAY DEMO SONG button is visible so the play button
// has its own row above the existing hint text.
static SDL_FRect hint_panel_rect(int win_w, int win_h, bool show_play)
{
  const float pw = 370.0f;
  const float ph = show_play ? 130.0f : 76.0f;
  return (SDL_FRect){((float)win_w - pw) * 0.5f, (float)win_h - ph - 24.0f, pw, ph};
}

// × dismiss button — top-right corner of the hint panel.
static SDL_FRect hint_dismiss_rect(int win_w, int win_h, bool show_play)
{
  SDL_FRect p = hint_panel_rect(win_w, win_h, show_play);
  return (SDL_FRect){p.x + p.w - 30.0f, p.y + 8.0f, 22.0f, 22.0f};
}

// "Don't show again" checkbox — bottom-left of the hint panel.
static SDL_FRect hint_checkbox_rect(int win_w, int win_h, bool show_play)
{
  SDL_FRect p = hint_panel_rect(win_w, win_h, show_play);
  return (SDL_FRect){p.x + 14.0f, p.y + p.h - 26.0f, 14.0f, 14.0f};
}

// PLAY DEMO SONG button — centered horizontally near the top of the
// expanded hint panel. Only valid when hint_show_play() is true.
static SDL_FRect hint_play_rect(int win_w, int win_h)
{
  SDL_FRect p = hint_panel_rect(win_w, win_h, true);
  const float bw = 220.0f;
  const float bh = 34.0f;
  return (SDL_FRect){p.x + (p.w - bw) * 0.5f, p.y + 14.0f, bw, bh};
}

// About panel "< BACK" button — left side of the modal title bar.
static SDL_FRect about_back_rect(const Layout *L)
{
  return (SDL_FRect){(float)(L->modal_x + 14), (float)(L->modal_y + 14), 74.0f, 22.0f};
}

// Clickable GitHub URL rect in the about panel. Mirrors the y-position
// arithmetic in render_about so hit-testing and rendering are in sync.
static SDL_FRect about_link_rect(const Layout *L)
{
  int y = L->viewport_y0 + 8;
  y += ui_text_line_height(UI_TEXT_LARGE) + 6;  // "Audio Visualizer"
  y += ui_text_line_height(UI_TEXT_MEDIUM) * 3; // Version / Author / License
  y += 16;                                      // gap before PRIVACY
  y += ui_text_line_height(UI_TEXT_MEDIUM) + 4; // "PRIVACY" header
  y += ui_text_line_height(UI_TEXT_SMALL) * 5;  // 5 privacy lines (incl. blank)
  y += 14;                                      // gap before SOURCE CODE
  y += ui_text_line_height(UI_TEXT_MEDIUM) + 4; // "SOURCE CODE" header
  const int x = L->modal_x + 28;
  const int w = ui_text_width(APP_GITHUB_URL, UI_TEXT_SMALL);
  const int h = ui_text_line_height(UI_TEXT_SMALL);
  return (SDL_FRect){(float)x, (float)y, (float)w, (float)h};
}

// ---------------------------------------------------------------------------
// Third-party library links shown in the About panel.
typedef struct
{
  const char *name;
  const char *url;
} AboutLib;

static const AboutLib k_about_libs[] = {
    {"SDL3", LIB_SDL3_URL},
    {"FFTW3", LIB_FFTW3_URL},
    {"miniaudio", LIB_MINIAUDIO_URL},
    {"stb_truetype", LIB_STB_TT_URL},
};
#define ABOUT_LIBS_COUNT (int)(sizeof(k_about_libs) / sizeof(k_about_libs[0]))

// Y coordinate where the LIBRARIES section begins (just below the source URL).
static int about_libs_y0(const Layout *L)
{
  SDL_FRect src = about_link_rect(L);
  return (int)src.y + ui_text_line_height(UI_TEXT_SMALL) + 14;
}

// Clickable rect for the URL of library at index i. Mirrors render_about.
static SDL_FRect about_lib_url_rect(const Layout *L, int i)
{
  const int lh_m = ui_text_line_height(UI_TEXT_MEDIUM);
  const int lh_s = ui_text_line_height(UI_TEXT_SMALL);
  int y = about_libs_y0(L);
  y += lh_m + 4;           // "LIBRARIES" header
  y += i * (lh_s * 2 + 4); // previous entries (name + url + gap)
  y += lh_s;               // skip the name line to reach the URL line
  const int x = L->modal_x + 28;
  const int w = ui_text_width(k_about_libs[i].url, UI_TEXT_SMALL);
  return (SDL_FRect){(float)x, (float)y, (float)w, (float)lh_s};
}

bool controls_handle_event(Controls *c, const SDL_Event *ev, AppState *app)
{
  if (!c || !ev || !app)
    return false;

  // Tab toggles the modal at any time. Dismiss the hint when Tab is
  // pressed — the user found the shortcut, so the nudge is no longer
  // needed for this session.
  if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_TAB)
  {
    c->show_hint = false;
    controls_toggle(c);
    return true;
  }

  // Hint banner events (only when the modal is closed).
  if (c->show_hint && !c->visible && ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
      ev->button.button == SDL_BUTTON_LEFT)
  {
    const float mx = ev->button.x;
    const float my = ev->button.y;
    const bool show_play = hint_show_play(app);

    // PLAY DEMO SONG button — only present when the demo source is loaded
    // but not yet playing. Asks the main loop to start the deferred
    // source; if the source failed to create at boot, request a fresh
    // source change instead so it gets recreated cleanly.
    if (show_play)
    {
      SDL_FRect play_r = hint_play_rect(app->window_width, app->window_height);
      if (point_in_rect(&play_r, mx, my))
      {
        if (app->source)
          app->start_audio_request = true;
        else
        {
          app->requested_source_kind = AUDIO_SOURCE_DEMO;
          app->source_change_pending = true;
        }
        return true;
      }
    }

    // × dismiss button.
    SDL_FRect dismiss_r = hint_dismiss_rect(app->window_width, app->window_height, show_play);
    if (point_in_rect(&dismiss_r, mx, my))
    {
      c->show_hint = false;
      if (c->hint_dont_show_again)
        config_save(app);
      return true;
    }

    // "Don't show again" checkbox.
    SDL_FRect check_r = hint_checkbox_rect(app->window_width, app->window_height, show_play);
    if (point_in_rect(&check_r, mx, my))
    {
      c->hint_dont_show_again = !c->hint_dont_show_again;
      return true;
    }

    // Clicks anywhere inside the panel are consumed (don't fall through).
    SDL_FRect panel_r = hint_panel_rect(app->window_width, app->window_height, show_play);
    if (point_in_rect(&panel_r, mx, my))
      return true;
  }

  if (!c->visible)
    return false;

  // Escape: close the about panel first (back to settings), or close the
  // dropdown, or close the modal.
  if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE)
  {
    if (c->open_dropdown >= 0)
      c->open_dropdown = -1;
    else if (c->about_visible)
      c->about_visible = false;
    else
      controls_toggle(c);
    return true;
  }

  Layout L;
  compute_layout(app->window_width, app->window_height, c, app, &L);

  // About panel: back button, GitHub link, and modal-interior consumption.
  if (c->about_visible)
  {
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev->button.button == SDL_BUTTON_LEFT)
    {
      const float mx = ev->button.x;
      const float my = ev->button.y;
      SDL_FRect back = about_back_rect(&L);
      if (point_in_rect(&back, mx, my))
      {
        c->about_visible = false;
        return true;
      }
      SDL_FRect link = about_link_rect(&L);
      if (point_in_rect(&link, mx, my))
      {
        SDL_OpenURL(APP_GITHUB_URL);
        return true;
      }
      for (int i = 0; i < ABOUT_LIBS_COUNT; ++i)
      {
        SDL_FRect lr = about_lib_url_rect(&L, i);
        if (point_in_rect(&lr, mx, my))
        {
          SDL_OpenURL(k_about_libs[i].url);
          return true;
        }
      }
      return point_in_modal(&L, mx, my);
    }
    return false;
  }

  switch (ev->type)
  {
  case SDL_EVENT_MOUSE_WHEEL:
  {
    if (L.max_scroll <= 0)
      return false;
    float mx = 0.0f, my = 0.0f;
    SDL_GetMouseState(&mx, &my);
    if (!point_in_modal(&L, mx, my))
      return false;
    // SDL reports wheel ticks; one notch ≈ a couple of rows.
    const float step = (float)L.row_h * 2.0f;
    c->scroll_offset = clamp_f(c->scroll_offset - ev->wheel.y * step, 0.0f, (float)L.max_scroll);
    // Anchored dropdown popups can't follow a scroll; close them.
    c->open_dropdown = -1;
    return true;
  }

  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (ev->button.button == SDL_BUTTON_LEFT)
    {
      const float mx = ev->button.x;
      const float my = ev->button.y;

      // 1. Scrollbar thumb takes priority so a click that happens to be
      // on top of a row widget at the right edge still grabs the thumb.
      if (L.max_scroll > 0)
      {
        SDL_FRect thumb;
        scrollbar_thumb_rect(&L, c->scroll_offset, &thumb);
        if (point_in_rect(&thumb, mx, my))
        {
          c->scrollbar_dragging = true;
          c->scrollbar_grab_y = my;
          c->scrollbar_grab_scroll = c->scroll_offset;
          c->open_dropdown = -1;
          return true;
        }
      }

      // 2. If a dropdown is open, give it first crack at the click.
      if (c->open_dropdown >= 0)
      {
        const int opt = hit_test_dropdown_option(&L, c->open_dropdown, mx, my);
        if (opt >= 0)
        {
          const SliderDef *def = &SLIDERS[c->open_dropdown];
          write_value(app, c->open_dropdown, (float)def->discrete[opt]);
          c->open_dropdown = -1;
          return true;
        }
        // Click outside any option closes the dropdown without selecting.
        c->open_dropdown = -1;
      }

      // 3. Hit-test the row widgets.
      const int hit = hit_test_widget(&L, mx, my);
      if (hit >= 0)
      {
        const SliderDef *def = &SLIDERS[hit];
        switch (def->kind)
        {
        case SK_FLOAT:
          c->dragging = hit;
          set_slider_from_x(app, &L, hit, mx);
          break;
        case SK_BOOL:
        {
          const float v = read_value(app, hit);
          write_value(app, hit, v >= 0.5f ? 0.0f : 1.0f);
          break;
        }
        case SK_DROPDOWN_UINT:
          c->open_dropdown = hit;
          break;
        case SK_BUTTON:
          if (hit == SID_BROWSE_FILE)
          {
            // Defer SDL_ShowOpenFileDialog to the main loop so it runs
            // on the main thread.
            app->open_file_picker_request = true;
          }
          else if (hit == SID_ABOUT)
          {
            c->about_visible = true;
          }
          break;
        case SK_LABEL:
          break;
        }
      }
      // Swallow clicks anywhere inside the modal so they don't reach the
      // app behind it. Clicks outside the modal pass through.
      return point_in_modal(&L, mx, my) || hit >= 0;
    }
    return false;

  case SDL_EVENT_MOUSE_BUTTON_UP:
    if (ev->button.button == SDL_BUTTON_LEFT)
    {
      bool consumed = false;
      if (c->scrollbar_dragging)
      {
        c->scrollbar_dragging = false;
        consumed = true;
      }
      if (c->dragging >= 0)
      {
        c->dragging = -1;
        consumed = true;
      }
      return consumed;
    }
    return false;

  case SDL_EVENT_MOUSE_MOTION:
  {
    const float mx = ev->motion.x;
    const float my = ev->motion.y;
    if (c->scrollbar_dragging && L.max_scroll > 0)
    {
      // Translate vertical mouse travel into scroll offset along the
      // thumb's track. Recompute thumb_h from the current layout so a
      // resize mid-drag stays consistent.
      SDL_FRect thumb;
      scrollbar_thumb_rect(&L, c->scrollbar_grab_scroll, &thumb);
      const float viewport_h = (float)(L.viewport_y1 - L.viewport_y0);
      const float travel = viewport_h - thumb.h;
      if (travel > 0.0f)
      {
        const float dy = my - c->scrollbar_grab_y;
        const float new_offset = c->scrollbar_grab_scroll + dy * ((float)L.max_scroll / travel);
        c->scroll_offset = clamp_f(new_offset, 0.0f, (float)L.max_scroll);
      }
      return true;
    }
    c->hovered = hit_test_widget(&L, mx, my);
    if (c->dragging >= 0)
    {
      set_slider_from_x(app, &L, c->dragging, mx);
      return true;
    }
    return false;
  }

  default:
    return false;
  }
}

// Y position that vertically centers a text run of `size_px` inside a
// rect of height `rect_h` whose top is at `rect_y`. Returns the text top
// expected by ui_text_draw.
static int text_y_for_rect(float rect_y, float rect_h, int size_px)
{
  const int line_h = ui_text_line_height(size_px);
  return (int)rect_y + ((int)rect_h - line_h) / 2;
}

static void render_float_slider(SDL_Renderer *ren, const Layout *L, int i, float v)
{
  const SliderDef *def = &SLIDERS[i];
  const float t = value_to_pos(def, v);

  SDL_FRect track;
  widget_rect(L, i, &track);

  SDL_SetRenderDrawColor(ren, 255, 255, 255, 60);
  SDL_RenderFillRect(ren, &track);

  SDL_FRect fill = track;
  fill.w = track.w * t;
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 200);
  SDL_RenderFillRect(ren, &fill);

  const float knob_w = 6.0f;
  const float knob_h = 18.0f;
  SDL_FRect knob = {
      track.x + track.w * t - knob_w * 0.5f,
      track.y + track.h * 0.5f - knob_h * 0.5f,
      knob_w,
      knob_h,
  };
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  SDL_RenderFillRect(ren, &knob);
}

static void render_bool_checkbox(SDL_Renderer *ren, const Layout *L, int i, float v)
{
  SDL_FRect box;
  widget_rect(L, i, &box);

  SDL_SetRenderDrawColor(ren, 255, 255, 255, 60);
  SDL_RenderFillRect(ren, &box);
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 200);
  SDL_RenderRect(ren, &box);

  if (v >= 0.5f)
  {
    SDL_FRect inner = {box.x + 4.0f, box.y + 4.0f, box.w - 8.0f, box.h - 8.0f};
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_RenderFillRect(ren, &inner);
  }
}

// Returns a pointer into `path` at the trailing path component. Works for
// both forward and backslash separators so a Windows-style path supplied
// through the file dialog still renders sensibly.
static const char *basename_of(const char *path)
{
  if (!path || !*path)
    return "";
  const char *p = path;
  const char *last = path;
  for (; *p; ++p)
    if (*p == '/' || *p == '\\')
      last = p + 1;
  return last;
}

// Wide pill button (used by BROWSE FILE…). Renders the current filename if
// one is set, otherwise a "BROWSE…" placeholder.
static void render_button(SDL_Renderer *ren, const Layout *L, int i, const AppState *app)
{
  SDL_FRect btn;
  if (!widget_rect(L, i, &btn))
    return;

  SDL_SetRenderDrawColor(ren, 255, 255, 255, 60);
  SDL_RenderFillRect(ren, &btn);
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 200);
  SDL_RenderRect(ren, &btn);

  // Non-BROWSE buttons display their static label; BROWSE FILE shows the
  // chosen filename (or the placeholder when no file has been picked yet).
  const char *text = (i == SID_BROWSE_FILE) ? "BROWSE..." : SLIDERS[i].label;
  if (i == SID_BROWSE_FILE && app->current_file_path[0] != '\0')
    text = basename_of(app->current_file_path);
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  ui_text_draw(ren, text, (int)btn.x + 10, text_y_for_rect(btn.y, btn.h, L->text_size_px),
               L->text_size_px);
}

// Read-only red text row used for the source error message. Spans the full
// row width so longer messages still fit.
static void render_label(SDL_Renderer *ren, const Layout *L, int i, const AppState *app)
{
  SDL_FRect r;
  if (!widget_rect(L, i, &r))
    return;
  const char *text = (i == SID_SOURCE_ERROR) ? app->ui_error_msg : "";
  if (!text || !*text)
    return;
  SDL_SetRenderDrawColor(ren, 255, 90, 90, 255);
  ui_text_draw(ren, text, (int)r.x, text_y_for_rect(r.y, r.h, L->text_size_px), L->text_size_px);
}

static void render_dropdown_button(SDL_Renderer *ren, const Layout *L, int i, float v)
{
  const SliderDef *def = &SLIDERS[i];
  SDL_FRect btn;
  widget_rect(L, i, &btn);

  SDL_SetRenderDrawColor(ren, 255, 255, 255, 60);
  SDL_RenderFillRect(ren, &btn);
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 200);
  SDL_RenderRect(ren, &btn);

  // Current value text, left-aligned inside the button.
  char buf[32];
  format_value(def, v, buf, sizeof(buf));
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  ui_text_draw(ren, buf, (int)btn.x + 10, text_y_for_rect(btn.y, btn.h, L->text_size_px),
               L->text_size_px);

  // Small caret on the right edge to hint at the dropdown affordance.
  const float caret_size = 4.0f;
  const float cx = btn.x + btn.w - 12.0f;
  const float cy = btn.y + btn.h * 0.5f - 1.0f;
  for (int row = 0; row < (int)caret_size; ++row)
  {
    SDL_FRect r = {cx - (float)row, cy + (float)row, 2.0f * (float)row + 1.0f, 1.0f};
    SDL_RenderFillRect(ren, &r);
  }
}

static void render_dropdown_popup(SDL_Renderer *ren, const Layout *L, int slider_index, float v)
{
  const SliderDef *def = &SLIDERS[slider_index];
  if (def->kind != SK_DROPDOWN_UINT || def->discrete_count <= 0)
    return;

  const int cur = discrete_index_for_value(def, v);

  for (int i = 0; i < def->discrete_count; ++i)
  {
    SDL_FRect r;
    dropdown_option_rect(L, slider_index, i, &r);

    SDL_SetRenderDrawColor(ren, 0, 0, 0, 235);
    SDL_RenderFillRect(ren, &r);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, i == cur ? 200 : 80);
    SDL_RenderRect(ren, &r);

    char buf[32];
    if (def->labels && i < def->discrete_count)
      snprintf(buf, sizeof(buf), "%s", def->labels[i]);
    else
      snprintf(buf, sizeof(buf), "%u", (unsigned)def->discrete[i]);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    ui_text_draw(ren, buf, (int)r.x + 10, text_y_for_rect(r.y, r.h, L->text_size_px),
                 L->text_size_px);
  }
}

// ---------------------------------------------------------------------------
// Hint banner renderer.
// ---------------------------------------------------------------------------

static void render_hint(const Controls *c, SDL_Renderer *ren, const AppState *app)
{
  const int ww = app->window_width;
  const int wh = app->window_height;
  const bool show_play = hint_show_play(app);
  SDL_FRect panel = hint_panel_rect(ww, wh, show_play);
  SDL_FRect dismiss = hint_dismiss_rect(ww, wh, show_play);

  // Checkbox rect computed inline (avoids the helper calling order issue).
  SDL_FRect checkbox = {panel.x + 14.0f, panel.y + panel.h - 26.0f, 14.0f, 14.0f};

  // Panel background + outline.
  SDL_SetRenderDrawColor(ren, 10, 10, 10, 220);
  SDL_RenderFillRect(ren, &panel);
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 70);
  SDL_RenderRect(ren, &panel);

  // PLAY DEMO SONG button — only when the demo source is loaded and
  // silent. Pushes the hint text down a row so both fit cleanly.
  if (show_play)
  {
    SDL_FRect play = hint_play_rect(ww, wh);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 220);
    SDL_RenderFillRect(ren, &play);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderRect(ren, &play);
    const char *play_label = "> PLAY DEMO SONG";
    const int pw = ui_text_width(play_label, UI_TEXT_MEDIUM);
    const int plh = ui_text_line_height(UI_TEXT_MEDIUM);
    ui_text_draw(ren, play_label, (int)play.x + ((int)play.w - pw) / 2,
                 (int)play.y + ((int)play.h - plh) / 2, UI_TEXT_MEDIUM);
  }

  // Main hint text, centred horizontally below the play button (or at the
  // top of the panel when the play button is absent).
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 230);
  const char *hint_text = "PRESS TAB TO OPEN SETTINGS";
  const int tw = ui_text_width(hint_text, UI_TEXT_MEDIUM);
  const int tx = (int)panel.x + ((int)panel.w - tw) / 2;
  const int ty = (int)panel.y + (show_play ? 64 : 12);
  ui_text_draw(ren, hint_text, tx, ty, UI_TEXT_MEDIUM);

  // × dismiss button.
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 50);
  SDL_RenderFillRect(ren, &dismiss);
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 150);
  SDL_RenderRect(ren, &dismiss);
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 230);
  {
    const char *x_label = "X";
    const int xw = ui_text_width(x_label, UI_TEXT_SMALL);
    const int xlh = ui_text_line_height(UI_TEXT_SMALL);
    ui_text_draw(ren, x_label, (int)dismiss.x + ((int)dismiss.w - xw) / 2,
                 (int)dismiss.y + ((int)dismiss.h - xlh) / 2, UI_TEXT_SMALL);
  }

  // "Don't show again" checkbox.
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 50);
  SDL_RenderFillRect(ren, &checkbox);
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 180);
  SDL_RenderRect(ren, &checkbox);
  if (c->hint_dont_show_again)
  {
    SDL_FRect inner = {checkbox.x + 3.0f, checkbox.y + 3.0f, checkbox.w - 6.0f, checkbox.h - 6.0f};
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_RenderFillRect(ren, &inner);
  }

  // Checkbox label.
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 180);
  const int clh = ui_text_line_height(UI_TEXT_SMALL);
  ui_text_draw(ren, "DON'T SHOW AGAIN", (int)checkbox.x + (int)checkbox.w + 6,
               (int)checkbox.y + ((int)checkbox.h - clh) / 2, UI_TEXT_SMALL);
}

// ---------------------------------------------------------------------------
// About panel renderer. Replaces the rows region when c->about_visible.
// Draws title, back button, and static about + privacy content.
// ---------------------------------------------------------------------------

static void render_about(SDL_Renderer *ren, const Layout *L)
{
  // "About" title — centred in the modal header.
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  const char *title = "About";
  const int title_w = ui_text_width(title, L->title_size_px);
  ui_text_draw(ren, title, L->modal_x + (L->modal_w - title_w) / 2, L->modal_y + 18,
               L->title_size_px);

  // "< BACK" button in the header left.
  SDL_FRect back = about_back_rect(L);
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 40);
  SDL_RenderFillRect(ren, &back);
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 150);
  SDL_RenderRect(ren, &back);
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 230);
  ui_text_draw(ren, "< BACK", (int)back.x + 8, text_y_for_rect(back.y, back.h, L->text_size_px),
               L->text_size_px);

  // Content — rendered in the rows viewport region.
  const int x = L->modal_x + 28;
  int y = L->viewport_y0 + 8;
  const int lh_l = ui_text_line_height(UI_TEXT_LARGE);
  const int lh_m = ui_text_line_height(UI_TEXT_MEDIUM);
  const int lh_s = ui_text_line_height(UI_TEXT_SMALL);

  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  ui_text_draw(ren, "Audio Visualizer", x, y, UI_TEXT_LARGE);
  y += lh_l + 6;

  SDL_SetRenderDrawColor(ren, 255, 255, 255, 200);
  ui_text_draw(ren, "Version " APP_VERSION_STRING, x, y, UI_TEXT_MEDIUM);
  y += lh_m;
  ui_text_draw(ren, "Author: Charles Thompson", x, y, UI_TEXT_MEDIUM);
  y += lh_m;
  ui_text_draw(ren, "License: GNU GPL v3", x, y, UI_TEXT_MEDIUM);
  y += lh_m + 16;

  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  ui_text_draw(ren, "PRIVACY", x, y, UI_TEXT_MEDIUM);
  y += lh_m + 4;

  SDL_SetRenderDrawColor(ren, 255, 255, 255, 170);
  static const char *const privacy[] = {
      "This app collects no data, stores nothing,",
      "and makes no network connections of any kind.",
      "",
      "Microphone input is processed locally in real",
      "time and is never recorded or sent anywhere.",
  };
  const int n = (int)(sizeof(privacy) / sizeof(privacy[0]));
  for (int i = 0; i < n; ++i)
  {
    if (privacy[i][0])
      ui_text_draw(ren, privacy[i], x, y, UI_TEXT_SMALL);
    y += lh_s;
  }

  // Source code link.
  y += 14;
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  ui_text_draw(ren, "SOURCE CODE", x, y, UI_TEXT_MEDIUM);
  y += lh_m + 4;

  // Draw the URL in a distinct blue-ish tint with an underline so it reads
  // as a hyperlink. SDL_OpenURL is called when the user clicks on it.
  const int url_w = ui_text_width(APP_GITHUB_URL, UI_TEXT_SMALL);
  SDL_SetRenderDrawColor(ren, 100, 180, 255, 220);
  ui_text_draw(ren, APP_GITHUB_URL, x, y, UI_TEXT_SMALL);
  SDL_RenderLine(ren, (float)x, (float)(y + lh_s - 2), (float)(x + url_w), (float)(y + lh_s - 2));
  y += lh_s;

  // Libraries section.
  y += 14;
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  ui_text_draw(ren, "LIBRARIES", x, y, UI_TEXT_MEDIUM);
  y += lh_m + 4;

  for (int i = 0; i < ABOUT_LIBS_COUNT; ++i)
  {
    // Library name in dim white.
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 200);
    ui_text_draw(ren, k_about_libs[i].name, x, y, UI_TEXT_SMALL);
    y += lh_s;

    // Clickable URL in blue with underline.
    const int lw = ui_text_width(k_about_libs[i].url, UI_TEXT_SMALL);
    SDL_SetRenderDrawColor(ren, 100, 180, 255, 220);
    ui_text_draw(ren, k_about_libs[i].url, x, y, UI_TEXT_SMALL);
    SDL_RenderLine(ren, (float)x, (float)(y + lh_s - 2), (float)(x + lw), (float)(y + lh_s - 2));
    y += lh_s + 4;
  }

  // Demo song attribution. The bundled "demo-song.mp3" is a public-domain
  // recording — credit the performer and license per CC Public Domain
  // Mark 1.0 best practice.
  y += 14;
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  ui_text_draw(ren, "DEMO SONG", x, y, UI_TEXT_MEDIUM);
  y += lh_m + 4;

  SDL_SetRenderDrawColor(ren, 255, 255, 255, 170);
  static const char *const demo_credit[] = {
      "J. S. Bach - Prelude No. 1 in C major,",
      "BWV 846 (Well-Tempered Clavier, Book 1).",
      "Performed by Kimiko Ishizaka.",
      "License: CC Public Domain Mark 1.0 Universal.",
  };
  const int dn = (int)(sizeof(demo_credit) / sizeof(demo_credit[0]));
  for (int i = 0; i < dn; ++i)
  {
    ui_text_draw(ren, demo_credit[i], x, y, UI_TEXT_SMALL);
    y += lh_s;
  }
}

void controls_render(Controls *c, SDL_Renderer *ren, const AppState *app)
{
  if (!c || !ren || !app)
    return;

  SDL_BlendMode prev_blend = SDL_BLENDMODE_NONE;
  SDL_GetRenderDrawBlendMode(ren, &prev_blend);
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

  // Hint banner — shown when the settings modal is closed.
  if (c->show_hint && !c->visible)
  {
    render_hint(c, ren, app);
    SDL_SetRenderDrawBlendMode(ren, prev_blend);
    return;
  }

  if (!c->visible)
  {
    SDL_SetRenderDrawBlendMode(ren, prev_blend);
    return;
  }

  Layout L;
  compute_layout(app->window_width, app->window_height, c, app, &L);

  // Full-window black translucent overlay.
  SDL_SetRenderDrawColor(ren, 0, 0, 0, 180);
  SDL_FRect full = {0.0f, 0.0f, (float)app->window_width, (float)app->window_height};
  SDL_RenderFillRect(ren, &full);

  // Modal panel (slightly darker).
  SDL_SetRenderDrawColor(ren, 0, 0, 0, 210);
  SDL_FRect modal = {(float)L.modal_x, (float)L.modal_y, (float)L.modal_w, (float)L.modal_h};
  SDL_RenderFillRect(ren, &modal);

  // Thin white outline so the modal reads as a panel against the spectrum.
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 80);
  SDL_RenderRect(ren, &modal);

  // About panel replaces the slider rows (title + back button drawn inside).
  if (c->about_visible)
  {
    render_about(ren, &L);
    SDL_SetRenderDrawBlendMode(ren, prev_blend);
    return;
  }

  // Settings title.
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  const char *title = "Settings";
  const int title_w = ui_text_width(title, L.title_size_px);
  ui_text_draw(ren, title, L.modal_x + (L.modal_w - title_w) / 2, L.modal_y + 18, L.title_size_px);

  // Clip the rows region so scrolled-out rows don't bleed into the title
  // band or past the modal's bottom edge. Saved prior clip is restored
  // before returning.
  SDL_Rect prev_clip = {0, 0, 0, 0};
  const bool had_clip = SDL_RenderClipEnabled(ren);
  if (had_clip)
    SDL_GetRenderClipRect(ren, &prev_clip);
  const SDL_Rect rows_clip = {
      L.modal_x,
      L.viewport_y0,
      L.modal_w,
      L.viewport_y1 - L.viewport_y0,
  };
  SDL_SetRenderClipRect(ren, &rows_clip);

  // Per-row label, widget, and value text.
  for (int i = 0; i < SID_COUNT; ++i)
  {
    const SliderDef *def = &SLIDERS[i];
    const float v = read_value(app, i);

    SDL_FRect w;
    if (!widget_rect(&L, i, &w))
      continue;
    // Skip rows fully outside the viewport.
    if (w.y + w.h < (float)L.viewport_y0 || w.y >= (float)L.viewport_y1)
      continue;
    const int text_y = text_y_for_rect(w.y, w.h, L.text_size_px);

    // Left-column label. Empty label rows (e.g. SOURCE_ERROR) skip this so
    // their full-width text can start at label_x without overlap.
    if (def->label && def->label[0] && def->kind != SK_LABEL)
    {
      SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
      ui_text_draw(ren, def->label, L.label_x, text_y, L.text_size_px);
    }

    switch (def->kind)
    {
    case SK_FLOAT:
      render_float_slider(ren, &L, i, v);
      break;
    case SK_BOOL:
      render_bool_checkbox(ren, &L, i, v);
      break;
    case SK_DROPDOWN_UINT:
      render_dropdown_button(ren, &L, i, v);
      break;
    case SK_BUTTON:
      render_button(ren, &L, i, app);
      break;
    case SK_LABEL:
      render_label(ren, &L, i, app);
      break;
    }

    // Right-aligned numeric/state readout. Dropdowns render their value
    // inside the button; buttons/labels have no scalar value to display.
    if (def->kind == SK_FLOAT || def->kind == SK_BOOL)
    {
      char buf[32];
      format_value(def, v, buf, sizeof(buf));
      SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
      ui_text_draw(ren, buf, L.value_x, text_y, L.text_size_px);
    }
  }

  // Restore the prior clip rect before drawing the scrollbar and the
  // open dropdown popup, both of which intentionally draw outside the
  // rows-clip band.
  if (had_clip)
    SDL_SetRenderClipRect(ren, &prev_clip);
  else
    SDL_SetRenderClipRect(ren, NULL);

  // Scrollbar (track + thumb) along the modal's right padding.
  if (L.max_scroll > 0)
  {
    SDL_FRect track = {
        (float)L.scrollbar_x,
        (float)L.viewport_y0,
        (float)L.scrollbar_w,
        (float)(L.viewport_y1 - L.viewport_y0),
    };
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 30);
    SDL_RenderFillRect(ren, &track);

    SDL_FRect thumb;
    scrollbar_thumb_rect(&L, c->scroll_offset, &thumb);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, c->scrollbar_dragging ? 220 : 160);
    SDL_RenderFillRect(ren, &thumb);
  }

  // Open dropdown popup is drawn last so it overlaps later rows cleanly.
  if (c->open_dropdown >= 0)
  {
    const float v = read_value(app, c->open_dropdown);
    render_dropdown_popup(ren, &L, c->open_dropdown, v);
  }

  SDL_SetRenderDrawBlendMode(ren, prev_blend);
}
