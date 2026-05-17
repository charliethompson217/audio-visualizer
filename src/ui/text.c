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

// stb_truetype-backed text renderer. The font is loaded once from a
// list of candidate system paths; per requested pixel size a glyph
// atlas (ASCII 32..126) is baked into an SDL streaming texture and
// cached (LRU, bounded). ui_text_draw tints the atlas via
// SDL_SetTextureColorMod from the renderer's current draw color.

#include "text.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#include <SDL3/SDL.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#define ATLAS_W 512
#define ATLAS_H 512
#define GLYPH_FIRST 32
#define GLYPH_COUNT 95 // 32..126 inclusive
#define MAX_ATLASES 6

typedef struct Atlas
{
  int size_px;
  SDL_Texture *texture;
  stbtt_bakedchar chars[GLYPH_COUNT];
  uint64_t last_used;
} Atlas;

static unsigned char *g_font_data = NULL;
static stbtt_fontinfo g_font_info;
static bool g_font_loaded = false;
static Atlas g_atlases[MAX_ATLASES];
static int g_atlas_count = 0;
static uint64_t g_use_counter = 0;
// Ratio of physical pixels to the logical size_px callers pass in. Atlases
// are baked at size_px * g_pixel_scale physical pixels so that when their
// glyph quads are drawn into logical-point dst rects under SDL's logical
// presentation, the rasterizer lands one atlas pixel per physical pixel.
static float g_pixel_scale = 1.0f;

// Fast-path: well-known full paths we try in order before falling back to a
// recursive directory scan. Anything that loads as a TTF/OTF/TTC works; we
// don't require a monospace font.
static const char *FONT_CANDIDATES[] = {
    "/System/Library/Fonts/SFNSMono.ttf",
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Supplemental/Andale Mono.ttf",
    "/System/Library/Fonts/Supplemental/Courier New.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/cantarell/Cantarell-Regular.otf",
    "C:\\Windows\\Fonts\\consola.ttf",
    "C:\\Windows\\Fonts\\cour.ttf",
    "C:\\Windows\\Fonts\\segoeui.ttf",
    "C:\\Windows\\Fonts\\arial.ttf",
    NULL,
};

// Fallback: directories we recursively walk for any usable font file when no
// candidate path matched. Covers a fresh Arch/Hyprland install where the user
// has only installed e.g. JetBrains Mono via AUR or pulled in noto-fonts.
static const char *FONT_DIRS[] = {
#ifdef _WIN32
    "C:\\Windows\\Fonts",
#elif defined(__APPLE__)
    "/System/Library/Fonts",
    "/Library/Fonts",
#else
    "/usr/share/fonts",
    "/usr/local/share/fonts",
    "/run/current-system/sw/share/X11/fonts",
#endif
    NULL,
};

static unsigned char *read_file(const char *path, long *out_size)
{
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0)
  {
    fclose(f);
    return NULL;
  }
  long size = ftell(f);
  if (size <= 0)
  {
    fclose(f);
    return NULL;
  }
  rewind(f);
  unsigned char *buf = (unsigned char *)malloc((size_t)size);
  if (!buf)
  {
    fclose(f);
    return NULL;
  }
  if (fread(buf, 1, (size_t)size, f) != (size_t)size)
  {
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  *out_size = size;
  return buf;
}

static bool has_font_ext(const char *name)
{
  const char *dot = strrchr(name, '.');
  if (!dot || dot == name || strlen(dot) != 4)
    return false;
  char ext[5];
  for (int i = 0; i < 4; ++i)
    ext[i] = (char)tolower((unsigned char)dot[i]);
  ext[4] = '\0';
  return strcmp(ext, ".ttf") == 0 || strcmp(ext, ".otf") == 0 || strcmp(ext, ".ttc") == 0;
}

