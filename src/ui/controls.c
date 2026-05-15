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

#include "controls.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "app_state.h"
#include "text.h"
#include "../analyzer/window_fn.h"

// ---------------------------------------------------------------------------
// Slider table. Each entry binds a label to a field in AppState. Discrete
// sliders (fft_size) snap to one of `discrete` values; the slider position
// is the index into that list.
// ---------------------------------------------------------------------------

typedef enum SliderId
{
  SID_SHOW_SPECTRUM = 0,
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
  SID_COUNT,
} SliderId;

typedef enum SliderKind
{
  SK_FLOAT,
  SK_BOOL,
  SK_DROPDOWN_UINT,
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

static const SliderDef SLIDERS[SID_COUNT] = {
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
  int row_y0; // top of first slider row
  int label_x;
  int label_w;
  int track_x;
  int track_w;
  int value_x;
  int value_w;
  int text_size_px;  // body text size for labels/values
  int title_size_px; // modal title size
} Layout;

static int imin(int a, int b)
{
  return a < b ? a : b;
}
static int imax(int a, int b)
{
  return a > b ? a : b;
}

static void compute_layout(int win_w, int win_h, Layout *L)
{
  const int pad = 28;
  const int title_h = 44;
  const int row_h = 40;

  L->row_h = row_h;
  L->text_size_px = UI_TEXT_MEDIUM;
  L->title_size_px = UI_TEXT_TITLE;

  L->modal_w = imin(720, imax(420, win_w - 2 * pad));
  L->modal_h = title_h + SID_COUNT * row_h + 2 * pad;
  L->modal_x = (win_w - L->modal_w) / 2;
  L->modal_y = (win_h - L->modal_h) / 2;
  if (L->modal_x < 0)
    L->modal_x = 0;
  if (L->modal_y < 0)
    L->modal_y = 0;

  const int inner_x = L->modal_x + pad;
  const int inner_w = L->modal_w - 2 * pad;
  L->row_y0 = L->modal_y + pad + title_h;

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
// fixed-width button with the current value inside.
static void widget_rect(const Layout *L, int slider_index, SDL_FRect *out)
{
  const SliderKind k = SLIDERS[slider_index].kind;
  const int cy = L->row_y0 + slider_index * L->row_h + L->row_h / 2;
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
  case SK_DROPDOWN_UINT:
  default:
    out->x = (float)L->track_x;
    out->y = (float)(cy - 11);
    out->w = 140.0f;
    out->h = 22.0f;
    break;
  }
}

// Per-option rect for an open dropdown popup. Options stack vertically
// directly below the dropdown button.
static void dropdown_option_rect(const Layout *L, int slider_index, int option_index,
                                 SDL_FRect *out)
{
  SDL_FRect btn;
  widget_rect(L, slider_index, &btn);
  out->x = btn.x;
  out->y = btn.y + btn.h + (float)option_index * btn.h;
  out->w = btn.w;
  out->h = btn.h;
}

// ---------------------------------------------------------------------------
// Hit testing + event handling.
// ---------------------------------------------------------------------------

static bool point_in_rect(const SDL_FRect *r, float mx, float my)
{
  return mx >= r->x && mx < r->x + r->w && my >= r->y && my < r->y + r->h;
}

// Row index whose widget rect contains (mx, my), or -1.
static int hit_test_widget(const Layout *L, float mx, float my)
{
  for (int i = 0; i < SID_COUNT; ++i)
  {
    SDL_FRect r;
    widget_rect(L, i, &r);
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
  widget_rect(L, slider_index, &r);
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
}

void controls_toggle(Controls *c)
{
  if (!c)
    return;
  c->visible = !c->visible;
  c->dragging = -1;
  c->hovered = -1;
  c->open_dropdown = -1;
}

bool controls_handle_event(Controls *c, const SDL_Event *ev, AppState *app)
{
  if (!c || !ev || !app)
    return false;

  // Tab toggles the modal at any time.
  if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_TAB)
  {
    controls_toggle(c);
    return true;
  }

  if (!c->visible)
    return false;

  // Escape closes the modal (or an open dropdown first) while open;
  // consume so the main loop doesn't also treat it as quit.
  if (ev->type == SDL_EVENT_KEY_DOWN && ev->key.key == SDLK_ESCAPE)
  {
    if (c->open_dropdown >= 0)
      c->open_dropdown = -1;
    else
      controls_toggle(c);
    return true;
  }

  Layout L;
  compute_layout(app->window_width, app->window_height, &L);

  switch (ev->type)
  {
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    if (ev->button.button == SDL_BUTTON_LEFT)
    {
      const float mx = ev->button.x;
      const float my = ev->button.y;

      // 1. If a dropdown is open, give it first crack at the click.
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

      // 2. Hit-test the row widgets.
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
        }
      }
      // Swallow clicks anywhere inside the modal so they don't reach the
      // app behind it. Clicks outside the modal pass through.
      return point_in_modal(&L, mx, my) || hit >= 0;
    }
    return false;

  case SDL_EVENT_MOUSE_BUTTON_UP:
    if (ev->button.button == SDL_BUTTON_LEFT && c->dragging >= 0)
    {
      c->dragging = -1;
      return true;
    }
    return false;

  case SDL_EVENT_MOUSE_MOTION:
  {
    const float mx = ev->motion.x;
    const float my = ev->motion.y;
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

void controls_render(Controls *c, SDL_Renderer *ren, const AppState *app)
{
  if (!c || !ren || !app || !c->visible)
    return;

  Layout L;
  compute_layout(app->window_width, app->window_height, &L);

  SDL_BlendMode prev_blend = SDL_BLENDMODE_NONE;
  SDL_GetRenderDrawBlendMode(ren, &prev_blend);
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

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

  // Title.
  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  const char *title = "Settings";
  const int title_w = ui_text_width(title, L.title_size_px);
  ui_text_draw(ren, title, L.modal_x + (L.modal_w - title_w) / 2, L.modal_y + 18, L.title_size_px);

  // Per-row label, widget, and value text.
  for (int i = 0; i < SID_COUNT; ++i)
  {
    const SliderDef *def = &SLIDERS[i];
    const float v = read_value(app, i);

    SDL_FRect w;
    widget_rect(&L, i, &w);
    const int text_y = text_y_for_rect(w.y, w.h, L.text_size_px);

    // Label.
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    ui_text_draw(ren, def->label, L.label_x, text_y, L.text_size_px);

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
    }

    // Right-aligned numeric/state readout. Dropdowns already render the
    // current value inside the button, so don't repeat it here.
    if (def->kind != SK_DROPDOWN_UINT)
    {
      char buf[32];
      format_value(def, v, buf, sizeof(buf));
      SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
      ui_text_draw(ren, buf, L.value_x, text_y, L.text_size_px);
    }
  }

  // Open dropdown popup is drawn last so it overlaps later rows cleanly.
  if (c->open_dropdown >= 0)
  {
    const float v = read_value(app, c->open_dropdown);
    render_dropdown_popup(ren, &L, c->open_dropdown, v);
  }

  SDL_SetRenderDrawBlendMode(ren, prev_blend);
}
