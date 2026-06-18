// Copyright (C) 2003-2024 VLC authors and VideoLAN
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// chapter_command_script.cpp : Matroska Script Codec for Matroska Chapter Codecs

#include "chapter_command_script.hpp"
#include "virtual_segment.hpp"
#include "demux.hpp"

#include <vlc_vout_osd.h>
#include <vlc_input.h>
#include <vlc_subpicture.h>
#include <vlc_text_style.h>
#include <vlc_image.h>

#include <cctype>
#include <sstream>
#include <chrono>

namespace mkv {

const std::string matroska_script_interpretor_c::CMD_MS_GOTO_AND_PLAY = "GotoAndPlay";

// ─────────────────────────────────────────────────────────────────────────────
// OSD helpers
// ─────────────────────────────────────────────────────────────────────────────

// OSD channel for our menu — use a fixed channel distinct from other OSD users
static const int MENU_OSD_CHANNEL = 42;

// ── Subpicture updater for menu text ─────────────────────────────────────────
// Netflix-style horizontal layout:
//   - Options side by side in lower third
//   - Selected option: bright white, all-caps, underline bar beneath
//   - Unselected option: dimmed (50% alpha), all-caps
//   - Font: from sys.s_osd_font_name if set, else renderer default

struct MenuOSDOption {
    char  *label;
    float  x;       // center % of frame width (-1 = auto)
    float  y;       // center % of frame height
    float  width;   // % of frame width
    float  height;  // % of frame height
    int64_t image_uid; // attachment UID for image-style options (0 = none)
};

struct MenuOSDSys {
    MenuOSDOption *options;
    size_t         n_options;
    char          *font_name;
    MenuStyle      menu_style;        // none | libass | image
    IndicatorSpec  selected_ind;
    IndicatorSpec  unselected_ind;
    TimerBarSpec   timer_bar;
    // Pre-decoded indicator image (if type == Image)
    picture_t     *p_indicator_pic;  // decoded from attachment, owned by us
    // Pre-decoded per-option images (for style: image menus)
    picture_t    **pp_option_pics;   // array of n_options pictures, may be NULL entries
    // Timer state — updated each frame
    std::chrono::steady_clock::time_point menu_start;
    int64_t        timeout_s;
    // Currently-highlighted option index. Read by MenuOSDUpdate (vout/
    // prerender thread) every redraw, written by the interpreter's nav
    // event handling (event thread) when the user moves the selection.
    // Guarded by sel_mtx so selection changes never require tearing down
    // and recreating the SPU channel — the live subpicture just reads the
    // updated index on its next natural redraw.
    size_t         current_selected = 0;
    std::mutex     sel_mtx;
    // Back-pointer so MenuOSDDestroy can null out the interpreter's
    // p_live_osd_sys if this subpicture is torn down by some path other
    // than clearMenuOSD()/renderMenuOSD() (e.g. vout flush on seek),
    // preventing a dangling non-owning pointer.
    matroska_script_interpretor_c *owner = nullptr;
};

static void MenuOSDSys_Free( MenuOSDSys *s )
{
    if( !s ) return;
    for( size_t i = 0; i < s->n_options; ++i )
        free( s->options[i].label );
    free( s->options );
    free( s->font_name );
    if( s->p_indicator_pic ) picture_Release( s->p_indicator_pic );
    if( s->pp_option_pics ) {
        for( size_t i = 0; i < s->n_options; ++i )
            if( s->pp_option_pics[i] ) picture_Release( s->pp_option_pics[i] );
        free( s->pp_option_pics );
    }
    delete s;
}

// Convert ASCII string to uppercase in-place
static std::string ToUpper( const std::string & s )
{
    std::string out = s;
    for( char & c : out ) c = (char)std::toupper( (unsigned char)c );
    return out;
}

static void MenuOSDUpdate( subpicture_t *subpic,
                           const struct vlc_spu_updater_configuration *cfg )
{
    MenuOSDSys *sys = static_cast<MenuOSDSys*>( subpic->updater.sys );
    const video_format_t *fmt = cfg->current.video_dst;

    if( !fmt || sys->n_options == 0 ) {
        return;
    }
    if( fmt->i_visible_width == 0 || fmt->i_visible_height == 0 ) {
        return;
    }

    bool regions_empty = vlc_spu_regions_is_empty( &subpic->regions );
    bool similar = video_format_IsSimilar( cfg->previous.video_dst, fmt );
    bool has_timer_active = sys->timer_bar.enabled && sys->timeout_s > 0;
    // Allow redraw every frame when timer is running (fill fraction changes)
    if( !has_timer_active && !regions_empty && similar ) {
        return;
    }

    if( !regions_empty ) {
        vlc_spu_regions_Clear( &subpic->regions );
    }

    const int pic_w = fmt->i_visible_width  * fmt->i_sar_num / fmt->i_sar_den;
    const int pic_h = fmt->i_visible_height;

    subpic->i_original_picture_width  = pic_w;
    subpic->i_original_picture_height = pic_h;

    const int font_size   = pic_h / 14;
    const int slot_w      = pic_w / (int)sys->n_options;
    bool has_positions    = sys->n_options > 0 && sys->options[0].x >= 0.f;

    size_t selected_idx;
    {
        std::unique_lock<std::mutex> sel_lk( sys->sel_mtx );
        selected_idx = sys->current_selected;
    }

    for( size_t i = 0; i < sys->n_options; ++i ) {
        bool sel = (i == selected_idx);

        // ── Text or image region ─────────────────────────────────────────
        // Pixel center position from percentage coords or slot fallback
        int cx = has_positions && sys->options[i].x >= 0.f
                 ? (int)(sys->options[i].x / 100.f * pic_w)
                 : (int)(i) * slot_w + slot_w / 2;
        int cy = has_positions && sys->options[i].y >= 0.f
                 ? (int)(sys->options[i].y / 100.f * pic_h)
                 : (int)(pic_h * 0.88f);

        // For style:image menus with decoded option pictures, render the image.
        // Otherwise render text.
        if( sys->menu_style == MenuStyle::Image &&
            sys->pp_option_pics && sys->pp_option_pics[i] ) {
            // Image-style: render the option icon scaled to option slot
            picture_t *pic = sys->pp_option_pics[i];
            // The source PNGs are @2x assets — halve for display density
            int native_w = (int)pic->format.i_visible_width;
            int native_h = (int)pic->format.i_visible_height;
            int img_w = native_w / 2;
            int img_h = native_h / 2;
            img_w = std::max( 4, img_w );
            img_h = std::max( 1, img_h );

            // Create region at native size then override display dimensions.
            // With b_absolute=true, i_x/i_y are output pixels.
            // fmt.i_visible_width/height control how large the region
            // renders in the output frame.
            video_format_t rfmt;
            video_format_Copy( &rfmt, &pic->format );
            rfmt.i_visible_width  = (unsigned)img_w;
            rfmt.i_visible_height = (unsigned)img_h;
            rfmt.i_width          = rfmt.i_visible_width;
            rfmt.i_height         = rfmt.i_visible_height;
            rfmt.i_x_offset       = 0;
            rfmt.i_y_offset       = 0;
            rfmt.i_sar_num        = 1;
            rfmt.i_sar_den        = 1;
            subpicture_region_t *ir = subpicture_region_New( &rfmt );
            video_format_Clean( &rfmt );
            if( ir && ir->p_picture ) {
                vlc_spu_regions_push( &subpic->regions, ir );
                // Scale source picture into the (smaller) region buffer
                picture_t *dst = ir->p_picture;
                // Simple nearest-neighbour copy if sizes differ
                if( pic->format.i_visible_width  == (unsigned)img_w &&
                    pic->format.i_visible_height == (unsigned)img_h ) {
                    picture_Copy( dst, pic );
                } else {
                    // Fill with scaled pixels — use image handler to convert
                    // For now do a simple row/column subsample
                    const int sw = (int)pic->format.i_visible_width;
                    const int sh = (int)pic->format.i_visible_height;
                    for( int y = 0; y < img_h; ++y ) {
                        int sy = y * sh / img_h;
                        const uint8_t *src_row = pic->p[0].p_pixels
                            + sy * pic->p[0].i_pitch;
                        uint8_t *dst_row = dst->p[0].p_pixels
                            + y * dst->p[0].i_pitch;
                        for( int x = 0; x < img_w; ++x ) {
                            int sx = x * sw / img_w;
                            dst_row[x*4+0] = src_row[sx*4+0];
                            dst_row[x*4+1] = src_row[sx*4+1];
                            dst_row[x*4+2] = src_row[sx*4+2];
                            dst_row[x*4+3] = src_row[sx*4+3];
                        }
                    }
                }
                ir->b_absolute    = true;
                ir->b_in_window   = false;
                ir->i_align       = SUBPICTURE_ALIGN_TOP | SUBPICTURE_ALIGN_LEFT;
                ir->i_x           = cx + fmt->i_x_offset - img_w / 2;
                ir->i_y           = cy + fmt->i_y_offset - img_h / 2;
                ir->i_alpha       = sel ? 0xFF : 0x80;
            } else if( ir ) {
                subpicture_region_Delete( ir );
            }
        } else {
        // ── Text region ──────────────────────────────────────────────────────
        subpicture_region_t *r = subpicture_region_NewText();
        if( !r ) continue;
        vlc_spu_regions_push( &subpic->regions, r );

        r->fmt.i_sar_num = 1;
        r->fmt.i_sar_den = 1;
        r->b_absolute    = false;
        r->b_in_window   = false;

        // With b_absolute=false and no LEFT/RIGHT flag, i_x is offset from
        // frame center. Center text on cx: i_x = cx - pic_w/2.
        r->i_align    = SUBPICTURE_ALIGN_TOP;
        r->i_x        = cx - pic_w / 2;
        r->i_y        = cy - font_size / 2;
        r->text_flags |= SUBPICTURE_ALIGN_TOP;

        text_segment_t *seg = text_segment_New( sys->options[i].label );
        if( seg ) {
            // Start from STYLE_NO_DEFAULTS (a genuinely zero-filled style)
            // rather than STYLE_FULLY_SET. STYLE_FULLY_SET does not mean
            // "every field below is final" — per vlc_text_style.h it tells
            // text_style_Create to populate the struct with the renderer's
            // own sensible defaults first (and sets i_features = 0xFFFF,
            // flagging outline/shadow/background as deliberately present).
            // Setting i_outline_width/i_shadow_width to 0 afterward does not
            // clear those feature flags, so the renderer (libass) was still
            // drawing its default black outline + soft shadow under the
            // text, which is the stroke/glow the user spotted in
            // screenshots — not something this code asked for. Building the
            // style from NO_DEFAULTS and only flagging the features we
            // actually set (font color + alpha) leaves outline/shadow/
            // background genuinely unset, so the renderer draws flat fill
            // only, matching the Netflix reference (clean sans-serif, no
            // stroke, no shadow).
            text_style_t *style = text_style_Create( STYLE_NO_DEFAULTS );
            if( style ) {
                style->i_features    = STYLE_HAS_FONT_COLOR | STYLE_HAS_FONT_ALPHA
                                      | STYLE_HAS_FLAGS;
                style->i_font_size   = font_size;
                style->i_font_color  = sel ? 0xFFFFFF : 0xAAAAAA;
                style->i_font_alpha  = sel ? 0xFF     : 0x80;
                style->i_style_flags = STYLE_BOLD;
                style->i_outline_width = 0;
                style->i_shadow_width  = 0;
                if( sys->font_name && sys->font_name[0] )
                    style->psz_fontname = strdup( sys->font_name );
                seg->style = style;
            }
            r->p_text = seg;
        }
        } // end text/image branch

        // ── Selection indicator ────────────────────────────────────────────────
        if( sel ) {
            const IndicatorSpec & ind = sys->selected_ind;
            // Convert cx/cy to absolute pixels for indicator regions
            int abs_cx = cx + fmt->i_x_offset;
            int abs_cy = cy + fmt->i_y_offset;
            int ind_x = abs_cx;
            // Per Netflix manifest: underline at bottom-center of slot.
            // Slot height = options[i].height% of frame. top:40% within slot.
            //
            // For image-style options, anchor the underline to the bottom
            // edge of *that option's own rendered icon* plus a fixed gap,
            // rather than a flat fraction of the shared slot height. Icons
            // of different aspect ratios (e.g. the wide Netflix wordmark vs
            // the taller, narrower White Bear glyph) render at different
            // heights within the same slot, so a flat slot-relative offset
            // produces an inconsistent visual gap — tight under the tall
            // icon, loose under the short one. Anchoring to each icon's own
            // bottom edge keeps the gap visually equal regardless of the
            // icon's own height.
            int ind_y;
            if( sys->menu_style == MenuStyle::Image &&
                sys->pp_option_pics && sys->pp_option_pics[i] ) {
                picture_t *opt_pic_for_y = sys->pp_option_pics[i];
                int opt_native_h = (int)opt_pic_for_y->format.i_visible_height;
                int opt_img_h    = std::max( 1, opt_native_h / 2 );
                // cy is the option's vertical center; the icon's own bottom
                // edge sits at cy + opt_img_h/2 (icon is centered on cy, see
                // the image-render block above). Add a small fixed gap.
                const int gap_px = std::max( 2, pic_h / 180 );
                ind_y = abs_cy + opt_img_h / 2 + gap_px;
            } else if( has_positions && sys->options[i].height > 0.f ) {
                float slot_h_px = sys->options[i].height / 100.f * pic_h;
                ind_y = abs_cy + (int)(slot_h_px * 0.40f);
            } else {
                ind_y = abs_cy + font_size / 2 + 2;
            }
            if( ind.y_offset != 0.f )
                ind_y = abs_cy + (int)( ind.y_offset / 100.f * pic_h );
            if( ind.x_offset != 0.f )
                ind_x = abs_cx + (int)( ind.x_offset / 100.f * pic_w );

            if( sys->p_indicator_pic ) {
                picture_t *pic = sys->p_indicator_pic;
                // Use the decoded picture's native format for the region
                // Underline width is held constant across all options in
                // this menu (rather than scaled to each icon's own width)
                // per feedback — the y-offset above is what now adapts per
                // icon, not the width.
                int ind_w = (has_positions && sys->options[i].width > 0.f)
                    ? (int)(sys->options[i].width / 100.f * pic_w * 0.30f)
                    : slot_w * 30 / 100;
                ind_w = std::max( 10, ind_w );
                int ind_h = (pic->format.i_visible_width > 0)
                    ? ind_w * (int)pic->format.i_visible_height
                           / (int)pic->format.i_visible_width
                    : std::max(2, pic_h / 150);
                ind_h = std::max( 1, ind_h );

                // Use the decoded picture's native format for the region
                subpicture_region_t *ir = subpicture_region_New( &pic->format );
                if( ir && ir->p_picture ) {
                    vlc_spu_regions_push( &subpic->regions, ir );
                    picture_Copy( ir->p_picture, pic );
                    ir->b_absolute   = true;
                    ir->b_in_window  = false;
                    ir->i_align      = SUBPICTURE_ALIGN_TOP | SUBPICTURE_ALIGN_LEFT;
                    ir->i_x          = ind_x - ind_w / 2;
                    ir->i_y          = ind_y;
                    // Override display size via format fields
                    ir->fmt.i_visible_width  = (unsigned)ind_w;
                    ir->fmt.i_visible_height = (unsigned)ind_h;
                } else if( ir ) {
                    subpicture_region_Delete( ir );
                }
            } else {
                // Fallback: block characters
                static const char BLOCKS[] =
                    "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88"
                    "\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88";
                const int bar_h = std::max( 4, pic_h / 100 );

                subpicture_region_t *bar = subpicture_region_NewText();
                if( bar ) {
                    vlc_spu_regions_push( &subpic->regions, bar );
                    bar->fmt.i_sar_num = 1;
                    bar->fmt.i_sar_den = 1;
                    bar->b_absolute    = true;
                    bar->b_in_window   = false;
                    bar->i_align       = SUBPICTURE_ALIGN_TOP | SUBPICTURE_ALIGN_LEFT;
                    bar->i_x           = ind_x;
                    bar->i_y           = ind_y;
                    bar->text_flags    |= SUBPICTURE_ALIGN_TOP | SUBPICTURE_ALIGN_LEFT;
                    text_segment_t *bseg = text_segment_New( BLOCKS );
                    if( bseg ) {
                        text_style_t *bstyle = text_style_Create( STYLE_NO_DEFAULTS );
                        if( bstyle ) {
                            bstyle->i_font_size   = bar_h;
                            bstyle->i_font_color  = 0xFFFFFF;
                            bstyle->i_font_alpha  = 0xFF;
                            bstyle->i_style_flags = 0;
                            bseg->style = bstyle;
                        }
                        bar->p_text = bseg;
                    }
                }
            }
        }
    }

    // ── Timer bar ────────────────────────────────────────────────────────────
    // Rendered as a raw filled pixel region (not text/glyph based). The
    // previous implementation built the bar out of repeated U+2588 FULL
    // BLOCK characters through the text/libass rendering path. That path
    // applies normal text layout rules (wrapping, line height, kerning)
    // which we have no pixel-level control over; for a long enough run of
    // glyphs the renderer could wrap the string onto 2-3 lines, which is
    // exactly what produced the doubled/tripled-looking timer bar — not
    // multiple subpictures, but one text region whose content wrapped.
    // Filling a picture_t buffer directly sidesteps text layout entirely.
    bool has_timer = sys->timer_bar.enabled && sys->timeout_s > 0;
    if( has_timer && sys->timer_bar.width > 0.f ) {
        using namespace std::chrono;
        float elapsed   = duration<float>(
            steady_clock::now() - sys->menu_start).count();
        float total     = (float)sys->timeout_s;
        float fill_frac = 1.0f - (elapsed / total);
        fill_frac = std::max( sys->timer_bar.min_pct,
                              std::min( 1.0f, fill_frac ) );
        if( sys->timer_bar.steps > 0 ) {
            float step = 1.0f / (float)sys->timer_bar.steps;
            fill_frac  = std::ceil( fill_frac / step ) * step;
            fill_frac  = std::max( sys->timer_bar.min_pct,
                                   std::min( 1.0f, fill_frac ) );
        }

        const TimerBarSpec & tb = sys->timer_bar;
        int bar_full_w = (int)( tb.width  / 100.f * pic_w );
        int bar_h      = std::max( 1, (int)( tb.height / 100.f * pic_h ) );
        int bar_fill_w = (int)( bar_full_w * fill_frac );
        int bar_x      = (int)( tb.x / 100.f * pic_w ) - bar_full_w / 2 + fmt->i_x_offset;
        int bar_y      = (int)( tb.y / 100.f * pic_h ) + fmt->i_y_offset;

        if( bar_fill_w > 0 ) {
            video_format_t bfmt;
            video_format_Init( &bfmt, VLC_CODEC_RGBA );
            bfmt.i_width          = (unsigned)bar_fill_w;
            bfmt.i_height         = (unsigned)bar_h;
            bfmt.i_visible_width  = (unsigned)bar_fill_w;
            bfmt.i_visible_height = (unsigned)bar_h;
            bfmt.i_x_offset       = 0;
            bfmt.i_y_offset       = 0;
            bfmt.i_sar_num        = 1;
            bfmt.i_sar_den        = 1;

            subpicture_region_t *tr = subpicture_region_New( &bfmt );
            video_format_Clean( &bfmt );
            if( tr && tr->p_picture ) {
                vlc_spu_regions_push( &subpic->regions, tr );
                picture_t *dst = tr->p_picture;
                // Fill every pixel opaque white — single solid-color region,
                // no text layout involved.
                for( int y = 0; y < bar_h; ++y ) {
                    uint8_t *row = dst->p[0].p_pixels + y * dst->p[0].i_pitch;
                    for( int x = 0; x < bar_fill_w; ++x ) {
                        row[x*4+0] = 0xFF; // R
                        row[x*4+1] = 0xFF; // G
                        row[x*4+2] = 0xFF; // B
                        row[x*4+3] = 0xFF; // A
                    }
                }
                tr->b_absolute   = true;
                tr->b_in_window  = false;
                tr->i_align      = SUBPICTURE_ALIGN_TOP | SUBPICTURE_ALIGN_LEFT;
                tr->i_x          = bar_x;
                tr->i_y          = bar_y - bar_h / 2;
            } else if( tr ) {
                subpicture_region_Delete( tr );
            }
        }
    }
}

static void MenuOSDDestroy( subpicture_t *subpic )
{
    MenuOSDSys *s = static_cast<MenuOSDSys*>( subpic->updater.sys );
    if( s && s->owner )
        s->owner->onMenuOSDDestroyed( s );
    MenuOSDSys_Free( s );
}

static const struct vlc_spu_updater_ops menu_osd_ops = {
    .update  = MenuOSDUpdate,
    .destroy = MenuOSDDestroy,
};

static subpicture_t * MakeMenuSubpicture( const MenuState & ms,
                                          const std::string & font_name,
                                          demux_sys_t * p_sys,
                                          matroska_script_interpretor_c * owner )
{
    const std::vector<MenuOption> & options = ms.options;
    size_t selected = ms.selected;

    // Allocated with `new` (not calloc) because MenuOSDSys now contains a
    // std::mutex member (sel_mtx) which requires real construction —
    // calloc's zero-fill produces a bit-pattern that LOOKS like an unlocked
    // mutex but isn't one; locking it is undefined behavior and crashed
    // with "mutex lock failed: Invalid argument" the first time
    // MenuOSDUpdate tried to lock sel_mtx on a freshly activated menu.
    // Value-initialization (the parens) zeroes the POD members exactly as
    // calloc did, while still running sel_mtx's constructor.
    MenuOSDSys *sys = new (std::nothrow) MenuOSDSys();
    if( !sys ) return nullptr;

    sys->n_options = options.size();
    sys->options   = static_cast<MenuOSDOption*>(
                         calloc( options.size(), sizeof(MenuOSDOption) ) );
    if( !sys->options ) { delete sys; return nullptr; }

    for( size_t i = 0; i < options.size(); ++i ) {
        sys->options[i].label     = strdup( ToUpper(options[i].label).c_str() );
        sys->options[i].x          = options[i].x;
        sys->options[i].y          = options[i].y;
        sys->options[i].width      = options[i].width;
        sys->options[i].height     = options[i].height;
        sys->options[i].image_uid  = options[i].image_uid;
    }
    sys->current_selected = selected;
    sys->owner             = owner;

    sys->font_name      = font_name.empty() ? nullptr : strdup( font_name.c_str() );
    sys->menu_style     = ms.style;
    sys->selected_ind   = ms.selected_ind;
    sys->unselected_ind = ms.unselected_ind;
    sys->timer_bar      = ms.timer_bar;
    sys->menu_start     = std::chrono::steady_clock::now();
    sys->timeout_s      = ms.timeout_s;
    sys->p_indicator_pic = nullptr;
    sys->pp_option_pics   = nullptr;

    // Helper: decode an attachment PNG/image by UID, optionally scaled to target size
    auto decode_attachment = [&]( int64_t uid, int target_w = 0, int target_h = 0 ) -> picture_t* {
        if( !p_sys || uid == 0 ) return nullptr;
        for( auto & att : p_sys->stored_attachments ) {
            if( !att ) continue;
            std::string fname(att->psz_name ? att->psz_name : "");
            auto it = p_sys->attachment_uid_map.find((uint64_t)uid);
            if( it == p_sys->attachment_uid_map.end() ) continue;
            if( fname != it->second ) continue;
            image_handler_t *ih = image_HandlerCreate( &p_sys->demuxer );
            if( !ih ) return nullptr;
            block_t *b = block_Alloc( att->i_data );
            if( !b ) { image_HandlerDelete(ih); return nullptr; }
            memcpy( b->p_buffer, att->p_data, att->i_data );
            es_format_t fmt_in;
            es_format_Init( &fmt_in, VIDEO_ES, VLC_CODEC_PNG );
            video_format_t fmt_out;
            video_format_Init( &fmt_out, VLC_CODEC_RGBA );
            if( target_w > 0 && target_h > 0 ) {
                fmt_out.i_width          = (unsigned)target_w;
                fmt_out.i_height         = (unsigned)target_h;
                fmt_out.i_visible_width  = (unsigned)target_w;
                fmt_out.i_visible_height = (unsigned)target_h;
            }
            picture_t *pic = image_Read( ih, b, &fmt_in, &fmt_out );
            es_format_Clean( &fmt_in );
            video_format_Clean( &fmt_out );
            image_HandlerDelete( ih );
            return pic;
        }
        return nullptr;
    };

    // Decode indicator image
    if( p_sys && ms.selected_ind.type == IndicatorSpec::Type::Image ) {
        sys->p_indicator_pic = decode_attachment( ms.selected_ind.attach_uid );
        // Fallback: find by filename if UID lookup failed
        if( !sys->p_indicator_pic && !ms.selected_ind.attach_name.empty() ) {
            for( auto & [uid, fname] : p_sys->attachment_uid_map ) {
                if( fname == ms.selected_ind.attach_name ) {
                    sys->p_indicator_pic = decode_attachment( (int64_t)uid );
                    break;
                }
            }
        }
    }

    // Decode per-option images for style: image menus.
    // Decode at native resolution; display size is computed at render time
    // from the native dimensions (assets are @2x, so display at half size).
    if( p_sys && ms.style == MenuStyle::Image ) {
        sys->pp_option_pics = static_cast<picture_t**>(
            calloc( options.size(), sizeof(picture_t*) ) );
        if( sys->pp_option_pics ) {
            for( size_t i = 0; i < options.size(); ++i )
                sys->pp_option_pics[i] = decode_attachment( options[i].image_uid );
        }
    }

    subpicture_updater_t updater = { .sys = sys, .ops = &menu_osd_ops };
    subpicture_t *subpic = subpicture_New( &updater );
    if( !subpic ) { MenuOSDSys_Free(sys); return nullptr; }

    subpic->i_channel  = 0;
    subpic->i_start    = vlc_tick_now();
    subpic->i_stop     = VLC_TICK_INVALID;
    subpic->b_ephemer  = false;
    subpic->b_fade     = false;
    subpic->b_subtitle = false;
    return subpic;
}

void matroska_script_interpretor_c::renderMenuOSD()
{
    demux_sys_t & sys = static_cast<demux_sys_t &>( vm );

    // Log current state for debugging
    std::ostringstream oss;
    for (size_t i = 0; i < menu_state.options.size(); ++i) {
        oss << (i == menu_state.selected ? "> " : "  ")
            << menu_state.options[i].label << "\n";
    }
    vlc_info(l, "MKVScript: MENU: %s", oss.str().c_str());

    if( sys.p_video_es == nullptr )
        return;

    // Remove previous overlay (only happens on activation/dismissal now —
    // selection changes go through updateMenuOSDSelection() instead and
    // never reach this function).
    if( sys.i_menu_overlay_id != SIZE_MAX ) {
        es_out_Control( sys.demuxer.out, ES_OUT_VOUT_DEL_OVERLAY,
                        sys.p_video_es, sys.i_menu_overlay_id );
        sys.i_menu_overlay_id = SIZE_MAX;
        std::unique_lock<std::mutex> osd_lk(osd_sys_mtx);
        p_live_osd_sys = nullptr;
    }

    subpicture_t *subpic = MakeMenuSubpicture( menu_state, sys.s_osd_font_name, &sys, this );
    if( !subpic ) return;

    size_t channel_id = SIZE_MAX;
    if( es_out_Control( sys.demuxer.out, ES_OUT_VOUT_ADD_OVERLAY,
                        sys.p_video_es, subpic, &channel_id ) == VLC_SUCCESS ) {
        sys.i_menu_overlay_id = channel_id;
        std::unique_lock<std::mutex> osd_lk(osd_sys_mtx);
        p_live_osd_sys = subpic->updater.sys;
    } else {
        subpicture_Delete( subpic );
    }
}

void matroska_script_interpretor_c::clearMenuOSD()
{
    demux_sys_t & sys = static_cast<demux_sys_t &>( vm );
    if( sys.p_video_es != nullptr && sys.i_menu_overlay_id != SIZE_MAX ) {
        es_out_Control( sys.demuxer.out, ES_OUT_VOUT_DEL_OVERLAY,
                        sys.p_video_es, sys.i_menu_overlay_id );
        sys.i_menu_overlay_id = SIZE_MAX;
    }
    std::unique_lock<std::mutex> osd_lk(osd_sys_mtx);
    p_live_osd_sys = nullptr;
}

bool matroska_script_interpretor_c::updateMenuOSDSelection()
{
    std::unique_lock<std::mutex> osd_lk(osd_sys_mtx);
    if( !p_live_osd_sys )
        return false;
    MenuOSDSys *osd = static_cast<MenuOSDSys*>( p_live_osd_sys );

    std::unique_lock<std::mutex> lk(menu_state.mtx);
    size_t new_selected = menu_state.selected;
    lk.unlock();

    std::unique_lock<std::mutex> sel_lk(osd->sel_mtx);
    osd->current_selected = new_selected;
    return true;
}

void matroska_script_interpretor_c::onMenuOSDDestroyed( void *osd_sys )
{
    std::unique_lock<std::mutex> osd_lk(osd_sys_mtx);
    if( p_live_osd_sys == osd_sys )
        p_live_osd_sys = nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Nav event handler (called from event thread via HandleKeyEvent)
// ─────────────────────────────────────────────────────────────────────────────

bool matroska_script_interpretor_c::HandleNavEvent( bool up, bool activate )
{
    std::unique_lock<std::mutex> lk(menu_state.mtx);
    if (!menu_state.active)
        return false;

    if (activate) {
        menu_state.confirmed = true;
        return true;
    }

    size_t n = menu_state.options.size();
    if (n == 0) return false;

    if (up) {
        menu_state.selected = (menu_state.selected + n - 1) % n;
    } else {
        menu_state.selected = (menu_state.selected + 1) % n;
    }
    lk.unlock();

    // Try to mutate the already-live subpicture in place first — this is
    // the common case and avoids any ES_OUT_VOUT_DEL_OVERLAY/ADD_OVERLAY
    // round trip (which previously caused the timer bar to render twice
    // briefly on every single selection change, not just on activation).
    if( updateMenuOSDSelection() )
        return true;

    // Fallback: no live subpicture yet (e.g. nav event arrived before the
    // first renderMenuOSD() from Demux()'s poller). Mark dirty so the
    // poller does a full render on its next pass.
    {
        std::unique_lock<std::mutex> lk2(menu_state.mtx);
        menu_state.osd_dirty = true;
    }
    return true;
}

bool matroska_script_interpretor_c::DispatchMenuResult()
{
    size_t chosen;
    {
        std::unique_lock<std::mutex> lk(menu_state.mtx);
        if (!menu_state.active)
            return false;

        bool timed_out = menu_state.has_deadline &&
                         std::chrono::steady_clock::now() >= menu_state.deadline;

        if (!menu_state.confirmed && !timed_out)
            return false;  // still waiting

        chosen = menu_state.selected;
        // NOTE: leave active = true here — JumpTo checks it to suppress
        // premature Enter() calls. We clear it after the jump below.
        menu_state.confirmed = false;
    }

    vlc_info(l, "MKVScript: Menu dispatching option %zu (\"%s\")",
             chosen + 1, menu_state.options[chosen].label.c_str());

    clearMenuOSD();
    bool jumped = dispatchCallable(menu_state.options[chosen]);

    // Now safe to clear active — JumpTo has already run
    {
        std::unique_lock<std::mutex> lk(menu_state.mtx);
        menu_state.active       = false;
        // jump_pending is cleared in JumpTo() and must not be re-set here —
        // JumpTo now handles b_current_vchapter_entered directly.
    }

    return jumped;
}

// ─────────────────────────────────────────────────────────────────────────────
// Callable dispatch
// ─────────────────────────────────────────────────────────────────────────────

bool matroska_script_interpretor_c::dispatchCallable( const MenuOption & opt )
{
    if (opt.is_uid) {
        return doGoto(opt.uid);
    }
    if (!opt.block_label.empty()) {
        vlc_debug(l, "MKVScript: Menu dispatching to block '%s'",
                  opt.block_label.c_str());
        return dispatchBlock(opt.block_label);
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Lexer
// ─────────────────────────────────────────────────────────────────────────────

void matroska_script_interpretor_c::Lexer::skipWS()
{
    while (pos < src.size()) {
        if (std::isspace((unsigned char)src[pos])) {
            ++pos;
        } else if (pos + 1 < src.size() && src[pos] == '/' && src[pos+1] == '/') {
            while (pos < src.size() && src[pos] != '\n')
                ++pos;
        } else {
            break;
        }
    }
}

Token matroska_script_interpretor_c::Lexer::peek()
{
    if (!has_peek) {
        peek_tok = next();
        has_peek = true;
    }
    return peek_tok;
}

Token matroska_script_interpretor_c::Lexer::next()
{
    if (has_peek) {
        has_peek = false;
        return peek_tok;
    }

    skipWS();

    if (pos >= src.size())
        return {TokType::Eof, ""};

    char c = src[pos];

    if (std::isdigit((unsigned char)c)) {
        size_t start = pos;
        while (pos < src.size() && std::isdigit((unsigned char)src[pos]))
            ++pos;
        std::string s = src.substr(start, pos - start);
        int64_t v = 0;
        try { v = std::stoll(s); } catch (...) {}
        return {TokType::Number, s, v};
    }

    if (std::isalpha((unsigned char)c) || c == '_') {
        size_t start = pos;
        while (pos < src.size() &&
               (std::isalnum((unsigned char)src[pos]) || src[pos] == '_'))
            ++pos;
        return {TokType::Ident, src.substr(start, pos - start)};
    }

    if (c == '"') {
        ++pos;
        std::string s;
        while (pos < src.size() && src[pos] != '"') {
            if (src[pos] == '\\' && pos+1 < src.size()) {
                ++pos;
                switch (src[pos]) {
                    case 'n':  s += '\n'; break;
                    case 't':  s += '\t'; break;
                    case '"':  s += '"';  break;
                    case '\\': s += '\\'; break;
                    default:   s += src[pos]; break;
                }
            } else {
                s += src[pos];
            }
            ++pos;
        }
        if (pos < src.size()) ++pos;
        return {TokType::StringLit, s};
    }

    if (pos + 1 < src.size()) {
        std::string two = src.substr(pos, 2);
        if (two == "==") { pos += 2; return {TokType::EqEq,  "=="}; }
        if (two == "!=") { pos += 2; return {TokType::NotEq, "!="}; }
        if (two == "<=") { pos += 2; return {TokType::LtEq,  "<="}; }
        if (two == ">=") { pos += 2; return {TokType::GtEq,  ">="}; }
        if (two == "&&") { pos += 2; return {TokType::And,   "&&"}; }
        if (two == "||") { pos += 2; return {TokType::Or,    "||"}; }
    }

    ++pos;
    switch (c) {
        case '(': return {TokType::LParen,    "("};
        case ')': return {TokType::RParen,    ")"};
        case '{': return {TokType::LBrace,    "{"};
        case '}': return {TokType::RBrace,    "}"};
        case ',': return {TokType::Comma,     ","};
        case ';': return {TokType::Semicolon, ";"};
        case ':': return {TokType::Colon,     ":"};
        case '+': return {TokType::Plus,      "+"};
        case '-': return {TokType::Minus,     "-"};
        case '*': return {TokType::Star,      "*"};
        case '/': return {TokType::Slash,     "/"};
        case '%': return {TokType::Percent,   "%"};
        case '<': return {TokType::Lt,        "<"};
        case '>': return {TokType::Gt,        ">"};
        case '!': return {TokType::Not,       "!"};
        case '=': return {TokType::Assign,    "="};
        default:  return {TokType::Error, std::string(1,c)};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

bool matroska_script_interpretor_c::expect(
    Lexer & lex, TokType t, const char * ctx)
{
    Token tok = lex.next();
    if (tok.type != t) {
        vlc_debug(l, "MKVScript: expected token type %d got '%s' in %s",
                  (int)t, tok.text.c_str(), ctx);
        return false;
    }
    return true;
}

bool matroska_script_interpretor_c::doGoto(chapter_uid uid)
{
    virtual_segment_c * p_vsegment = nullptr;
    virtual_chapter_c * p_vchapter = vm.FindVChapter(uid, p_vsegment);
    if (!p_vchapter) {
        vlc_debug(l, "MKVScript: GotoAndPlay: chapter %" PRIu64 " not found", uid);
        return false;
    }
    vm.JumpTo(*p_vsegment, *p_vchapter);
    return true;
}

std::string matroska_script_interpretor_c::interpolate(const std::string & tmpl)
{
    std::string result;
    size_t i = 0;
    while (i < tmpl.size()) {
        if (tmpl[i] == '{') {
            size_t j = tmpl.find('}', i+1);
            if (j != std::string::npos) {
                std::string varname = tmpl.substr(i+1, j-i-1);
                auto it = vars.find(varname);
                if (it != vars.end()) {
                    result += std::to_string(it->second);
                } else {
                    result += '{';
                    result += varname;
                    result += '}';
                }
                i = j + 1;
                continue;
            }
        }
        result += tmpl[i++];
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Expression evaluator
// ─────────────────────────────────────────────────────────────────────────────

int64_t matroska_script_interpretor_c::evalExpr(Lexer & lex)  { return evalOr(lex); }

int64_t matroska_script_interpretor_c::evalOr(Lexer & lex)
{
    int64_t v = evalAnd(lex);
    while (lex.peek().type == TokType::Or) {
        lex.next();
        int64_t r = evalAnd(lex);
        v = (v || r) ? 1 : 0;
    }
    return v;
}

int64_t matroska_script_interpretor_c::evalAnd(Lexer & lex)
{
    int64_t v = evalNot(lex);
    while (lex.peek().type == TokType::And) {
        lex.next();
        int64_t r = evalNot(lex);
        v = (v && r) ? 1 : 0;
    }
    return v;
}

int64_t matroska_script_interpretor_c::evalNot(Lexer & lex)
{
    if (lex.peek().type == TokType::Not) {
        lex.next();
        return evalNot(lex) ? 0 : 1;
    }
    return evalCmp(lex);
}

int64_t matroska_script_interpretor_c::evalCmp(Lexer & lex)
{
    int64_t v = evalAdd(lex);
    TokType t = lex.peek().type;
    if (t == TokType::EqEq || t == TokType::NotEq ||
        t == TokType::Lt   || t == TokType::Gt    ||
        t == TokType::LtEq || t == TokType::GtEq)
    {
        lex.next();
        int64_t r = evalAdd(lex);
        switch (t) {
            case TokType::EqEq:  return (v == r) ? 1 : 0;
            case TokType::NotEq: return (v != r) ? 1 : 0;
            case TokType::Lt:    return (v <  r) ? 1 : 0;
            case TokType::Gt:    return (v >  r) ? 1 : 0;
            case TokType::LtEq:  return (v <= r) ? 1 : 0;
            case TokType::GtEq:  return (v >= r) ? 1 : 0;
            default: break;
        }
    }
    return v;
}

int64_t matroska_script_interpretor_c::evalAdd(Lexer & lex)
{
    int64_t v = evalMul(lex);
    for (;;) {
        TokType t = lex.peek().type;
        if      (t == TokType::Plus)  { lex.next(); v += evalMul(lex); }
        else if (t == TokType::Minus) { lex.next(); v -= evalMul(lex); }
        else break;
    }
    return v;
}

int64_t matroska_script_interpretor_c::evalMul(Lexer & lex)
{
    int64_t v = evalUnary(lex);
    for (;;) {
        TokType t = lex.peek().type;
        if (t == TokType::Star) {
            lex.next(); v *= evalUnary(lex);
        } else if (t == TokType::Slash) {
            lex.next(); int64_t r = evalUnary(lex); v = r ? (v/r) : 0;
        } else break;
        // NOTE: TokType::Percent is intentionally NOT handled here.
        // In iMKV scripts, '%' is always a postfix annotation on a numeric
        // literal (e.g. "50%"), never a binary modulo operator.
        // Callers that need float percentages use direct token reading.
    }
    return v;
}

int64_t matroska_script_interpretor_c::evalUnary(Lexer & lex)
{
    if (lex.peek().type == TokType::Minus) {
        lex.next();
        return -evalUnary(lex);
    }
    return evalPrim(lex);
}

int64_t matroska_script_interpretor_c::evalPrim(Lexer & lex)
{
    Token t = lex.peek();
    if (t.type == TokType::Number) { lex.next(); return t.ival; }
    if (t.type == TokType::Ident) {
        lex.next();
        auto it = vars.find(t.text);
        return (it != vars.end()) ? it->second : 0;
    }
    if (t.type == TokType::LParen) {
        lex.next();
        int64_t v = evalExpr(lex);
        expect(lex, TokType::RParen, "expression");
        return v;
    }
    vlc_debug(l, "MKVScript: unexpected token '%s' in expression", t.text.c_str());
    lex.next();
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Statement executors
// ─────────────────────────────────────────────────────────────────────────────

bool matroska_script_interpretor_c::execBlock(Lexer & lex)
{
    // Skip optional label: ident ':'
    while (true) {
        Token t = lex.peek();
        if (t.type == TokType::Ident) {
            Token ident = lex.next();
            Token colon = lex.peek();
            if (colon.type == TokType::Colon) {
                lex.next();
                if (!expect(lex, TokType::LBrace, "block after label"))
                    return false;
                break;
            } else {
                return execStmtWithIdent(lex, ident);
            }
        } else if (t.type == TokType::LBrace) {
            lex.next();
            break;
        } else {
            break;
        }
    }

    bool result = false;
    while (true) {
        Token t = lex.peek();
        if (t.type == TokType::RBrace || t.type == TokType::Eof)
            break;
        result |= execStmt(lex);
        if (result) {
            int depth = 1;
            while (depth > 0 && lex.peek().type != TokType::Eof) {
                Token drain = lex.next();
                if (drain.type == TokType::LBrace) ++depth;
                else if (drain.type == TokType::RBrace) --depth;
            }
            return true;
        }
    }
    if (lex.peek().type == TokType::RBrace)
        lex.next();
    return result;
}

bool matroska_script_interpretor_c::execStmtWithIdent(Lexer & lex, Token ident)
{
    vlc_debug(l, "MKVScript: unexpected ident '%s' as statement", ident.text.c_str());
    while (lex.peek().type != TokType::Semicolon &&
           lex.peek().type != TokType::Eof)
        lex.next();
    if (lex.peek().type == TokType::Semicolon) lex.next();
    return false;
}

bool matroska_script_interpretor_c::execStmt(Lexer & lex)
{
    Token t = lex.peek();
    if (t.type == TokType::Ident) {
        const std::string & kw = t.text;
        if (kw == "Let")         return execLet(lex);
        if (kw == "GotoAndPlay") return execGoto(lex);
        if (kw == "Select")      return execSelect(lex);
        if (kw == "Menu")        return execMenu(lex);
        if (kw == "Panic")       return execPanic(lex);
        if (kw == "Log")         return execLog(lex);
        if (kw == "SetFont")        return execSetFont(lex);
        if (kw == "SetTimerBar")    return execSetTimerBar(lex);
        if (kw == "if")          return execIf(lex);
        return execBlock(lex);
    }
    vlc_debug(l, "MKVScript: unexpected token '%s'", t.text.c_str());
    lex.next();
    while (lex.peek().type != TokType::Semicolon &&
           lex.peek().type != TokType::Eof)
        lex.next();
    if (lex.peek().type == TokType::Semicolon) lex.next();
    return false;
}

bool matroska_script_interpretor_c::execLet(Lexer & lex)
{
    lex.next();
    if (!expect(lex, TokType::LParen, "Let")) return false;
    Token name = lex.next();
    if (name.type != TokType::Ident) {
        vlc_debug(l, "MKVScript: Let: expected variable name");
        return false;
    }
    if (!expect(lex, TokType::Comma, "Let")) return false;
    int64_t val = evalExpr(lex);
    vars[name.text] = val;
    vlc_info(l, "MKVScript: Let(%s, %" PRId64 ")", name.text.c_str(), val);
    if (!expect(lex, TokType::RParen, "Let")) return false;
    if (lex.peek().type == TokType::Semicolon) lex.next();
    return false;
}

bool matroska_script_interpretor_c::execGoto(Lexer & lex)
{
    lex.next();
    if (!expect(lex, TokType::LParen, "GotoAndPlay")) return false;
    int64_t uid = evalExpr(lex);
    if (!expect(lex, TokType::RParen, "GotoAndPlay")) return false;
    if (lex.peek().type == TokType::Semicolon) lex.next();
    vlc_debug(l, "MKVScript: GotoAndPlay(%" PRId64 ")", uid);
    return doGoto((chapter_uid)uid);
}

bool matroska_script_interpretor_c::execIf(Lexer & lex)
{
    lex.next();
    if (!expect(lex, TokType::LParen, "if")) return false;
    int64_t cond = evalExpr(lex);
    if (!expect(lex, TokType::RParen, "if")) return false;
    if (!expect(lex, TokType::LBrace, "if-then")) return false;

    bool result = false;
    if (cond) {
        while (lex.peek().type != TokType::RBrace &&
               lex.peek().type != TokType::Eof)
            result |= execStmt(lex);
        if (lex.peek().type == TokType::RBrace) lex.next();
        if (lex.peek().type == TokType::Ident && lex.peek().text == "else") {
            lex.next();
            if (lex.peek().type == TokType::LBrace) {
                lex.next();
                int depth = 1;
                while (depth > 0 && lex.peek().type != TokType::Eof) {
                    Token d = lex.next();
                    if (d.type == TokType::LBrace) ++depth;
                    else if (d.type == TokType::RBrace) --depth;
                }
            }
        }
    } else {
        int depth = 1;
        while (depth > 0 && lex.peek().type != TokType::Eof) {
            Token d = lex.next();
            if (d.type == TokType::LBrace) ++depth;
            else if (d.type == TokType::RBrace) --depth;
        }
        if (lex.peek().type == TokType::Ident && lex.peek().text == "else") {
            lex.next();
            if (!expect(lex, TokType::LBrace, "if-else")) return false;
            while (lex.peek().type != TokType::RBrace &&
                   lex.peek().type != TokType::Eof)
                result |= execStmt(lex);
            if (lex.peek().type == TokType::RBrace) lex.next();
        }
    }
    return result;
}

bool matroska_script_interpretor_c::execSelect(Lexer & lex)
{
    lex.next();
    if (!expect(lex, TokType::LBrace, "Select")) return false;

    bool fired = false;
    bool done  = false;

    while (!done) {
        Token t = lex.peek();
        if (t.type == TokType::RBrace || t.type == TokType::Eof) break;
        if (t.type != TokType::Ident) { lex.next(); continue; }

        if (t.text == "Case") {
            lex.next();
            if (!expect(lex, TokType::LParen, "Case")) return false;
            int64_t cond = evalExpr(lex);
            if (!expect(lex, TokType::RParen, "Case")) return false;
            if (!expect(lex, TokType::LBrace, "Case body")) return false;

            if (!fired && cond) {
                fired = true;
                while (lex.peek().type != TokType::RBrace &&
                       lex.peek().type != TokType::Eof) {
                    bool jumped = execStmt(lex);
                    if (jumped) {
                        int depth = 1;
                        while (depth > 0 && lex.peek().type != TokType::Eof) {
                            Token d = lex.next();
                            if (d.type == TokType::LBrace) ++depth;
                            else if (d.type == TokType::RBrace) --depth;
                        }
                        int depth2 = 1;
                        while (depth2 > 0 && lex.peek().type != TokType::Eof) {
                            Token d = lex.next();
                            if (d.type == TokType::LBrace) ++depth2;
                            else if (d.type == TokType::RBrace) --depth2;
                        }
                        return true;
                    }
                }
                if (lex.peek().type == TokType::RBrace) lex.next();
                done = true;
            } else {
                int depth = 1;
                while (depth > 0 && lex.peek().type != TokType::Eof) {
                    Token d = lex.next();
                    if (d.type == TokType::LBrace) ++depth;
                    else if (d.type == TokType::RBrace) --depth;
                }
            }
        } else if (t.text == "Default") {
            lex.next();
            if (!expect(lex, TokType::LBrace, "Default body")) return false;
            if (!fired) {
                while (lex.peek().type != TokType::RBrace &&
                       lex.peek().type != TokType::Eof) {
                    bool jumped = execStmt(lex);
                    if (jumped) {
                        int depth = 1;
                        while (depth > 0 && lex.peek().type != TokType::Eof) {
                            Token d = lex.next();
                            if (d.type == TokType::LBrace) ++depth;
                            else if (d.type == TokType::RBrace) --depth;
                        }
                        int depth2 = 1;
                        while (depth2 > 0 && lex.peek().type != TokType::Eof) {
                            Token d = lex.next();
                            if (d.type == TokType::LBrace) ++depth2;
                            else if (d.type == TokType::RBrace) --depth2;
                        }
                        return true;
                    }
                }
                if (lex.peek().type == TokType::RBrace) lex.next();
            } else {
                int depth = 1;
                while (depth > 0 && lex.peek().type != TokType::Eof) {
                    Token d = lex.next();
                    if (d.type == TokType::LBrace) ++depth;
                    else if (d.type == TokType::RBrace) --depth;
                }
            }
            done = true;
        } else {
            lex.next();
        }
    }
    if (lex.peek().type == TokType::RBrace) lex.next();
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Menu — interactive choice with OSD and condition variable
// ─────────────────────────────────────────────────────────────────────────────

bool matroska_script_interpretor_c::execMenu(Lexer & lex)
{
    lex.next(); // consume 'Menu'
    if (!expect(lex, TokType::LParen, "Menu")) return false;

    int64_t timeout_s   = 10;
    int64_t default_idx = 1;
    std::vector<MenuOption> options;
    MenuStyle    style = MenuStyle::None;
    IndicatorSpec sel_ind, unsel_ind;
    TimerBarSpec  tb;
    bool has_timer_override = false;

    while (lex.peek().type != TokType::RParen &&
           lex.peek().type != TokType::Eof)
    {
        Token t = lex.peek();

        if (t.type == TokType::Ident && t.text == "timeout") {
            lex.next();
            if (lex.peek().type == TokType::Colon) { lex.next(); timeout_s = evalExpr(lex); }
            if (lex.peek().type == TokType::Comma) lex.next();
        }
        else if (t.type == TokType::Ident && t.text == "default") {
            lex.next();
            if (lex.peek().type == TokType::Colon) { lex.next(); default_idx = evalExpr(lex); }
            if (lex.peek().type == TokType::Comma) lex.next();
        }
        else if (t.type == TokType::Ident && t.text == "style") {
            lex.next();
            if (lex.peek().type == TokType::Colon) {
                lex.next();
                Token sv = lex.next();
                if (sv.type == TokType::Ident) {
                    if      (sv.text == "libass") style = MenuStyle::LibASS;
                    else if (sv.text == "image")  style = MenuStyle::Image;
                    else                          style = MenuStyle::None;
                }
            }
            if (lex.peek().type == TokType::Comma) lex.next();
        }
        else if (t.type == TokType::Ident && t.text == "selected") {
            lex.next();
            if (lex.peek().type == TokType::Colon) {
                lex.next();
                parseIndicator(lex, sel_ind);
            }
            if (lex.peek().type == TokType::Comma) lex.next();
        }
        else if (t.type == TokType::Ident && t.text == "unselected") {
            lex.next();
            if (lex.peek().type == TokType::Colon) {
                lex.next();
                parseIndicator(lex, unsel_ind);
            }
            if (lex.peek().type == TokType::Comma) lex.next();
        }
        else if (t.type == TokType::Ident && t.text == "timer_bar") {
            lex.next();
            if (lex.peek().type == TokType::Colon) {
                lex.next();
                if (parseTimerBar(lex, tb)) {
                    has_timer_override = true;
                }
            }
            if (lex.peek().type == TokType::Comma) lex.next();
        }
        else if (t.type == TokType::Ident && t.text == "Option") {
            lex.next();
            if (!expect(lex, TokType::LParen, "Option")) return false;
            MenuOption opt;
            if (!execOption(lex, opt)) return false;
            if (!expect(lex, TokType::RParen, "Option")) return false;
            options.push_back(opt);
            if (lex.peek().type == TokType::Comma) lex.next();
        }
        else if (t.type == TokType::Comma) {
            lex.next();
        }
        else {
            lex.next();
        }
    }

    if (lex.peek().type == TokType::RParen) lex.next();
    if (lex.peek().type == TokType::Semicolon) lex.next();

    if (options.empty()) {
        vlc_debug(l, "MKVScript: Menu: no options");
        return false;
    }

    size_t def = (size_t)(default_idx - 1);
    if (def >= options.size()) def = 0;

    vlc_info(l, "MKVScript: Menu with %zu options (timeout=%" PRId64 "s default=%zu)",
             options.size(), timeout_s, def + 1);
    for (size_t i = 0; i < options.size(); ++i)
        vlc_info(l, "MKVScript:   [%zu] %s", i+1, options[i].label.c_str());

    {
        std::unique_lock<std::mutex> lk(menu_state.mtx);
        menu_state.options      = options;
        menu_state.selected     = def;
        menu_state.default_idx  = def;
        menu_state.timeout_s    = timeout_s;
        menu_state.confirmed    = false;
        menu_state.active       = true;
        menu_state.osd_dirty    = true;
        menu_state.style        = style;
        menu_state.selected_ind = sel_ind;
        menu_state.unselected_ind = unsel_ind;
        // Timer: per-menu override takes precedence; else inherit default
        if (has_timer_override)
            menu_state.timer_bar = tb;
        else
            menu_state.timer_bar = default_timer_bar;

        if (timeout_s > 0) {
            menu_state.deadline     = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(timeout_s);
            menu_state.has_deadline = true;
        } else {
            menu_state.has_deadline = false;
        }
    }

    // NOTE: Do not call renderMenuOSD() here. menu_state.osd_dirty is
    // already true, and Demux()'s polling loop is the single authoritative
    // caller of renderMenuOSD() — it renders once and clears osd_dirty
    // immediately after. Calling it here too produced a double (sometimes
    // triple, if a nav event landed in the same window) render: two or
    // three live subpictures stacked, each drawing its own timer bar and
    // option regions.
    vlc_info(l, "MKVScript: Menu activated (non-blocking) — Demux() will poll");
    return false;
}

bool matroska_script_interpretor_c::execPanic(Lexer & lex)
{
    lex.next();
    if (!expect(lex, TokType::LParen, "Panic")) return false;
    Token msg = lex.next();
    std::string text = (msg.type == TokType::StringLit) ? msg.text : msg.text;
    if (!expect(lex, TokType::RParen, "Panic")) return false;
    if (lex.peek().type == TokType::Semicolon) lex.next();
    vlc_info(l, "MKVScript: PANIC: %s", text.c_str());
    return false;
}

bool matroska_script_interpretor_c::execLog(Lexer & lex)
{
    lex.next();
    if (!expect(lex, TokType::LParen, "Log")) return false;
    Token msg = lex.next();
    std::string text = (msg.type == TokType::StringLit) ?
                       interpolate(msg.text) : msg.text;
    if (!expect(lex, TokType::RParen, "Log")) return false;
    if (lex.peek().type == TokType::Semicolon) lex.next();
    vlc_info(l, "MKVScript: LOG: %s", text.c_str());
    return false;
}

bool matroska_script_interpretor_c::execSetFont(Lexer & lex)
{
    // SetFont(attach(N))  — set OSD font by attachment UID
    // SetFont("name")     — set OSD font by name (informational; renderer uses if available)
    // May appear at most once per file; if called again, last call wins.
    lex.next(); // consume 'SetFont'
    if (!expect(lex, TokType::LParen, "SetFont")) return false;

    demux_sys_t & sys = static_cast<demux_sys_t &>(vm);
    Token t = lex.peek();

    if (t.type == TokType::Ident && t.text == "attach") {
        // SetFont(attach(N))
        lex.next();
        if (!expect(lex, TokType::LParen, "attach")) return false;
        int64_t uid = evalExpr(lex);
        if (!expect(lex, TokType::RParen, "attach")) return false;

        auto it = sys.attachment_uid_map.find((uint64_t)uid);
        if (it != sys.attachment_uid_map.end()) {
            sys.s_osd_font_name = it->second;
            // Try to extract the font family name from the attachment data
            // so psz_fontname matches what freetype registered.
            // The OTF/TTF name table stores family name at nameID=1.
            // We look for the stored attachment and scan its name table.
            for (auto & att : sys.stored_attachments) {
                if (!att) continue;
                std::string fname(att->psz_name ? att->psz_name : "");
                if (fname == it->second) {
                    const uint8_t *d = static_cast<const uint8_t*>(att->p_data);
                    size_t sz = att->i_data;
                    // Minimal OTF/TTF name table parser:
                    // Offset 4: numTables (uint16_t big-endian)
                    // Table records at offset 12, each 16 bytes: tag(4)+checksum(4)+offset(4)+length(4)
                    // Find 'name' table, then parse records for nameID=1 platformID=3
                    if (sz < 12) break;
                    uint16_t numTables = (d[4] << 8) | d[5];
                    for (uint16_t ti = 0; ti < numTables && 12 + ti*16 + 16 <= sz; ++ti) {
                        const uint8_t *rec = d + 12 + ti * 16;
                        if (rec[0]=='n' && rec[1]=='a' && rec[2]=='m' && rec[3]=='e') {
                            uint32_t toff = (rec[8]<<24)|(rec[9]<<16)|(rec[10]<<8)|rec[11];
                            if (toff + 6 > sz) break;
                            const uint8_t *nt = d + toff;
                            uint16_t count  = (nt[2]<<8)|nt[3];
                            uint16_t stroff = (nt[4]<<8)|nt[5];
                            for (uint16_t ni = 0; ni < count && toff+6+ni*12+12<=sz; ++ni) {
                                const uint8_t *nr = nt + 6 + ni * 12;
                                uint16_t platID  = (nr[0]<<8)|nr[1];
                                uint16_t nameID  = (nr[6]<<8)|nr[7];
                                uint16_t slen    = (nr[8]<<8)|nr[9];
                                uint16_t soff    = (nr[10]<<8)|nr[11];
                                if (nameID == 1 && platID == 3) {
                                    // UTF-16BE — convert to UTF-8 (ASCII range only)
                                    const uint8_t *sp = nt + stroff + soff;
                                    if (toff + stroff + soff + slen > sz) break;
                                    std::string family;
                                    for (uint16_t ci = 0; ci+1 < slen; ci += 2) {
                                        uint16_t ch = (sp[ci]<<8)|sp[ci+1];
                                        if (ch < 0x80) family += (char)ch;
                                    }
                                    if (!family.empty()) {
                                        sys.s_osd_font_name = family;
                                        vlc_info(l, "MKVScript: SetFont: family='%s'",
                                                 family.c_str());
                                    }
                                    break;
                                }
                            }
                            break;
                        }
                    }
                    break;
                }
            }
            vlc_info(l, "MKVScript: SetFont(attach(%" PRId64 ")) -> \"%s\"",
                     uid, sys.s_osd_font_name.c_str());
        } else {
            vlc_info(l, "MKVScript: SetFont(attach(%" PRId64 ")) -> attachment not found, using renderer default",
                     uid);
        }
    } else if (t.type == TokType::StringLit) {
        // SetFont("FontName")
        lex.next();
        sys.s_osd_font_name = t.text;
        vlc_info(l, "MKVScript: SetFont(\"%s\")", sys.s_osd_font_name.c_str());
    } else {
        vlc_info(l, "MKVScript: SetFont: expected attach(N) or string literal");
        while (lex.peek().type != TokType::RParen &&
               lex.peek().type != TokType::Eof)
            lex.next();
    }

    if (!expect(lex, TokType::RParen, "SetFont")) return false;
    if (lex.peek().type == TokType::Semicolon) lex.next();
    return false;
}

bool matroska_script_interpretor_c::execOption(
    Lexer & lex, MenuOption & opt)
{
    // Option body: already consumed '(', will consume up to but not including ')'
    // Syntax: text: "...", [image: attach(N),] [x: %, y: %, width: %, height: %,]
    //         GotoAndPlay(uid)
    // OR legacy: "label", GotoAndPlay(uid)

    Token first = lex.peek();

    // Legacy positional: first arg is a string literal
    if (first.type == TokType::StringLit) {
        lex.next();
        opt.label = first.text;
        if (lex.peek().type == TokType::Comma) lex.next();
        // Next should be GotoAndPlay or block label
        Token callable = lex.peek();
        if (callable.type == TokType::Ident && callable.text == "GotoAndPlay") {
            lex.next();
            if (!expect(lex, TokType::LParen, "GotoAndPlay")) return false;
            opt.uid    = (chapter_uid)evalExpr(lex);
            opt.is_uid = true;
            if (!expect(lex, TokType::RParen, "GotoAndPlay")) return false;
        } else if (callable.type == TokType::Ident) {
            lex.next();
            opt.block_label = callable.text;
        }
        return true;
    }

    // Named parameters
    while (lex.peek().type != TokType::RParen &&
           lex.peek().type != TokType::Eof)
    {
        Token t = lex.peek();
        if (t.type != TokType::Ident) { lex.next(); continue; }

        if (t.text == "text") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            Token v = lex.next();
            if (v.type == TokType::StringLit) opt.label = v.text;
        }
        else if (t.text == "image") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            // expect: attach(N) or attach("filename")
            Token attach = lex.next(); // 'attach'
            if (attach.text == "attach") {
                if (lex.peek().type == TokType::LParen) {
                    lex.next();
                    Token av = lex.next();
                    if (av.type == TokType::StringLit) {
                        opt.image_attach = av.text;
                    } else if (av.type == TokType::Number) {
                        // UID integer — look up filename later
                        opt.image_uid = av.ival;
                    }
                    if (lex.peek().type == TokType::RParen) lex.next();
                }
            }
        }
        else if (t.text == "x") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            // Parse float percentage: e.g. 28.67%
            Token iv = lex.next();
            opt.x = (float)iv.ival;
            if (lex.peek().type == TokType::Error && !lex.peek().text.empty() && lex.peek().text[0] == '.') {
                std::string frac = lex.next().text;
                if (lex.peek().type == TokType::Number) {
                    std::string fs = std::to_string(iv.ival) + frac + lex.next().text;
                    try { opt.x = std::stof(fs); } catch(...) {}
                }
            }
            if (lex.peek().type == TokType::Percent) lex.next();
        }
        else if (t.text == "y") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            Token iv = lex.next();
            opt.y = (float)iv.ival;
            if (lex.peek().type == TokType::Error && !lex.peek().text.empty() && lex.peek().text[0] == '.') {
                std::string frac = lex.next().text;
                if (lex.peek().type == TokType::Number) {
                    std::string fs = std::to_string(iv.ival) + frac + lex.next().text;
                    try { opt.y = std::stof(fs); } catch(...) {}
                }
            }
            if (lex.peek().type == TokType::Percent) lex.next();
        }
        else if (t.text == "width") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            Token iv = lex.next();
            opt.width = (float)iv.ival;
            if (lex.peek().type == TokType::Error && !lex.peek().text.empty() && lex.peek().text[0] == '.') {
                std::string frac = lex.next().text;
                if (lex.peek().type == TokType::Number) {
                    std::string fs = std::to_string(iv.ival) + frac + lex.next().text;
                    try { opt.width = std::stof(fs); } catch(...) {}
                }
            }
            if (lex.peek().type == TokType::Percent) lex.next();
        }
        else if (t.text == "height") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            Token iv = lex.next();
            opt.height = (float)iv.ival;
            if (lex.peek().type == TokType::Error && !lex.peek().text.empty() && lex.peek().text[0] == '.') {
                std::string frac = lex.next().text;
                if (lex.peek().type == TokType::Number) {
                    std::string fs = std::to_string(iv.ival) + frac + lex.next().text;
                    try { opt.height = std::stof(fs); } catch(...) {}
                }
            }
            if (lex.peek().type == TokType::Percent) lex.next();
        }
        else if (t.text == "GotoAndPlay") {
            lex.next();
            if (lex.peek().type == TokType::LParen) {
                lex.next();
                opt.uid    = (chapter_uid)evalExpr(lex);
                opt.is_uid = true;
                if (lex.peek().type == TokType::RParen) lex.next();
            }
        }
        else {
            lex.next(); // unknown keyword, skip
        }

        if (lex.peek().type == TokType::Comma) lex.next();
    }
    return true;
}

bool matroska_script_interpretor_c::parseIndicator(
    Lexer & lex, IndicatorSpec & ind)
{
    // Parses: none  |  { type: image|libass, attach: "...", markup: "...",
    //                    x_offset: %, y_offset: % }
    Token t = lex.peek();
    if (t.type == TokType::Ident && t.text == "none") {
        lex.next();
        ind.type = IndicatorSpec::Type::None;
        return true;
    }
    if (t.type != TokType::LBrace) return false;
    lex.next(); // consume '{'

    while (lex.peek().type != TokType::RBrace &&
           lex.peek().type != TokType::Eof)
    {
        Token k = lex.peek();
        if (k.type != TokType::Ident) { lex.next(); continue; }

        if (k.text == "libass") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            Token v = lex.next();
            if (v.type == TokType::StringLit) {
                ind.markup = v.text;
                ind.type   = IndicatorSpec::Type::LibASS;
            }
        }
        else if (k.text == "image") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            Token attach = lex.next();
            if (attach.text == "attach") {
                if (lex.peek().type == TokType::LParen) {
                    lex.next();
                    Token av = lex.next();
                    if (av.type == TokType::StringLit)
                        ind.attach_name = av.text;
                    else if (av.type == TokType::Number)
                        ind.attach_uid = av.ival;
                    if (lex.peek().type == TokType::RParen) lex.next();
                }
                ind.type = IndicatorSpec::Type::Image;
            }
        }
        else if (k.text == "x_offset") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            Token iv = lex.next();
            ind.x_offset = (float)iv.ival;
            if (lex.peek().type == TokType::Error &&
                !lex.peek().text.empty() && lex.peek().text[0] == '.') {
                std::string frac = lex.next().text;
                if (lex.peek().type == TokType::Number) {
                    std::string fs = std::to_string(iv.ival) + frac + lex.next().text;
                    try { ind.x_offset = std::stof(fs); } catch(...) {}
                }
            }
            if (lex.peek().type == TokType::Percent) lex.next();
        }
        else if (k.text == "y_offset") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            Token iv = lex.next();
            ind.y_offset = (float)iv.ival;
            if (lex.peek().type == TokType::Error &&
                !lex.peek().text.empty() && lex.peek().text[0] == '.') {
                std::string frac = lex.next().text;
                if (lex.peek().type == TokType::Number) {
                    std::string fs = std::to_string(iv.ival) + frac + lex.next().text;
                    try { ind.y_offset = std::stof(fs); } catch(...) {}
                }
            }
            if (lex.peek().type == TokType::Percent) lex.next();
        }
        else { lex.next(); }

        if (lex.peek().type == TokType::Comma) lex.next();
    }
    if (lex.peek().type == TokType::RBrace) lex.next();
    return true;
}