static bool try_load_font_file(const char *path)
{
  long size = 0;
  unsigned char *data = read_file(path, &size);
  if (!data)
    return false;
  int offset = stbtt_GetFontOffsetForIndex(data, 0);
  if (offset < 0 || !stbtt_InitFont(&g_font_info, data, offset))
  {
    free(data);
    return false;
  }
  g_font_data = data;
  g_font_loaded = true;
  return true;
}

#ifdef _WIN32
// Flat scan of a single directory. Windows fonts live at the top level of
// C:\Windows\Fonts so we don't need to recurse.
static bool scan_dir_for_font(const char *dir, int depth)
{
  (void)depth;
  char pattern[MAX_PATH];
  if (snprintf(pattern, sizeof pattern, "%s\\*", dir) <= 0)
    return false;
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  bool ok = false;
  do
  {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      continue;
    if (!has_font_ext(fd.cFileName))
      continue;
    char path[MAX_PATH];
    if (snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName) <= 0)
      continue;
    if (try_load_font_file(path))
    {
      ok = true;
      break;
    }
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return ok;
}
#else
// Bounded-depth recursive scan. Real font trees on Linux/macOS are shallow
// (usually 2-3 levels), but cap depth so a pathological symlink loop can't
// keep us recursing.
static bool scan_dir_for_font(const char *dir, int depth)
{
  if (depth > 4)
    return false;
  DIR *d = opendir(dir);
  if (!d)
    return false;
  bool ok = false;
  struct dirent *e;
  while ((e = readdir(d)) != NULL)
  {
    if (e->d_name[0] == '.')
      continue;
    char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
    if (n < 0 || (size_t)n >= sizeof path)
      continue;
    struct stat st;
    if (stat(path, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode))
    {
      if (scan_dir_for_font(path, depth + 1))
      {
        ok = true;
        break;
      }
    }
    else if (S_ISREG(st.st_mode) && has_font_ext(e->d_name) && try_load_font_file(path))
    {
      ok = true;
      break;
    }
  }
  closedir(d);
  return ok;
}
#endif

static bool load_font(void)
{
  for (const char **pp = FONT_CANDIDATES; *pp; ++pp)
    if (try_load_font_file(*pp))
      return true;

  for (const char **pp = FONT_DIRS; *pp; ++pp)
    if (scan_dir_for_font(*pp, 0))
      return true;

#ifndef _WIN32
  // Also try the user's per-account font directories. XDG_DATA_HOME wins over
  // HOME if it's set (matches the spec); fall back to ~/.local/share/fonts and
  // the legacy ~/.fonts location.
  const char *xdg = getenv("XDG_DATA_HOME");
  const char *home = getenv("HOME");
  char buf[1024];
  if (xdg && *xdg)
  {
    int n = snprintf(buf, sizeof buf, "%s/fonts", xdg);
    if (n > 0 && (size_t)n < sizeof buf && scan_dir_for_font(buf, 0))
      return true;
  }
  if (home && *home)
  {
    static const char *user_subdirs[] = {".local/share/fonts", ".fonts", NULL};
    for (const char **sp = user_subdirs; *sp; ++sp)
    {
      int n = snprintf(buf, sizeof buf, "%s/%s", home, *sp);
      if (n > 0 && (size_t)n < sizeof buf && scan_dir_for_font(buf, 0))
        return true;
    }
  }
#endif
  return false;
}

bool ui_text_init(struct SDL_Renderer *ren)
{
  (void)ren;
  if (g_font_loaded)
    return true;
  memset(g_atlases, 0, sizeof(g_atlases));
  g_atlas_count = 0;
  g_use_counter = 0;
  return load_font();
}

void ui_text_shutdown(void)
{
  for (int i = 0; i < g_atlas_count; ++i)
  {
    if (g_atlases[i].texture)
      SDL_DestroyTexture(g_atlases[i].texture);
  }
  memset(g_atlases, 0, sizeof(g_atlases));
  g_atlas_count = 0;
  free(g_font_data);
  g_font_data = NULL;
  g_font_loaded = false;
}

