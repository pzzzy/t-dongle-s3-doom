//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 1993-2008 Raven Software
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2021-2022 Graham Sanderson
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Gamma correction LUT stuff.
//	Functions to draw patches (by post) directly to screen.
//	Functions to blit a block to the screen.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "i_system.h"

#include "doomtype.h"

#include "deh_str.h"
#include "i_input.h"
#include "i_swap.h"
#include "i_video.h"
#include "m_bbox.h"
#include "m_misc.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#if USE_WHD

static int vpatch_dest_stride = SCREENWIDTH;
static int vpatch_dest_origin_y;

#include "doom/r_data.h"

static_assert(VPATCH_NAME_INVALID == 0, "");
//static_assert(NUM_VPATCHES < 256, "");
// ^ note we have relaxed this because we are close... to allowing for sequential patches (just added to the base) to extend beyond our vpatch_small_t size which is one byte
static_assert(VPATCH_STCFN033 < 256, "");
#endif

#include "config.h"

#ifdef HAVE_LIBPNG
#include <png.h>
#endif

#ifndef NDEBUG
// TODO: There are separate RANGECHECK defines for different games, but this
// is common code. Fix this.
#define RANGECHECK
#endif

// Blending table used for fuzzpatch, etc.
// Only used in Heretic/Hexen
#if !DOOM_ONLY
should_be_const byte *tinttable = NULL;
#endif

#if USE_WHD
const uint8_t vpatch_for_shared_palette[NUM_SHARED_PALETTES] = {VPATCH_STBAR, VPATCH_STCFN033, VPATCH_WIBP1};
static const uint8_t *shared_palette8[NUM_SHARED_PALETTES];
#endif
// villsa [STRIFE] Blending table used for Strife
byte *xlatab = NULL;

// The screen buffer that the v_video.c code draws to.

static pixel_t *dest_screen = NULL;
uint8_t vpatch_clip_top, vpatch_clip_bottom = SCREENHEIGHT;

#if USE_WHD
vpatchlist_t *vpatchlist;
#else
int dirtybox[4];
#endif

#if !DOOM_ONLY
// haleyjd 08/28/10: clipping callback function for patches.
// This is needed for Chocolate Strife, which clips patches to the screen.
static vpatchclipfunc_t patchclip_callback = NULL;
#endif

//
// V_MarkRect 
// 
#if !USE_WHD
void V_MarkRect(int x, int y, int width, int height)
{
    // If we are temporarily using an alternate screen, do not
    // affect the update box.

    if (dest_screen == I_VideoBuffer)
    {
        M_AddToBox (dirtybox, x, y); 
        M_AddToBox (dirtybox, x + width-1, y + height-1); 
    }
}
#endif

//
// V_CopyRect 
// 
void V_CopyRect(int srcx, int srcy, pixel_t *source,
                int width, int height,
                int destx, int desty) {
    pixel_t *src;
    pixel_t *dest;

#ifdef RANGECHECK
    if (srcx < 0
        || srcx + width > SCREENWIDTH
        || srcy < 0
        || srcy + height > SCREENHEIGHT
        || destx < 0
        || destx + width > SCREENWIDTH
        || desty < 0
        || desty + height > SCREENHEIGHT) {
        I_Error("Bad V_CopyRect");
    }
#endif

    V_MarkRect(destx, desty, width, height);

    src = source + SCREENWIDTH * srcy + srcx;
    dest = dest_screen + SCREENWIDTH * desty + destx;

    for (; height > 0; height--) {
        memcpy(dest, src, width * sizeof(*dest));
        src += SCREENWIDTH;
        dest += SCREENWIDTH;
    }
}

#if !DOOM_ONLY
//
// V_SetPatchClipCallback
//
// haleyjd 08/28/10: Added for Strife support.
// By calling this function, you can setup runtime error checking for patch 
// clipping. Strife never caused errors by drawing patches partway off-screen.
// Some versions of vanilla DOOM also behaved differently than the default
// implementation, so this could possibly be extended to those as well for
// accurate emulation.
//
void V_SetPatchClipCallback(vpatchclipfunc_t func)
{
    patchclip_callback = func;
}
#endif
//
// V_DrawPatch
// Masks a column based masked pic to the screen. 
//

#if USE_WHD

void V_BeginPatchList(vpatchlist_t *patchlist) {
    vpatchlist = patchlist;
    vpatchlist->header.size = 1;
}

void V_EndPatchList(void) {
    vpatchlist = 0;
}

static void V_InitSharedPalettes(void)
{
    if (shared_palette8[0]) return;
    for (int i = 0; i < NUM_SHARED_PALETTES; i++) {
        patch_t *patch = resolve_vpatch_handle(vpatch_for_shared_palette[i]);
        assert(patch);
        assert(vpatch_has_shared_palette(patch));
        assert(vpatch_colorcount(patch));
        shared_palette8[i] = vpatch_palette(patch);
    }
}

#pragma GCC push_options
#if PICO_ON_DEVICE
#pragma GCC optimize("O3")
#endif