bool matroska_script_interpretor_c::parseTimerBar(
    Lexer & lex, TimerBarSpec & tb)
{
    Token t = lex.peek();
    if (t.type == TokType::Ident && t.text == "none") {
        lex.next();
        tb.enabled = false;
        return true;
    }
    if (t.type != TokType::LBrace) return false;
    lex.next();

    tb.enabled = true;

    while (lex.peek().type != TokType::RBrace &&
           lex.peek().type != TokType::Eof)
    {
        Token k = lex.peek();
        if (k.type != TokType::Ident) { lex.next(); continue; }

        auto parseFloatPct = [&](float & f) {
            if (lex.peek().type == TokType::Colon) lex.next();
            Token iv = lex.next(); // integer part
            f = (float)iv.ival;
            // decimal part: e.g. 80.2
            if (lex.peek().type == TokType::Error &&
                !lex.peek().text.empty() && lex.peek().text[0] == '.') {
                std::string frac = lex.next().text;
                if (lex.peek().type == TokType::Number) {
                    std::string fs = std::to_string(iv.ival) + frac + lex.next().text;
                    try { f = std::stof(fs); } catch(...) {}
                }
            }
            if (lex.peek().type == TokType::Percent) lex.next();
        };

        if      (k.text == "x")              { lex.next(); parseFloatPct(tb.x); }
        else if (k.text == "y")              { lex.next(); parseFloatPct(tb.y); }
        else if (k.text == "width")          { lex.next(); parseFloatPct(tb.width); }
        else if (k.text == "height")         { lex.next(); parseFloatPct(tb.height); }
        else if (k.text == "min_percentage") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            // Parse as float; value may be written as 0 or 0.022
            // evalExpr only handles integers, so handle decimal manually
            Token v = lex.next();
            if (v.type == TokType::Number) {
                tb.min_pct = (float)v.ival;
                // Check for decimal: N.NNNN
                if (lex.peek().type == TokType::Error &&
                    !lex.peek().text.empty() && lex.peek().text[0] == '.') {
                    // consume the dot-and-digits as a float suffix
                    std::string frac = lex.next().text; // '.'
                    Token digits = lex.peek();
                    if (digits.type == TokType::Number) {
                        lex.next();
                        std::string fs = std::to_string(v.ival) + frac + digits.text;
                        try { tb.min_pct = std::stof(fs); } catch (...) {}
                    }
                }
            }
        }
        else if (k.text == "steps") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            tb.steps = (int)evalExpr(lex);
        }
        else if (k.text == "background") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            Token v = lex.peek();
            if (v.type == TokType::Ident && v.text == "none") {
                lex.next();
                tb.has_background = false;
            } else if (v.type == TokType::StringLit) {
                lex.next();
                tb.background     = v.text;
                tb.has_background = true;
            }
        }
        else if (k.text == "fill") {
            lex.next();
            if (lex.peek().type == TokType::Colon) lex.next();
            Token v = lex.next();
            if (v.type == TokType::StringLit) tb.fill = v.text;
        }
        else { lex.next(); }

        if (lex.peek().type == TokType::Comma) lex.next();
    }
    if (lex.peek().type == TokType::RBrace) lex.next();
    return true;
}

