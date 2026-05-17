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

#ifndef PLATFORM_PATHS_H
#define PLATFORM_PATHS_H

#include <stdbool.h>
#include <stddef.h>

// Fills `out` with the absolute path to the settings.conf for this OS,
// creating the parent directory tree if missing. Returns false when the
// environment is missing the expected home/appdata variable or when
// directory creation fails; `out` is set to an empty string in that case.
//
// Paths per platform:
//   macOS:   $HOME/Library/Application Support/audiovisualizer/settings.conf
//   Windows: %APPDATA%\audiovisualizer\settings.conf
//   Linux:   ${XDG_CONFIG_HOME:-$HOME/.config}/audiovisualizer/settings.conf
//
// macOS App Sandbox automatically redirects $HOME into the per-app
// container, so no extra handling is needed for sandboxed builds.
bool paths_settings_file(char *out, size_t out_cap);

#endif