void V_DrawPatchList(const vpatchlist_t *patchlist) {
    V_InitSharedPalettes();
    for (int l = 1; l < patchlist[0].header.size; l++) {
        uint8_t *orig = dest_screen
                + ((int)patchlist[l].entry.y - vpatch_dest_origin_y)
                  * vpatch_dest_stride
                + patchlist[l].entry.x;
        const patch_t *patch = resolve_vpatch_handle(patchlist[l].entry.patch_handle);
        const uint8_t *pal;
        if (!vpatch_has_shared_palette(patch)) {
            pal = vpatch_palette(patch);
        } else {
            assert(vpatch_shared_palette(patch) < NUM_SHARED_PALETTES);
            pal = shared_palette8[vpatch_shared_palette(patch)];
        }
        int repeat = patchlist[l].entry.repeat;
        int w = vpatch_width(patch);
        int h0 = vpatch_height(patch);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
        int skip_top = 0;
#pragma GCC diagnostic pop
        int type = vpatch_type(patch);
        if (patchlist[l].entry.y + h0 > vpatch_clip_bottom) {
            // clipping bottom which is trivial
            h0 = vpatch_clip_bottom - patchlist[l].entry.y;
            if (h0 <= 0) continue;
        }
        if (patchlist[l].entry.y < vpatch_clip_top) {
            skip_top = vpatch_clip_top - patchlist[l].entry.y;
            if (skip_top >= h0) continue;
            h0 -= skip_top;
            type += vp4_runs_clipped - vp4_runs;
        }
        const uint8_t *data = vpatch_data(patch);
        uint8_t *desttop = orig;
        int h = h0;
        switch (type) {
            case vp4_runs_clipped:
                for(;skip_top--; desttop += vpatch_dest_stride) {
                    uint8_t gap;
                    int p = 0;
                    while (0xff != (gap = *data++)) {
                        p += gap;
                        int len = *data++;
                        for (int i = 1; i < len; i += 2) {
                            p += 2;
                            data++;
                        }
                        if (len & 1) {
                            p++;
                            data++;
                        }
                        assert(p <= w);
                        if (p == w) break;
                    }
                }
                // fall thru
            case vp4_runs:
                for (; h > 0; h--, desttop += vpatch_dest_stride) {
                    uint8_t *p = desttop;
                    uint8_t *pend = desttop + w;
                    uint8_t gap;
                    while (0xff != (gap = *data++)) {
                        p += gap;
                        int len = *data++;
                        for (int i = 1; i < len; i += 2) {
                            uint v = *data++;
                            *p++ = pal[v & 0xf];
                            *p++ = pal[v >> 4];
                        }
                        if (len & 1) {
                            *p++ = pal[(*data++) & 0xf];
                        }
                        assert(p <= pend);
                        if (p == pend) break;
                    }
                }
                break;
            case vp4_alpha_clipped:
                data += ((w + 1) / 2) * skip_top;
                desttop += vpatch_dest_stride * skip_top;
                // fallthru
            case vp4_alpha:
                for (; h > 0; h--, desttop += vpatch_dest_stride) {
                    uint8_t *p = desttop;
                    for (int i = 0; i < w / 2; i++) {
                        uint v = *data++;
                        if (v & 0xf) p[0] = pal[v & 0xf];
                        if (v >> 4) p[1] = pal[v >> 4];
                        p += 2;
                    }
                    if (w & 1) {
                        uint v = *data++;
                        if (v & 0xf) p[0] = pal[v & 0xf];
                    }
                }
                break;
            case vp4_solid:
                for (; h > 0; h--, desttop += vpatch_dest_stride) {
                    uint8_t *p = desttop;
                    for (int i = 0; i < w / 2; i++) {
                        uint v = *data++;
                        p[0] = pal[v & 0xf];
                        p[1] = pal[v >> 4];
                        p += 2;
                    }
                    if (w & 1) {
                        uint v = *data++;
                        p[0] = pal[v & 0xf];
                    }
                }
                break;
            case vp6_runs_clipped:
                // todo implement this (perhaps needed for multi player?)
                continue;
            case vp6_runs:
                for (; h > 0; h--, desttop += vpatch_dest_stride) {
                    uint8_t *p = desttop;
                    uint8_t *pend = desttop + w;
                    uint8_t gap;
                    while (0xff != (gap = *data++)) {
                        p += gap;
                        int len = *data++;
                        for (int i = 3; i < len; i += 4) {
                            uint v = *data++;
                            v |= (*data++) << 8;
                            v |= (*data++) << 16;
                            *p++ = pal[v & 0x3f];
                            *p++ = pal[(v >> 6) & 0x3f];
                            *p++ = pal[(v >> 12) & 0x3f];
                            *p++ = pal[(v >> 18) & 0x3f];
                        }
                        len &= 3;
                        if (len--) {
                            uint v = *data++;
                            *p++ = pal[v & 0x3f];
                            if (len--) {
                                v >>= 6;
                                v |= (*data++) << 2;
                                *p++ = pal[v & 0x3f];
                                if (len--) {
                                    v >>= 6;
                                    v |= (*data++) << 4;
                                    *p++ = pal[v & 0x3f];
                                    assert(!len);
                                }
                            }
                        }
                        assert(p <= pend);
                        if (p == pend) break;
                    }
                }
                break;
            case vp8_runs:
                for (; h > 0; h--, desttop += vpatch_dest_stride) {
                    uint8_t *p = desttop;
                    uint8_t *pend = desttop + w;
                    uint8_t gap;
                    while (0xff != (gap = *data++)) {
                        p += gap;
                        int len = *data++;
                        for (int i = 0; i < len; i++) {
                            *p++ = pal[*data++];
                        }
                        assert(p <= pend);
                        if (p == pend) break;
                    }
                }
                break;
            case vp_border_clipped:
                data += 3 * skip_top;
                // fall thru
            case vp_border: {
                for (; h > 0; h--, desttop += vpatch_dest_stride) {
                    desttop[0] = data[0];
                    for (int i = 1; i < w - 1; i++) desttop[i] = data[1];
                    desttop[w - 1] = data[2];
                    data += 3;
                }
                break;
            }
            default:
                // these two we ignore for now
                assert(type == vp8_runs_clipped || type == vp4_solid_clipped);
                continue;
        }
        if (repeat) {
            // we need them to be solid... which they are, but if not you'll just get some visual funk
            //assert(vpatch_type(patch) == vp4_solid);
            h = h0;
            if (patchlist[l].entry.patch_handle == VPATCH_M_THERMM) w--; // hackity hack
            uint8_t *desttop = orig;
            for(;h>0;h--) {
                for (int i = 0; i < repeat * w; i++) {
                    desttop[w + i] = desttop[i];
                }
                desttop += vpatch_dest_stride;
            }
        }
    }
}

