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

#include "config.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "platform/paths.h"

#define CONFIG_VERSION 1

// ---------------------------------------------------------------------------
// Tiny key=value parser. Lines starting with '#' or ';' are comments; blank
// lines are skipped. Values are trimmed of leading/trailing whitespace.
// Unknown keys are ignored so older configs survive a schema bump.
// ---------------------------------------------------------------------------

static char *trim(char *s)
{
  if (!s)
    return s;
  while (*s && isspace((unsigned char)*s))
    ++s;
  char *end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1]))
    --end;
  *end = '\0';
  return s;
}

static bool parse_int(const char *s, int *out)
{
  if (!s || !*s)
    return false;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (end == s)
    return false;
  *out = (int)v;
  return true;
}

static bool parse_uint(const char *s, uint32_t *out)
{
  int v;
  if (!parse_int(s, &v) || v < 0)
    return false;
  *out = (uint32_t)v;
  return true;
}

static bool parse_float(const char *s, float *out)
{
  if (!s || !*s)
    return false;
  char *end = NULL;
  float v = strtof(s, &end);
  if (end == s)
    return false;
  *out = v;
  return true;
}

static bool parse_bool(const char *s, bool *out)
{
  int v;
  if (!parse_int(s, &v))
    return false;
  *out = (v != 0);
  return true;
}

// Parses a comma-separated list of up to `cap` floats into `out`. Returns
// the number actually read; values beyond `cap` are discarded.
static int parse_float_list(const char *s, float *out, int cap)
{
  int n = 0;
  const char *p = s;
  while (p && *p && n < cap)
  {
    char *end = NULL;
    float v = strtof(p, &end);
    if (end == p)
      break;
    out[n++] = v;
    p = end;
    while (*p == ',' || isspace((unsigned char)*p))
      ++p;
  }
  return n;
}

// Apply a single key=value pair to AppState. Unknown keys are no-ops.
static void apply_kv(AppState *app, const char *key, const char *val)
{
  if (strcmp(key, "version") == 0)
    return; // schema marker; ignored at load time
  if (strcmp(key, "ui.show_hint") == 0)
  {
    bool v;
    if (parse_bool(val, &v))
      app->controls.show_hint = v;
    return;
  }
  if (strcmp(key, "source.kind") == 0)
  {
    uint32_t v;
    if (parse_uint(val, &v) && v <= AUDIO_SOURCE_SYSTEM)
      app->source_config.kind = (AudioSourceKind)v;
    return;
  }
  if (strcmp(key, "source.file_path") == 0)
  {
    snprintf(app->current_file_path, sizeof(app->current_file_path), "%s", val);
    return;
  }
  if (strcmp(key, "window.width") == 0)
  {
    int v;
    if (parse_int(val, &v) && v >= 480)
      app->window_width = v;
    return;
  }
  if (strcmp(key, "window.height") == 0)
  {
    int v;
    if (parse_int(val, &v) && v >= 240)
      app->window_height = v;
    return;
  }
  VisualizerConfig *vc = &app->visualizer_config;
  if (strcmp(key, "visualizer.brightness_power") == 0)
  {
    parse_float(val, &vc->brightness_power);
    return;
  }
  if (strcmp(key, "visualizer.length_power") == 0)
  {
    parse_float(val, &vc->length_power);
    return;
  }
  if (strcmp(key, "visualizer.min_semitone") == 0)
  {
    parse_float(val, &vc->min_semitone);
    return;
  }
  if (strcmp(key, "visualizer.max_semitone") == 0)
  {
    parse_float(val, &vc->max_semitone);
    return;
  }
  if (strcmp(key, "visualizer.show_labels") == 0)
  {
    parse_bool(val, &vc->show_labels);
    return;
  }
  if (strcmp(key, "visualizer.show_cursor") == 0)
  {
    parse_bool(val, &vc->show_cursor);
    return;
  }
  if (strcmp(key, "visualizer.show_spectrum") == 0)
  {
    parse_bool(val, &vc->show_spectrum);
    return;
  }
  if (strcmp(key, "visualizer.show_waveform") == 0)
  {
    parse_bool(val, &vc->show_waveform);
    return;
  }
  if (strcmp(key, "visualizer.waveform_gain") == 0)
  {
    parse_float(val, &vc->waveform_gain);
    return;
  }
  if (strcmp(key, "visualizer.waveform_samples_per_px") == 0)
  {
    parse_float(val, &vc->waveform_samples_per_px);
    return;
  }
  if (strcmp(key, "visualizer.f0") == 0)
  {
    parse_float(val, &vc->f0);
    return;
  }
  if (strcmp(key, "visualizer.note_hues") == 0)
  {
    parse_float_list(val, vc->note_hues, 12);
    return;
  }
  AnalyzerConfig *ac = &app->analyzer_config;
  if (strcmp(key, "analyzer.fft_size") == 0)
  {
    parse_uint(val, &ac->fft_size);
    return;
  }
  if (strcmp(key, "analyzer.min_db") == 0)
  {
    parse_float(val, &ac->min_db);
    return;
  }
  if (strcmp(key, "analyzer.max_db") == 0)
  {
    parse_float(val, &ac->max_db);
    return;
  }
  if (strcmp(key, "analyzer.smoothing") == 0)
  {
    parse_float(val, &ac->smoothing);
    return;
  }
  if (strcmp(key, "analyzer.window_function") == 0)
  {
    uint32_t v;
    if (parse_uint(val, &v))
      ac->window_function = (WindowFunction)v;
    return;
  }
}