void ui_text_set_pixel_scale(float scale)
{
  if (scale <= 0.0f)
    scale = 1.0f;
  if (scale == g_pixel_scale)
    return;
  g_pixel_scale = scale;
  // Existing atlases were baked at the old scale; drop them so the next
  // draw rebakes at the new physical size.
  for (int i = 0; i < g_atlas_count; ++i)
  {
    if (g_atlases[i].texture)
      SDL_DestroyTexture(g_atlases[i].texture);
  }
  memset(g_atlases, 0, sizeof(g_atlases));
  g_atlas_count = 0;
}

// ---------------------------------------------------------------------------
// Per-size atlas bake + LRU cache.
// ---------------------------------------------------------------------------

static SDL_Texture *bake_atlas_texture(SDL_Renderer *ren, int size_px, stbtt_bakedchar *chars_out)
{
  unsigned char *bitmap = (unsigned char *)calloc(ATLAS_W * ATLAS_H, 1);
  if (!bitmap)
    return NULL;
  const float baked_size = (float)size_px * g_pixel_scale;
  const int rc = stbtt_BakeFontBitmap(g_font_data, 0, baked_size, bitmap, ATLAS_W, ATLAS_H,
                                      GLYPH_FIRST, GLYPH_COUNT, chars_out);
  if (rc == 0)
  {
    free(bitmap);
    return NULL;
  }
  // Expand single-channel alpha to RGBA (white tint preserved, alpha = src).
  uint32_t *rgba = (uint32_t *)malloc((size_t)ATLAS_W * ATLAS_H * sizeof(uint32_t));
  if (!rgba)
  {
    free(bitmap);
    return NULL;
  }
  for (int i = 0; i < ATLAS_W * ATLAS_H; ++i)
    rgba[i] = ((uint32_t)bitmap[i] << 24) | 0x00FFFFFFu;
  free(bitmap);

  SDL_Texture *tex =
      SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, ATLAS_W, ATLAS_H);
  if (!tex)
  {
    free(rgba);
    return NULL;
  }
  SDL_UpdateTexture(tex, NULL, rgba, ATLAS_W * (int)sizeof(uint32_t));
  free(rgba);
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
  // The atlas is rasterized by stb_truetype at exactly the pixel size the
  // caller will draw at; the glyphs already carry their own anti-aliased
  // edges. Sampling with NEAREST avoids the extra bilinear blur SDL would
  // otherwise apply at draw time (visible as soft, washed-out text even
  // when source and destination rects are 1:1 in size).
  SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
  return tex;
}

static Atlas *get_or_bake_atlas(SDL_Renderer *ren, int size_px)
{
  if (!g_font_loaded || size_px <= 0)
    return NULL;
  for (int i = 0; i < g_atlas_count; ++i)
  {
    if (g_atlases[i].size_px == size_px && g_atlases[i].texture)
    {
      g_atlases[i].last_used = ++g_use_counter;
      return &g_atlases[i];
    }
  }

  int slot;
  if (g_atlas_count < MAX_ATLASES)
  {
    slot = g_atlas_count++;
  }
  else
  {
    slot = 0;
    for (int i = 1; i < g_atlas_count; ++i)
      if (g_atlases[i].last_used < g_atlases[slot].last_used)
        slot = i;
    if (g_atlases[slot].texture)
      SDL_DestroyTexture(g_atlases[slot].texture);
    memset(&g_atlases[slot], 0, sizeof(Atlas));
  }

  Atlas *a = &g_atlases[slot];
  a->size_px = size_px;
  a->texture = bake_atlas_texture(ren, size_px, a->chars);
  if (!a->texture)
  {
    // Shrink the cache back so we don't poison a slot.
    if (slot == g_atlas_count - 1)
      g_atlas_count--;
    return NULL;
  }
  a->last_used = ++g_use_counter;
  return a;
}