// Decode one row of the compact, row-major WHD vpatch format. Transparent
// pixels are returned as -1, otherwise values are PLAYPAL indexes. This is a
// deliberately cold path used for menus on displays smaller than the vanilla
// coordinate system.
static boolean V_DecodePatchRow(const patch_t *patch, int wanted,
                                int16_t *row)
{
    const int width = vpatch_width(patch);
    const int height = vpatch_height(patch);
    if ((unsigned)wanted >= (unsigned)height || width <= 0
        || width > WHD_PATCH_MAX_WIDTH)
        return false;

    const uint8_t *palette;
    if (vpatch_has_shared_palette(patch)) {
        const unsigned shared = vpatch_shared_palette(patch);
        if (shared >= NUM_SHARED_PALETTES || !shared_palette8[shared])
            return false;
        palette = shared_palette8[shared];
    } else {
        palette = vpatch_palette(patch);
    }
    const uint8_t *data = vpatch_data(patch);
    const int type = vpatch_type(patch);

    for (int y = 0; y <= wanted; ++y) {
        const boolean emit = y == wanted;
        if (emit)
            for (int x = 0; x < width; ++x) row[x] = -1;

        if (type == vp4_runs || type == vp6_runs || type == vp8_runs) {
            int x = 0;
            for (;;) {
                const uint8_t gap = *data++;
                if (gap == 0xff) break;
                if (gap > width - x) return false;
                x += gap;
                const int length = *data++;
                if (length > width - x) return false;
                if (type == vp4_runs) {
                    for (int i = 0; i < length; i += 2) {
                        const uint8_t packed = *data++;
                        if (emit) {
                            row[x + i] = palette[packed & 15];
                            if (i + 1 < length)
                                row[x + i + 1] = palette[packed >> 4];
                        }
                    }
                } else if (type == vp6_runs) {
                    const int bytes = (length * 6 + 7) >> 3;
                    if (emit) {
                        for (int i = 0; i < length; ++i) {
                            const int bit = i * 6;
                            uint16_t packed = data[bit >> 3];
                            if ((bit & 7) > 2) packed |= data[(bit >> 3) + 1] << 8;
                            row[x + i] = palette[(packed >> (bit & 7)) & 63];
                        }
                    }
                    data += bytes;
                } else {
                    if (emit)
                        for (int i = 0; i < length; ++i)
                            row[x + i] = palette[data[i]];
                    data += length;
                }
                x += length;
                // A run reaching the right edge has no 0xff terminator in
                // WHD. Continuing would consume the next row as run data.
                if (x == width) break;
            }
        } else if (type == vp4_alpha || type == vp4_solid) {
            for (int x = 0; x < width; x += 2) {
                const uint8_t packed = *data++;
                if (emit) {
                    const int low = packed & 15;
                    const int high = packed >> 4;
                    if (type == vp4_solid || low) row[x] = palette[low];
                    if (x + 1 < width && (type == vp4_solid || high))
                        row[x + 1] = palette[high];
                }
            }
        } else if (type == vp_border) {
            if (emit) {
                row[0] = data[0];
                for (int x = 1; x < width - 1; ++x) row[x] = data[1];
                row[width - 1] = data[2];
            }
            data += 3;
        } else {
            return false;
        }
    }
    return true;
}