bool matroska_script_interpretor_c::execSetTimerBar(Lexer & lex)
{
    lex.next(); // consume 'SetTimerBar'
    if (!expect(lex, TokType::LParen, "SetTimerBar")) return false;

    Token t = lex.peek();
    if (t.type == TokType::Ident && t.text == "none") {
        lex.next();
        default_timer_bar = TimerBarSpec{}; // disabled
        vlc_info(l, "MKVScript: SetTimerBar(none) — timer bar disabled");
    } else {
        // Parse as a brace block or inline named params (same as timer_bar: {...})
        // Wrap in braces for parseTimerBar if not already
        TimerBarSpec tb;
        tb.enabled = true;

        // Peek: if next is '{', delegate to parseTimerBar
        if (lex.peek().type == TokType::LBrace) {
            parseTimerBar(lex, tb);
        } else {
            // Inline named params (no braces): parse until ')'
            while (lex.peek().type != TokType::RParen &&
                   lex.peek().type != TokType::Eof)
            {
                Token k = lex.peek();
                if (k.type != TokType::Ident) { lex.next(); continue; }

                auto parseFloatPct = [&](float & f) {
                if (lex.peek().type == TokType::Colon) lex.next();
                Token iv = lex.next();
                f = (float)iv.ival;
                    if (lex.peek().type == TokType::Error &&
                !lex.peek().text.empty() && lex.peek().text[0] == '.') {
                        std::string frac = lex.next().text;
                        if (lex.peek().type == TokType::Number) {
                            std::string fs = std::to_string(iv.ival) + frac + lex.next().text;
                            try { f = std::stof(fs); } catch(...) {}
                }
            }
            if (lex.peek().type == TokType::Percent) lex.next();
        };

        if      (k.text == "x")              { lex.next(); parseFloatPct(tb.x); }
        else if (k.text == "y")              { lex.next(); parseFloatPct(tb.y); }
        else if (k.text == "width")          { lex.next(); parseFloatPct(tb.width); }
        else if (k.text == "height")         { lex.next(); parseFloatPct(tb.height); }
                else if (k.text == "min_percentage") {
                    lex.next();
                    if (lex.peek().type == TokType::Colon) lex.next();
                    Token v = lex.next();
                    if (v.type == TokType::Number) tb.min_pct = (float)v.ival;
                }
                else if (k.text == "steps") {
                    lex.next();
                    if (lex.peek().type == TokType::Colon) lex.next();
                    tb.steps = (int)evalExpr(lex);
                }
                else if (k.text == "background") {
                    lex.next();
                    if (lex.peek().type == TokType::Colon) lex.next();
                    Token v = lex.peek();
                    if (v.type == TokType::Ident && v.text == "none") {
                        lex.next(); tb.has_background = false;
                    } else if (v.type == TokType::StringLit) {
                        lex.next(); tb.background = v.text; tb.has_background = true;
                    }
                }
                else if (k.text == "fill") {
                    lex.next();
                    if (lex.peek().type == TokType::Colon) lex.next();
                    Token v = lex.next();
                    if (v.type == TokType::StringLit) tb.fill = v.text;
                }
                else { lex.next(); }

                if (lex.peek().type == TokType::Comma) lex.next();
            }
        }
        default_timer_bar = tb;
        vlc_info(l, "MKVScript: SetTimerBar() — timer bar configured x=%.1f%% y=%.1f%% w=%.1f%% h=%.1f%%",
                 tb.x, tb.y, tb.width, tb.height);
    }

    if (!expect(lex, TokType::RParen, "SetTimerBar")) return false;
    if (lex.peek().type == TokType::Semicolon) lex.next();
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Block dispatch
// ─────────────────────────────────────────────────────────────────────────────

bool matroska_script_interpretor_c::dispatchBlock(const std::string & label)
{
    if (current_script.empty()) {
        vlc_debug(l, "MKVScript: dispatchBlock('%s'): no current script",
                  label.c_str());
        return false;
    }

    Lexer scanner(current_script);
    while (scanner.peek().type != TokType::Eof) {
        Token t = scanner.next();
        if (t.type == TokType::Ident && t.text == label) {
            if (scanner.peek().type == TokType::Colon) {
                scanner.next();
                if (scanner.peek().type != TokType::LBrace) return false;
                scanner.next();
                vlc_debug(l, "MKVScript: dispatchBlock('%s'): executing",
                          label.c_str());
                while (scanner.peek().type != TokType::RBrace &&
                       scanner.peek().type != TokType::Eof) {
                    bool jumped = execStmt(scanner);
                    if (jumped) {
                        int depth = 1;
                        while (depth > 0 && scanner.peek().type != TokType::Eof) {
                            Token d = scanner.next();
                            if (d.type == TokType::LBrace) ++depth;
                            else if (d.type == TokType::RBrace) --depth;
                        }
                        return true;
                    }
                }
                if (scanner.peek().type == TokType::RBrace) scanner.next();
                return false;
            }
        }
    }

    vlc_debug(l, "MKVScript: dispatchBlock('%s'): not found", label.c_str());
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Top-level entry point
// ─────────────────────────────────────────────────────────────────────────────

bool matroska_script_interpretor_c::Interpret(
    MatroskaChapterProcessTime time,
    const binary * p_command,
    size_t i_size)
{
    std::string script(reinterpret_cast<const char*>(p_command), i_size);
    current_script = script;

    vlc_info(l, "MKVScript: executing (%s): %.80s%s",
             time == MATROSKA_CHAPPROCESSTIME_BEFORE ? "enter" : "leave",
             script.c_str(),
             script.size() > 80 ? "..." : "");

    Lexer lex(script);
    return execBlock(lex);
}

} // namespace mkv