bool config_load(AppState *app)
{
  if (!app)
    return false;
  char path[1024];
  if (!paths_settings_file(path, sizeof(path)))
    return false;

  FILE *f = fopen(path, "r");
  if (!f)
    return false;

  char line[2048];
  while (fgets(line, sizeof(line), f))
  {
    char *p = trim(line);
    if (!*p || *p == '#' || *p == ';')
      continue;
    char *eq = strchr(p, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *key = trim(p);
    char *val = trim(eq + 1);
    apply_kv(app, key, val);
  }
  fclose(f);
  return true;
}

bool config_save(const AppState *app)
{
  if (!app)
    return false;
  char path[1024];
  if (!paths_settings_file(path, sizeof(path)))
    return false;

  FILE *f = fopen(path, "w");
  if (!f)
    return false;

  const VisualizerConfig *vc = &app->visualizer_config;
  const AnalyzerConfig *ac = &app->analyzer_config;

  fprintf(f, "version=%d\n", CONFIG_VERSION);
  fprintf(f, "ui.show_hint=%d\n", app->controls.show_hint ? 1 : 0);
  // FILE is intentionally not persisted: a renamed/moved file would lock
  // the app in an error state on the next launch with no in-UI recovery
  // path. Coerce FILE to DEMO and skip writing the path so every launch
  // starts with a known-good source and a clean file-picker slate.
  const AudioSourceKind persisted_kind =
      (app->source_config.kind == AUDIO_SOURCE_FILE) ? AUDIO_SOURCE_DEMO : app->source_config.kind;
  fprintf(f, "source.kind=%u\n", (unsigned)persisted_kind);
  fprintf(f, "window.width=%d\n", app->window_width);
  fprintf(f, "window.height=%d\n", app->window_height);
  fprintf(f, "visualizer.brightness_power=%.4f\n", (double)vc->brightness_power);
  fprintf(f, "visualizer.length_power=%.4f\n", (double)vc->length_power);
  fprintf(f, "visualizer.min_semitone=%.4f\n", (double)vc->min_semitone);
  fprintf(f, "visualizer.max_semitone=%.4f\n", (double)vc->max_semitone);
  fprintf(f, "visualizer.show_labels=%d\n", vc->show_labels ? 1 : 0);
  fprintf(f, "visualizer.show_cursor=%d\n", vc->show_cursor ? 1 : 0);
  fprintf(f, "visualizer.show_spectrum=%d\n", vc->show_spectrum ? 1 : 0);
  fprintf(f, "visualizer.show_waveform=%d\n", vc->show_waveform ? 1 : 0);
  fprintf(f, "visualizer.waveform_gain=%.4f\n", (double)vc->waveform_gain);
  fprintf(f, "visualizer.waveform_samples_per_px=%.4f\n", (double)vc->waveform_samples_per_px);
  fprintf(f, "visualizer.f0=%.6f\n", (double)vc->f0);
  fprintf(f, "visualizer.note_hues=");
  for (int i = 0; i < 12; ++i)
    fprintf(f, "%s%.2f", i ? "," : "", (double)vc->note_hues[i]);
  fprintf(f, "\n");
  fprintf(f, "analyzer.fft_size=%u\n", ac->fft_size);
  fprintf(f, "analyzer.min_db=%.2f\n", (double)ac->min_db);
  fprintf(f, "analyzer.max_db=%.2f\n", (double)ac->max_db);
  fprintf(f, "analyzer.smoothing=%.4f\n", (double)ac->smoothing);
  fprintf(f, "analyzer.window_function=%u\n", (unsigned)ac->window_function);

  fclose(f);
  return true;
}