int ui_text_ascent(int size_px)
{
  if (!g_font_loaded || size_px <= 0)
    return 0;
  int a, d, l;
  stbtt_GetFontVMetrics(&g_font_info, &a, &d, &l);
  const float scale = stbtt_ScaleForPixelHeight(&g_font_info, (float)size_px);
  return (int)ceilf((float)a * scale);
}

int ui_text_line_height(int size_px)
{
  if (!g_font_loaded || size_px <= 0)
    return size_px;
  int a, d, l;
  stbtt_GetFontVMetrics(&g_font_info, &a, &d, &l);
  const float scale = stbtt_ScaleForPixelHeight(&g_font_info, (float)size_px);
  return (int)ceilf(((float)(a - d) + (float)l) * scale);
}

// Width helper that doesn't need a renderer: uses font metrics directly.
// This avoids forcing a renderer parameter into ui_text_width and keeps the
// API symmetrical with the previous bitmap implementation.
int ui_text_width(const char *s, int size_px)
{
  if (!g_font_loaded || !s || size_px <= 0)
    return 0;
  const float scale = stbtt_ScaleForPixelHeight(&g_font_info, (float)size_px);
  float fx = 0.0f;
  for (const char *p = s; *p; ++p)
  {
    unsigned char c = (unsigned char)*p;
    if (c < GLYPH_FIRST || c >= GLYPH_FIRST + GLYPH_COUNT)
      continue;
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_font_info, c, &adv, &lsb);
    fx += (float)adv * scale;
    const unsigned char next = (unsigned char)*(p + 1);
    if (next)
      fx += (float)stbtt_GetCodepointKernAdvance(&g_font_info, c, next) * scale;
  }
  return (int)ceilf(fx);
}

void ui_text_draw(SDL_Renderer *ren, const char *s, int x, int y, int size_px)
{
  if (!ren || !s || !g_font_loaded || size_px <= 0)
    return;
  Atlas *a = get_or_bake_atlas(ren, size_px);
  if (!a)
    return;

  Uint8 cr = 255, cg = 255, cb = 255, ca = 255;
  SDL_GetRenderDrawColor(ren, &cr, &cg, &cb, &ca);
  SDL_SetTextureColorMod(a->texture, cr, cg, cb);
  SDL_SetTextureAlphaMod(a->texture, ca);

  // The atlas was baked at size_px * g_pixel_scale physical pixels, so its
  // metrics (xoff/yoff/xadvance encoded in stbtt_bakedchar) are in that
  // larger coordinate space. Walk the pen there, then divide the resulting
  // quad geometry back down to logical points for the destination rect.
  const float scale = g_pixel_scale;
  const float inv_scale = 1.0f / scale;
  int asc_phys = 0, desc = 0, lgap = 0;
  stbtt_GetFontVMetrics(&g_font_info, &asc_phys, &desc, &lgap);
  const float vscale = stbtt_ScaleForPixelHeight(&g_font_info, (float)size_px * scale);
  float fx = (float)x * scale;
  const float fy = (float)y * scale + ceilf((float)asc_phys * vscale);
  for (const char *p = s; *p; ++p)
  {
    unsigned char c = (unsigned char)*p;
    if (c < GLYPH_FIRST || c >= GLYPH_FIRST + GLYPH_COUNT)
      continue;
    stbtt_aligned_quad q;
    float pen_y = fy;
    stbtt_GetBakedQuad(a->chars, ATLAS_W, ATLAS_H, c - GLYPH_FIRST, &fx, &pen_y, &q, 1);
    SDL_FRect src = {q.s0 * (float)ATLAS_W, q.t0 * (float)ATLAS_H, (q.s1 - q.s0) * (float)ATLAS_W,
                     (q.t1 - q.t0) * (float)ATLAS_H};
    SDL_FRect dst = {q.x0 * inv_scale, q.y0 * inv_scale, (q.x1 - q.x0) * inv_scale,
                     (q.y1 - q.y0) * inv_scale};
    SDL_RenderTexture(ren, a->texture, &src, &dst);
  }
}
