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

#ifndef UI_TEXT_H
#define UI_TEXT_H

#include <stdbool.h>

struct SDL_Renderer;

// TrueType text shared by the settings modal and the in-canvas overlay
// (note labels, cursor readouts). Loads a system font via stb_truetype
// and bakes a glyph atlas per requested pixel size into an SDL texture
// (cached, bounded). Sizes throughout the API are in render-target
// pixels; the renderer's current draw color is used as a tint.

// Standard sizes used by the UI. Add more values if you need them; each
// distinct size triggers one atlas bake on first use.
#define UI_TEXT_SMALL 14
#define UI_TEXT_MEDIUM 18
#define UI_TEXT_LARGE 22
#define UI_TEXT_TITLE 28

// Loads a TTF font (system path fallbacks) and prepares atlas storage
// against `ren`. Returns false if no usable font was found. Subsequent
// draw/measure calls become no-ops when this returns false.
bool ui_text_init(struct SDL_Renderer *ren);

// Releases atlas textures and the font buffer. Safe to call multiple
// times; matches a single successful ui_text_init.
void ui_text_shutdown(void);

// Total line height (ascent + descent + line gap) at `size_px`. Use for
// vertical centering against a box of known height.
int ui_text_line_height(int size_px);

// Ascent (pixels from the position passed to ui_text_draw to the visual
// top of capital letters). Use to convert "top of text" coordinates to
// the baseline expected by the rasterizer.
int ui_text_ascent(int size_px);

// Advance width of `s` if rendered at `size_px` pixels. ASCII only;
// non-printable characters are skipped.
int ui_text_width(const char *s, int size_px);

// Draws `s` at (x, y) where y is the top of the text cell. Tints to the
// renderer's current draw color (incl. alpha). No-op when text init
// failed.
void ui_text_draw(struct SDL_Renderer *ren, const char *s, int x, int y, int size_px);

#endif