void V_DrawPatchListScaled(const vpatchlist_t *patchlist, pixel_t *buffer,
                           int stride, int dest_width, int dest_height,
                           int source_width, int source_height)
{
    V_InitSharedPalettes();
    int16_t row[WHD_PATCH_MAX_WIDTH];

    for (int list_index = 1; list_index < patchlist[0].header.size;
         ++list_index) {
        const vpatchlist_t *entry = &patchlist[list_index];
        const patch_t *patch = resolve_vpatch_handle(entry->entry.patch_handle);
        const int patch_width = vpatch_width(patch);
        const int patch_height = vpatch_height(patch);
        const int repeat_width = entry->entry.patch_handle == VPATCH_M_THERMM
                ? patch_width - 1 : patch_width;
        const int total_width = patch_width
                + entry->entry.repeat * repeat_width;
        const int left = entry->entry.x;
        const int top = entry->entry.y;

        int first_y = (top * dest_height + source_height - 1) / source_height;
        int last_y = ((top + patch_height) * dest_height
                      + source_height - 1) / source_height;
        if (first_y < 0) first_y = 0;
        if (last_y > dest_height) last_y = dest_height;

        for (int dest_y = first_y; dest_y < last_y; ++dest_y) {
            const int source_y = dest_y * source_height / dest_height - top;
            if (!V_DecodePatchRow(patch, source_y, row)) continue;

            int first_x = (left * dest_width + source_width - 1) / source_width;
            int last_x = ((left + total_width) * dest_width
                          + source_width - 1) / source_width;
            if (first_x < 0) first_x = 0;
            if (last_x > dest_width) last_x = dest_width;
            pixel_t *dest = buffer + dest_y * stride;

            for (int dest_x = first_x; dest_x < last_x; ++dest_x) {
                int source_x = dest_x * source_width / dest_width - left;
                if ((unsigned)source_x >= (unsigned)total_width) continue;
                if (source_x >= patch_width)
                    source_x = (source_x - patch_width) % repeat_width;
                if (row[source_x] >= 0) dest[dest_x] = row[source_x];
            }
        }
    }
}

void V_DrawPatchListToBuffer(const vpatchlist_t *patchlist, pixel_t *buffer,
                             int stride, int origin_y, int clip_top,
                             int clip_bottom) {
    pixel_t *saved_dest = dest_screen;
    int saved_stride = vpatch_dest_stride;
    int saved_origin = vpatch_dest_origin_y;
    uint8_t saved_top = vpatch_clip_top;
    uint8_t saved_bottom = vpatch_clip_bottom;
    dest_screen = buffer;
    vpatch_dest_stride = stride;
    vpatch_dest_origin_y = origin_y;
    vpatch_clip_top = clip_top;
    vpatch_clip_bottom = clip_bottom;
    V_DrawPatchList(patchlist);
    dest_screen = saved_dest;
    vpatch_dest_stride = saved_stride;
    vpatch_dest_origin_y = saved_origin;
    vpatch_clip_top = saved_top;
    vpatch_clip_bottom = saved_bottom;
}

#pragma GCC pop_options
#endif

void V_DrawPatch(int x, int y, vpatch_handle_large_t patch) {
    V_DrawPatchN(x, y, patch, 0);
}

void V_DrawPatchN(int x, int y, vpatch_handle_large_t patch_handle, int repeat) {
    int count;
    int col;
    column_t *column;
    pixel_t *desttop;
    pixel_t *dest;
    byte *source;
    int w;

#if !USE_WHD
    const patch_t *patch = patch_handle;
#else
    const patch_t *patch = resolve_vpatch_handle(patch_handle);
#endif
    y -= vpatch_topoffset(patch);
    x -= vpatch_leftoffset(patch);

#if !DOOM_ONLY
    // haleyjd 08/28/10: Strife needs silent error checking here.
    if(patchclip_callback)
    {
        if(!patchclip_callback(patch, x, y))
            return;
    }
#endif

#ifdef RANGECHECK
    int canvas_width = vpatchlist ? 320 : SCREENWIDTH;
    int canvas_height = vpatchlist ? 200 : SCREENHEIGHT;
    if (x < 0
        || x + vpatch_width(patch) > canvas_width
        || y < 0
        || y + vpatch_height(patch) > canvas_height) {
        I_Error("Bad V_DrawPatch");
    }
#endif

#if !PICO_DOOM
        V_MarkRect(x, y, vpatch_width(patch), vpatch_height(patch));
#endif

#if !USE_WHD
        desttop = dest_screen + y * SCREENWIDTH + x;
        w = vpatch_width(patch);
        do {
            col = 0;
            for (; col < w; x++, col++, desttop++) {
                column = (column_t *) ((byte *) patch + patch_columnofs(patch, col));

                // step through the posts in a column
                while (column->topdelta != 0xff) {
                    source = (byte *) column + 3;
                    dest = desttop + column->topdelta * SCREENWIDTH;
                    count = column->length;

                    while (count--) {
                        *dest = *source++;
                        dest += SCREENWIDTH;
                    }
                    column = (column_t *) ((byte *) column + column->length + 4);
                }
            }
        } while (repeat--);
#else
    assert(vpatchlist);
    if (vpatchlist[0].header.size <= vpatchlist[0].header.max) {
        vpatchlist[vpatchlist[0].header.size].entry.patch_handle = patch_handle;
        vpatchlist[vpatchlist[0].header.size].entry.x = x;
        vpatchlist[vpatchlist[0].header.size].entry.y = y;
        vpatchlist[vpatchlist[0].header.size].entry.repeat = repeat;
        vpatchlist[0].header.size++;
    }
#endif
}

//
// V_DrawPatchFlipped
// Masks a column based masked pic to the screen.
// Flips horizontally, e.g. to mirror face.
//

