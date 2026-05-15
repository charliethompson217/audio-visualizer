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

#ifndef UI_OVERLAY_H
#define UI_OVERLAY_H

struct SDL_Renderer;
struct AppState;

// In-canvas overlay drawn between the spectrum texture and the settings
// modal. Renders octave note labels along the frequency axis (when
// VisualizerConfig.show_labels is set) and a vertical cursor line with a
// frequency/note/semitone readout at the current mouse position (when
// VisualizerConfig.show_cursor is set). No-op when both toggles are off.
void overlay_render(struct SDL_Renderer *ren, const struct AppState *app);

#endif
