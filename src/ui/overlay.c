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

#include "overlay.h"

#include <math.h>
#include <stdio.h>

#include <SDL3/SDL.h>

#include "app_state.h"
#include "text.h"

static const char *NOTE_NAMES[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                     "F#", "G",  "G#", "A",  "A#", "B"};

static int pitch_class(int st)
{
  int pc = st % 12;
  if (pc < 0)
    pc += 12;
  return pc;
}

static int octave_of(int st)
{
  // f0 = C-1 (semitone 0). Scientific pitch: octave = floor(st/12) - 1.
  return (int)floor((double)st / 12.0) - 1;
}

static void semitone_to_note(int st, char *out, size_t cap)
{
  snprintf(out, cap, "%s%d", NOTE_NAMES[pitch_class(st)], octave_of(st));
}

static void semitone_to_note_f(float st, char *out, size_t cap)
{
  const int isemi = (int)floorf(st);
  const int pc = pitch_class(isemi);
  const int oct = octave_of(isemi);
  const float frac = st - (float)isemi;
  if (fabsf(frac - 0.5f) < 1e-4f)
    snprintf(out, cap, "%s%d plus half a semitone", NOTE_NAMES[pc], oct);
  else
    snprintf(out, cap, "%s%d", NOTE_NAMES[pc], oct);
}

static float semitone_to_x(float st, float min_st, float max_st, int win_w)
{
  const float range = max_st - min_st;
  if (range <= 0.0f)
    return 0.0f;
  return (st - min_st) / range * (float)win_w;
}

static float x_to_semitone(float x, float min_st, float max_st, int win_w)
{
  if (win_w <= 0)
    return 0.0f;
  return min_st + (x / (float)win_w) * (max_st - min_st);
}

static float semitone_to_freq(float st, float f0)
{
  return f0 * powf(2.0f, st / 12.0f);
}

static void draw_text_bl(SDL_Renderer *ren, const char *s, int x, int baseline_y, int size_px)
{
  const int line_h = ui_text_line_height(size_px);
  ui_text_draw(ren, s, x, baseline_y - line_h, size_px);
}

static void draw_text_bc(SDL_Renderer *ren, const char *s, int center_x, int baseline_y,
                         int size_px)
{
  const int tw = ui_text_width(s, size_px);
  const int line_h = ui_text_line_height(size_px);
  ui_text_draw(ren, s, center_x - tw / 2, baseline_y - line_h, size_px);
}

static void render_labels(SDL_Renderer *ren, const AppState *app, float min_st, float max_st)
{
  const int W = app->window_width;
  const int H = app->window_height;
  const float range = max_st - min_st;
  if (range <= 0.0f || W <= 0)
    return;

  const VisualizerConfig *vc = &app->visualizer_config;
  const float f0 = vc->f0;
  const float min_freq = semitone_to_freq(min_st, f0);
  const float max_freq = semitone_to_freq(max_st, f0);

  const int middle = H / 2;
  const int row_h = 20;
  const int size_px = UI_TEXT_SMALL;

  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);

  const int first_st = (int)ceilf(min_st);
  const int last_st = (int)floorf(max_st);
  for (int s = first_st; s <= last_st; ++s)
  {
    const float freq = semitone_to_freq((float)s, f0);
    if (freq < min_freq || freq > max_freq)
      continue;

    const float x = semitone_to_x((float)s, min_st, max_st, W);
    const int pc = pitch_class(s);
    // Matches: y = middle - rowHeight * (rowOffset + 1) + 6 * rowHeight
    const int baseline_y = middle - row_h * (pc + 1) + 6 * row_h;

    char buf[16];
    semitone_to_note(s, buf, sizeof(buf));
    draw_text_bc(ren, buf, (int)x, baseline_y, size_px);
  }

  char min_note[48];
  char max_note[48];
  semitone_to_note_f(min_st, min_note, sizeof(min_note));
  semitone_to_note_f(max_st, max_note, sizeof(max_note));

  char min_buf[160];
  char max_buf[160];
  snprintf(min_buf, sizeof(min_buf), "Min Freq: %.2f Hz (Semitone: %g - %s)", (double)min_freq,
           (double)min_st, min_note);
  snprintf(max_buf, sizeof(max_buf), "Max Freq: %.2f Hz (Semitone: %g - %s)", (double)max_freq,
           (double)max_st, max_note);

  const int footer_y = H - 60;
  draw_text_bl(ren, min_buf, 10, footer_y, size_px);
  draw_text_bl(ren, max_buf, W / 2 + 10, footer_y, size_px);
}

static void render_cursor(SDL_Renderer *ren, const AppState *app, float min_st, float max_st,
                          float f0)
{
  if (!app->mouse_in_window)
    return;
  const int W = app->window_width;
  const int H = app->window_height;
  const float mx = app->mouse_x;
  const float my = app->mouse_y;
  if (mx < 0.0f || mx > (float)W)
    return;

  SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
  SDL_FRect line = {mx, 0.0f, 1.0f, (float)H};
  SDL_RenderFillRect(ren, &line);

  const float st = x_to_semitone(mx, min_st, max_st, W);
  const float freq = semitone_to_freq(st, f0);
  const int st_round = (int)floorf(st + 0.5f);

  char note[16];
  semitone_to_note(st_round, note, sizeof(note));
  char freq_buf[32];
  char semi_buf[16];
  snprintf(freq_buf, sizeof(freq_buf), "%.2f Hz", (double)freq);
  snprintf(semi_buf, sizeof(semi_buf), "%.0f", (double)st);

  const int size_px = UI_TEXT_SMALL;
  const int tx = (int)mx + 10;
  draw_text_bl(ren, freq_buf, tx, (int)my - 20, size_px);
  draw_text_bl(ren, note, tx, (int)my - 5, size_px);
  draw_text_bl(ren, semi_buf, tx, (int)my + 10, size_px);
}

void overlay_render(SDL_Renderer *ren, const AppState *app)
{
  if (!ren || !app)
    return;
  const VisualizerConfig *vc = &app->visualizer_config;
  if (!vc->show_labels && !vc->show_cursor)
    return;
  if (app->window_width <= 0 || app->window_height <= 0)
    return;

  SDL_BlendMode prev_blend = SDL_BLENDMODE_NONE;
  SDL_GetRenderDrawBlendMode(ren, &prev_blend);
  SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);

  const float min_st = vc->min_semitone;
  const float max_st = vc->max_semitone;

  if (vc->show_labels && max_st > min_st)
    render_labels(ren, app, min_st, max_st);
  if (vc->show_cursor)
    render_cursor(ren, app, min_st, max_st, vc->f0);

  SDL_SetRenderDrawBlendMode(ren, prev_blend);
}