void V_DrawPatchFlipped(int x, int y, vpatch_handle_large_t patch_handle) {
#if !USE_WHD
    const patch_t *patch = patch_handle;
#else
    const patch_t *patch = resolve_vpatch_handle(patch_handle);
#endif
#if USE_WHD
    I_Error("V_DrawPatchFlipped is unsupported in the WHD renderer");
#endif
    int count;
    int col;
    column_t *column;
    pixel_t *desttop;
    pixel_t *dest;
    byte *source;
    int w;

    y -= vpatch_topoffset(patch);
    x -= vpatch_leftoffset(patch);

#if !DOOM_ONLY
    // haleyjd 08/28/10: Strife needs silent error checking here.
    if(patchclip_callback)
    {
        if(!patchclip_callback(patch, x, y))
            return;
    }
#endif

#ifdef RANGECHECK
    if (x < 0
        || x + vpatch_width(patch) > SCREENWIDTH
        || y < 0
        || y + vpatch_height(patch) > SCREENHEIGHT) {
        I_Error("Bad V_DrawPatchFlipped");
    }
#endif

    V_MarkRect (x, y, vpatch_width(patch), vpatch_height(patch));

    col = 0;
    desttop = dest_screen + y * SCREENWIDTH + x;

    w = vpatch_width(patch);

    for (; col < w; x++, col++, desttop++) {
        column = (column_t *) ((byte *) patch + patch_columnofs(patch, w - 1 - col));

        // step through the posts in a column
        while (column->topdelta != 0xff) {
            source = (byte *) column + 3;
            dest = desttop + column->topdelta * SCREENWIDTH;
            count = column->length;

            while (count--) {
                *dest = *source++;
                dest += SCREENWIDTH;
            }
            column = (column_t *) ((byte *) column + column->length + 4);
        }
    }
}



//
// V_DrawPatchDirect
// Draws directly to the screen on the pc. 
//

void V_DrawPatchDirect(int x, int y, vpatch_handle_large_t patch) {
    V_DrawPatch(x, y, patch);
}

void V_DrawPatchDirectN(int x, int y, vpatch_handle_large_t patch, int repeat) {
    V_DrawPatchN(x, y, patch, repeat);
}

//
// V_DrawTLPatch
//
// Masks a column based translucent masked pic to the screen.
//
#if !DOOM_ONLY
void V_DrawTLPatch(int x, int y, patch_t * patch)
{
    int count, col;
    column_t *column;
    pixel_t *desttop, *dest;
    byte *source;
    int w;

    y -= patch_topoffset(patch);
    x -= patch_leftoffset(patch);

    if (x < 0
     || x + patch_width(patch) > SCREENWIDTH
     || y < 0
     || y + patch_height(patch) > SCREENHEIGHT)
    {
        I_Error("Bad V_DrawTLPatch");
    }

    col = 0;
    desttop = dest_screen + y * SCREENWIDTH + x;

    w = patch_width(patch);
    for (; col < w; x++, col++, desttop++)
    {
        column = (column_t *) ((byte *) patch + LONG(patch->columnofs[col]));

        // step through the posts in a column

        while (column->topdelta != 0xff)
        {
            source = (byte *) column + 3;
            dest = desttop + column->topdelta * SCREENWIDTH;
            count = column->length;

            while (count--)
            {
                *dest = tinttable[((*dest) << 8) + *source++];
                dest += SCREENWIDTH;
            }
            column = (column_t *) ((byte *) column + column->length + 4);
        }
    }
}

//
// V_DrawXlaPatch
//
// villsa [STRIFE] Masks a column based translucent masked pic to the screen.
//

void V_DrawXlaPatch(int x, int y, patch_t * patch)
{
    int count, col;
    column_t *column;
    pixel_t *desttop, *dest;
    byte *source;
    int w;

    y -= patch_topoffset(patch);
    x -= patch_leftoffset(patch);

    if(patchclip_callback)
    {
        if(!patchclip_callback(patch, x, y))
            return;
    }

    col = 0;
    desttop = dest_screen + y * SCREENWIDTH + x;

    w = patch_width(patch);
    for(; col < w; x++, col++, desttop++)
    {
        column = (column_t *) ((byte *) patch + LONG(patch->columnofs[col]));

        // step through the posts in a column

        while(column->topdelta != 0xff)
        {
            source = (byte *) column + 3;
            dest = desttop + column->topdelta * SCREENWIDTH;
            count = column->length;

            while(count--)
            {
                *dest = xlatab[*dest + ((*source) << 8)];
                source++;
                dest += SCREENWIDTH;
            }
            column = (column_t *) ((byte *) column + column->length + 4);
        }
    }
}

//
// V_DrawAltTLPatch
//
// Masks a column based translucent masked pic to the screen.
//

void V_DrawAltTLPatch(int x, int y, patch_t * patch)
{
    int count, col;
    column_t *column;
    pixel_t *desttop, *dest;
    byte *source;
    int w;

    y -= patch_topoffset(patch);
    x -= patch_leftoffset(patch);

    if (x < 0
     || x + patch_width(patch) > SCREENWIDTH
     || y < 0
     || y + patch_height(patch) > SCREENHEIGHT)
    {
        I_Error("Bad V_DrawAltTLPatch");
    }

    col = 0;
    desttop = dest_screen + y * SCREENWIDTH + x;

    w = patch_width(patch);
    for (; col < w; x++, col++, desttop++)
    {
        column = (column_t *) ((byte *) patch + LONG(patch->columnofs[col]));

        // step through the posts in a column

        while (column->topdelta != 0xff)
        {
            source = (byte *) column + 3;
            dest = desttop + column->topdelta * SCREENWIDTH;
            count = column->length;

            while (count--)
            {
                *dest = tinttable[((*dest) << 8) + *source++];
                dest += SCREENWIDTH;
            }
            column = (column_t *) ((byte *) column + column->length + 4);
        }
    }
}

