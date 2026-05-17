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

#include "paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define PATH_SEP '\\'
#define IS_DIR(m) (((m) & _S_IFDIR) != 0)
#else
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0755)
#define PATH_SEP '/'
#define IS_DIR(m) (((m) & S_IFDIR) != 0)
#endif

// mkdir -p: walk path segments left-to-right, mkdir each. EEXIST is
// expected and ignored; only the final stat() determines success. The
// leading separator (POSIX) or drive prefix (Windows) is skipped over so
// we don't attempt mkdir("") or mkdir("C:").
static bool ensure_dir(const char *path)
{
  if (!path || !*path)
    return false;

  char buf[1024];
  const size_t n = strlen(path);
  if (n >= sizeof(buf))
    return false;
  memcpy(buf, path, n + 1);

  // Start past the root: '/' on POSIX, drive letter + ':' + sep on Windows.
  size_t start = 0;
#if defined(_WIN32)
  if (n >= 2 && buf[1] == ':')
    start = (n >= 3 && (buf[2] == '\\' || buf[2] == '/')) ? 3 : 2;
#else
  if (buf[0] == '/')
    start = 1;
#endif

  for (size_t i = start; i < n; ++i)
  {
    if (buf[i] == PATH_SEP || buf[i] == '/')
    {
      const char saved = buf[i];
      buf[i] = '\0';
      MKDIR(buf);
      buf[i] = saved;
    }
  }
  MKDIR(buf);

  struct stat st;
  if (stat(buf, &st) != 0)
    return false;
  return IS_DIR(st.st_mode);
}

bool paths_settings_file(char *out, size_t out_cap)
{
  if (!out || out_cap < 2)
    return false;
  out[0] = '\0';

  char dir[1024];
  int n;

#if defined(_WIN32)
  const char *appdata = getenv("APPDATA");
  if (!appdata || !*appdata)
    return false;
  n = snprintf(dir, sizeof(dir), "%s\\audiovisualizer", appdata);
#elif defined(__APPLE__)
  const char *home = getenv("HOME");
  if (!home || !*home)
    return false;
  n = snprintf(dir, sizeof(dir), "%s/Library/Application Support/audiovisualizer", home);
#else
  const char *xdg = getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg)
  {
    n = snprintf(dir, sizeof(dir), "%s/audiovisualizer", xdg);
  }
  else
  {
    const char *home = getenv("HOME");
    if (!home || !*home)
      return false;
    n = snprintf(dir, sizeof(dir), "%s/.config/audiovisualizer", home);
  }
#endif

  if (n < 0 || n >= (int)sizeof(dir))
    return false;
  if (!ensure_dir(dir))
    return false;

  n = snprintf(out, out_cap, "%s%csettings.conf", dir, PATH_SEP);
  if (n < 0 || n >= (int)out_cap)
  {
    out[0] = '\0';
    return false;
  }
  return true;
}
