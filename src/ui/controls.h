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

#ifndef UI_CONTROLS_H
#define UI_CONTROLS_H

#include <stdbool.h>

// Forward declarations to avoid pulling SDL/AppState into headers.
typedef union SDL_Event SDL_Event;
struct SDL_Renderer;
struct AppState;

// Settings modal with sliders for renderer + analyzer parameters.
// Rendered on top of the spectrum texture each frame, using SDL_Renderer
// primitives plus a built-in 5x7 bitmap font.
typedef struct Controls
{
  bool visible;

  // Index into the static slider table; -1 when none.
  int hovered;
  int dragging;

  // Currently open dropdown widget, or -1 when no dropdown is open.
  int open_dropdown;

  // Vertical scroll for the rows region, in logical pixels. Non-zero
  // only when the modal is too short to show every row. Clamped to
  // [0, max_scroll] each time the layout is computed.
  float scroll_offset;

  // True while the user is dragging the scrollbar thumb. Together with
  // the grab/start values, drives the proportional scroll math.
  bool scrollbar_dragging;
  float scrollbar_grab_y;
  float scrollbar_grab_scroll;
} Controls;

void controls_init(Controls *c);

// Toggle modal visibility (bound to a single keyboard shortcut).
void controls_toggle(Controls *c);

// Inspect/consume an SDL event. Returns true if the event should be
// considered consumed by the UI (e.g. mouse interaction with a slider, or
// the toggle key). The caller should still run its own quit handling.
bool controls_handle_event(Controls *c, const SDL_Event *ev, struct AppState *app);

// Renders the modal on top of the current SDL render target. Pulls the
// current parameter values directly from AppState. No-op when not visible.
void controls_render(Controls *c, struct SDL_Renderer *ren, const struct AppState *app);

#endif