//
// V_DrawShadowedPatch
//
// Masks a column based masked pic to the screen.
//

void V_DrawShadowedPatch(int x, int y, patch_t *patch)
{
    int count, col;
    column_t *column;
    pixel_t *desttop, *dest;
    byte *source;
    pixel_t *desttop2, *dest2;
    int w;

    y -= patch_topoffset(patch);
    x -= patch_leftoffset(patch);

    if (x < 0
     || x + patch_width(patch) > SCREENWIDTH
     || y < 0
     || y + patch_height(patch) > SCREENHEIGHT)
    {
        I_Error("Bad V_DrawShadowedPatch");
    }

    col = 0;
    desttop = dest_screen + y * SCREENWIDTH + x;
    desttop2 = dest_screen + (y + 2) * SCREENWIDTH + x + 2;

    w = patch_width(patch);
    for (; col < w; x++, col++, desttop++, desttop2++)
    {
        column = (column_t *) ((byte *) patch + LONG(patch->columnofs[col]));

        // step through the posts in a column

        while (column->topdelta != 0xff)
        {
            source = (byte *) column + 3;
            dest = desttop + column->topdelta * SCREENWIDTH;
            dest2 = desttop2 + column->topdelta * SCREENWIDTH;
            count = column->length;

            while (count--)
            {
                *dest2 = tinttable[((*dest2) << 8)];
                dest2 += SCREENWIDTH;
                *dest = *source++;
                dest += SCREENWIDTH;

            }
            column = (column_t *) ((byte *) column + column->length + 4);
        }
    }
}

//
// Load tint table from TINTTAB lump.
//

void V_LoadTintTable(void)
{
    tinttable = W_CacheLumpName("TINTTAB", PU_STATIC);
}

//
// V_LoadXlaTable
//
// villsa [STRIFE] Load xla table from XLATAB lump.
//

void V_LoadXlaTable(void)
{
    xlatab = W_CacheLumpName("XLATAB", PU_STATIC);
}
#endif

//
// V_DrawBlock
// Draw a linear block of pixels into the view buffer.
//

void V_DrawBlock(int x, int y, int width, int height, pixel_t *src) {
    pixel_t *dest;

#ifdef RANGECHECK
    if (x < 0
        || x + width > SCREENWIDTH
        || y < 0
        || y + height > SCREENHEIGHT) {
        I_Error("Bad V_DrawBlock");
    }
#endif

    V_MarkRect (x, y, width, height);

    dest = dest_screen + y * SCREENWIDTH + x;

    while (height--) {
        memcpy(dest, src, width * sizeof(*dest));
        src += width;
        dest += SCREENWIDTH;
    }
}

void V_DrawFilledBox(int x, int y, int w, int h, int c) {
    pixel_t *buf, *buf1;
    int x1, y1;

    buf = I_VideoBuffer + SCREENWIDTH * y + x;

    for (y1 = 0; y1 < h; ++y1) {
        buf1 = buf;

        for (x1 = 0; x1 < w; ++x1) {
            *buf1++ = c;
        }

        buf += SCREENWIDTH;
    }
}

void V_DrawHorizLine(int x, int y, int w, int c) {
    pixel_t *buf;
    int x1;

    buf = I_VideoBuffer + SCREENWIDTH * y + x;

    for (x1 = 0; x1 < w; ++x1) {
        *buf++ = c;
    }
}

void V_DrawVertLine(int x, int y, int h, int c) {
    pixel_t *buf;
    int y1;

    buf = I_VideoBuffer + SCREENWIDTH * y + x;

    for (y1 = 0; y1 < h; ++y1) {
        *buf = c;
        buf += SCREENWIDTH;
    }
}

void V_DrawBox(int x, int y, int w, int h, int c) {
    V_DrawHorizLine(x, y, w, c);
    V_DrawHorizLine(x, y + h - 1, w, c);
    V_DrawVertLine(x, y, h, c);
    V_DrawVertLine(x + w - 1, y, h, c);
}

//
// Draw a "raw" screen (lump containing raw data to blit directly
// to the screen)
//

void V_DrawRawScreen(pixel_t *raw) {
    memcpy(dest_screen, raw, SCREENWIDTH * SCREENHEIGHT * sizeof(*dest_screen));
}

//
// V_Init
// 
void V_Init(void) {
    // no-op!
    // There used to be separate screens that could be drawn to; these are
    // now handled in the upper layers.
}

// Set the buffer that the code draws to.

void V_UseBuffer(pixel_t *buffer) {
    dest_screen = buffer;
}

// Restore screen buffer to the i_video screen buffer.

void V_RestoreBuffer(void) {
    dest_screen = I_VideoBuffer;
}

//
// SCREEN SHOTS
//

typedef PACKED_STRUCT (
        {
            char manufacturer;
            char version;
            char encoding;
            char bits_per_pixel;

            unsigned short xmin;
            unsigned short ymin;
            unsigned short xmax;
            unsigned short ymax;

            unsigned short hres;
            unsigned short vres;

            unsigned char palette[48];

            char reserved;
            char color_planes;
            unsigned short bytes_per_line;
            unsigned short palette_type;

            char filler[58];
            unsigned char data;                // unbounded
        }) pcx_t;


//
// WritePCXfile
//

#if !NO_FILE_ACCESS
void WritePCXfile(char *filename, pixel_t *data,
                  int width, int height,
                  byte *palette)
{
    int		i;
    int		length;
    pcx_t*	pcx;
    byte*	pack;

    pcx = Z_Malloc (width*height*2+1000, PU_STATIC, 0);

    pcx->manufacturer = 0x0a;		// PCX id
    pcx->version = 5;			// 256 color
    pcx->encoding = 1;			// uncompressed
    pcx->bits_per_pixel = 8;		// 256 color
    pcx->xmin = 0;
    pcx->ymin = 0;
    pcx->xmax = SHORT(width-1);
    pcx->ymax = SHORT(height-1);
    pcx->hres = SHORT(1);
    pcx->vres = SHORT(1);
    memset (pcx->palette,0,sizeof(pcx->palette));
    pcx->reserved = 0;                  // PCX spec: reserved byte must be zero
    pcx->color_planes = 1;		// chunky image
    pcx->bytes_per_line = SHORT(width);
    pcx->palette_type = SHORT(2);	// not a grey scale
    memset (pcx->filler,0,sizeof(pcx->filler));

    // pack the image
    pack = &pcx->data;

    for (i=0 ; i<width*height ; i++)
    {
        if ( (*data & 0xc0) != 0xc0)
            *pack++ = *data++;
        else
        {
            *pack++ = 0xc1;
            *pack++ = *data++;
        }
    }

    // write the palette
    *pack++ = 0x0c;	// palette ID byte
    for (i=0 ; i<768 ; i++)
        *pack++ = *palette++;
    
    // write output file
    length = pack - (byte *)pcx;
    M_WriteFile (filename, pcx, length);

    Z_Free (pcx);
}
#endif

#ifdef HAVE_LIBPNG
//
// WritePNGfile
//

static void error_fn(png_structp p, png_const_charp s)
{
    printf("libpng error: %s\n", s);
}

static void warning_fn(png_structp p, png_const_charp s)
{
    printf("libpng warning: %s\n", s);
}

void WritePNGfile(char *filename, pixel_t *data,
                  int width, int height,
                  byte *palette)
{
    png_structp ppng;
    png_infop pinfo;
    png_colorp pcolor;
    FILE *handle;
    int i, j;
    int w_factor, h_factor;
    byte *rowbuf;

    if (aspect_ratio_correct == 1)
    {
        // scale up to accommodate aspect ratio correction
        w_factor = 5;
        h_factor = 6;

        width *= w_factor;
        height *= h_factor;
    }
    else
    {
        w_factor = 1;
        h_factor = 1;
    }

    handle = fopen(filename, "wb");
    if (!handle)
    {
        return;
    }

    ppng = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL,
                                   error_fn, warning_fn);
    if (!ppng)
    {
        fclose(handle);
        return;
    }

    pinfo = png_create_info_struct(ppng);
    if (!pinfo)
    {
        fclose(handle);
        png_destroy_write_struct(&ppng, NULL);
        return;
    }

    png_init_io(ppng, handle);

    png_set_IHDR(ppng, pinfo, width, height,
                 8, PNG_COLOR_TYPE_PALETTE, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    pcolor = malloc(sizeof(*pcolor) * 256);
    if (!pcolor)
    {
        fclose(handle);
        png_destroy_write_struct(&ppng, &pinfo);
        return;
    }

    for (i = 0; i < 256; i++)
    {
        pcolor[i].red   = *(palette + 3 * i);
        pcolor[i].green = *(palette + 3 * i + 1);
        pcolor[i].blue  = *(palette + 3 * i + 2);
    }

    png_set_PLTE(ppng, pinfo, pcolor, 256);
    free(pcolor);

    png_write_info(ppng, pinfo);

    rowbuf = malloc(width);

    if (rowbuf)
    {
        for (i = 0; i < SCREENHEIGHT; i++)
        {
            // expand the row 5x
            for (j = 0; j < SCREENWIDTH; j++)
            {
                memset(rowbuf + j * w_factor, *(data + i*SCREENWIDTH + j), w_factor);
            }

            // write the row 6 times
            for (j = 0; j < h_factor; j++)
            {
                png_write_row(ppng, rowbuf);
            }
        }

        free(rowbuf);
    }

    png_write_end(ppng, pinfo);
    png_destroy_write_struct(&ppng, &pinfo);
    fclose(handle);
}
#endif

//
// V_ScreenShot
//

#if !NO_SCREENSHOT
void V_ScreenShot(const char *format)
{
    int i;
    char lbmname[16]; // haleyjd 20110213: BUG FIX - 12 is too small!
    const char *ext;
    
    // find a file name to save it to

#ifdef HAVE_LIBPNG
    extern int png_screenshots;
    if (png_screenshots)
    {
        ext = "png";
    }
    else
#endif
    {
        ext = "pcx";
    }

    for (i=0; i<=99; i++)
    {
        M_snprintf(lbmname, sizeof(lbmname), format, i, ext);

        if (!M_FileExists(lbmname))
        {
            break;      // file doesn't exist
        }
    }

    if (i == 100)
    {
#ifdef HAVE_LIBPNG
        if (png_screenshots)
        {
            I_Error ("V_ScreenShot: Couldn't create a PNG");
        }
        else
#endif
        {
            I_Error ("V_ScreenShot: Couldn't create a PCX");
        }
    }

#ifdef HAVE_LIBPNG
    if (png_screenshots)
    {
    WritePNGfile(lbmname, I_VideoBuffer,
                 SCREENWIDTH, SCREENHEIGHT,
                 (byte *)W_CacheLumpName (DEH_String("PLAYPAL"), PU_CACHE));
    }
    else
#endif
    {
    // save the pcx file
    WritePCXfile(lbmname, I_VideoBuffer,
                 SCREENWIDTH, SCREENHEIGHT,
                 (byte *)W_CacheLumpName (DEH_String("PLAYPAL"), PU_CACHE));
    }
}
#endif

#if !NO_USE_MOUSE
#define MOUSE_SPEED_BOX_WIDTH  120
#define MOUSE_SPEED_BOX_HEIGHT 9

//
// V_DrawMouseSpeedBox
//

// If box is only to calibrate speed, testing relative speed (as a measure
// of game pixels to movement units) is important whether physical mouse DPI
// is high or low. Line resolution starts at 1 pixel per 1 move-unit: if
// line maxes out, resolution becomes 1 pixel per 2 move-units, then per
// 3 move-units, etc.

static int linelen_multiplier = 1;

void V_DrawMouseSpeedBox(int speed)
{
    extern int usemouse;
    int bgcolor, bordercolor, red, black, white, yellow;
    int box_x, box_y;
    int original_speed;
    int redline_x;
    int linelen;
    int i;
    boolean draw_acceleration = false;

    // Get palette indices for colors for widget. These depend on the
    // palette of the game being played.

    bgcolor = I_GetPaletteIndex(0x77, 0x77, 0x77);
    bordercolor = I_GetPaletteIndex(0x55, 0x55, 0x55);
    red = I_GetPaletteIndex(0xff, 0x00, 0x00);
    black = I_GetPaletteIndex(0x00, 0x00, 0x00);
    yellow = I_GetPaletteIndex(0xff, 0xff, 0x00);
    white = I_GetPaletteIndex(0xff, 0xff, 0xff);

    // If the mouse is turned off, don't draw the box at all.
    if (!usemouse)
    {
        return;
    }

    // If acceleration is used, draw a box that helps to calibrate the
    // threshold point.
    if (mouse_threshold > 0 && fabs(mouse_acceleration - 1) > 0.01)
    {
        draw_acceleration = true;
    }

    // Calculate box position

    box_x = SCREENWIDTH - MOUSE_SPEED_BOX_WIDTH - 10;
    box_y = 15;

    V_DrawFilledBox(box_x, box_y,
                    MOUSE_SPEED_BOX_WIDTH, MOUSE_SPEED_BOX_HEIGHT, bgcolor);
    V_DrawBox(box_x, box_y,
              MOUSE_SPEED_BOX_WIDTH, MOUSE_SPEED_BOX_HEIGHT, bordercolor);

    // Calculate the position of the red threshold line when calibrating
    // acceleration.  This is 1/3 of the way along the box.

    redline_x = MOUSE_SPEED_BOX_WIDTH / 3;

    // Calculate line length

    if (draw_acceleration && speed >= mouse_threshold)
    {
        // Undo acceleration and get back the original mouse speed
        original_speed = speed - mouse_threshold;
        original_speed = (int) (original_speed / mouse_acceleration);
        original_speed += mouse_threshold;

        linelen = (original_speed * redline_x) / mouse_threshold;
    }
    else
    {
        linelen = speed / linelen_multiplier;
    }

    // Draw horizontal "thermometer" 

    if (linelen > MOUSE_SPEED_BOX_WIDTH - 1)
    {
        linelen = MOUSE_SPEED_BOX_WIDTH - 1;
        if (!draw_acceleration)
        {
            linelen_multiplier++;
        }
    }

    V_DrawHorizLine(box_x + 1, box_y + 4, MOUSE_SPEED_BOX_WIDTH - 2, black);

    if (!draw_acceleration || linelen < redline_x)
    {
        V_DrawHorizLine(box_x + 1, box_y + MOUSE_SPEED_BOX_HEIGHT / 2,
                        linelen, white);
    }
    else
    {
        V_DrawHorizLine(box_x + 1, box_y + MOUSE_SPEED_BOX_HEIGHT / 2,
                        redline_x, white);
        V_DrawHorizLine(box_x + redline_x, box_y + MOUSE_SPEED_BOX_HEIGHT / 2,
                        linelen - redline_x, yellow);
    }

    if (draw_acceleration)
    {
        // Draw acceleration threshold line
        V_DrawVertLine(box_x + redline_x, box_y + 1,
                       MOUSE_SPEED_BOX_HEIGHT - 2, red);
    }
    else
    {
        // Draw multiplier lines to indicate current resolution
        for (i = 1; i < linelen_multiplier; i++)
        {
            V_DrawVertLine(
                box_x + (i * MOUSE_SPEED_BOX_WIDTH / linelen_multiplier),
                box_y + 1, MOUSE_SPEED_BOX_HEIGHT - 2, yellow);
        }
    }
}
#endif
