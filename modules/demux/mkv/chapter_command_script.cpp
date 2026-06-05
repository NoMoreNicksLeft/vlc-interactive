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

struct MenuOSDSys {
    char *text;
};

static void MenuOSDUpdate( subpicture_t *subpic,
                           const struct vlc_spu_updater_configuration *cfg )
{
    MenuOSDSys *sys = static_cast<MenuOSDSys*>( subpic->updater.sys );
    const video_format_t *fmt_dst = cfg->current.video_dst;

    if( video_format_IsSimilar( cfg->previous.video_dst, fmt_dst ) )
        return;

    vlc_spu_regions_Clear( &subpic->regions );

    subpic->i_original_picture_width  = fmt_dst->i_visible_width
                                        * fmt_dst->i_sar_num / fmt_dst->i_sar_den;
    subpic->i_original_picture_height = fmt_dst->i_visible_height;

    subpicture_region_t *r = subpicture_region_NewText();
    if( !r ) return;
    vlc_spu_regions_push( &subpic->regions, r );

    r->fmt.i_sar_num = 1;
    r->fmt.i_sar_den = 1;
    r->p_text        = text_segment_New( sys->text );

    const float margin = 0.05f;
    r->i_align   = SUBPICTURE_ALIGN_TOP | SUBPICTURE_ALIGN_LEFT;
    r->text_flags |= r->i_align;
    r->b_absolute = false;
    r->b_in_window = false;
    r->i_x = (int)( margin * fmt_dst->i_visible_width  ) + fmt_dst->i_x_offset;
    r->i_y = (int)( margin * fmt_dst->i_visible_height ) + fmt_dst->i_y_offset;
}

static void MenuOSDDestroy( subpicture_t *subpic )
{
    MenuOSDSys *sys = static_cast<MenuOSDSys*>( subpic->updater.sys );
    free( sys->text );
    free( sys );
}

static const struct vlc_spu_updater_ops menu_osd_ops = {
    .update  = MenuOSDUpdate,
    .destroy = MenuOSDDestroy,
};

static subpicture_t * MakeMenuSubpicture( const char *text )
{
    MenuOSDSys *sys = static_cast<MenuOSDSys*>( malloc( sizeof(*sys) ) );
    if( !sys ) return nullptr;
    sys->text = strdup( text );
    if( !sys->text ) { free(sys); return nullptr; }

    subpicture_updater_t updater = { .sys = sys, .ops = &menu_osd_ops };
    subpicture_t *subpic = subpicture_New( &updater );
    if( !subpic ) { free(sys->text); free(sys); return nullptr; }

    subpic->i_channel   = 0;
    subpic->i_start     = vlc_tick_now();
    subpic->i_stop      = VLC_TICK_INVALID;
    subpic->b_ephemer   = false;
    subpic->b_fade      = false;
    subpic->b_subtitle  = false;
    return subpic;
}

void matroska_script_interpretor_c::renderMenuOSD()
{
    demux_sys_t & sys = static_cast<demux_sys_t &>( vm );

    // Build menu text
    std::ostringstream oss;
    for (size_t i = 0; i < menu_state.options.size(); ++i) {
        oss << (i == menu_state.selected ? "> " : "  ");
        oss << menu_state.options[i].label << "\n";
    }
    std::string text = oss.str();
    vlc_info(l, "MKVScript: MENU: %s", text.c_str());

    if( sys.p_video_es == nullptr )
        return; // no video ES yet — OSD not available

    // Remove previous overlay if any
    if( sys.i_menu_overlay_id != SIZE_MAX ) {
        es_out_Control( sys.demuxer.out, ES_OUT_VOUT_DEL_OVERLAY,
                        sys.p_video_es, sys.i_menu_overlay_id );
        sys.i_menu_overlay_id = SIZE_MAX;
    }

    subpicture_t *subpic = MakeMenuSubpicture( text.c_str() );
    if( !subpic ) return;

    size_t channel_id = SIZE_MAX;
    if( es_out_Control( sys.demuxer.out, ES_OUT_VOUT_ADD_OVERLAY,
                        sys.p_video_es, subpic, &channel_id ) == VLC_SUCCESS ) {
        sys.i_menu_overlay_id = channel_id;
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

    // Mark OSD as needing redraw — renderMenuOSD() will be called from
    // Demux() on the demux thread where es_out_Control is safe.
    menu_state.osd_dirty = true;
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
        menu_state.jump_pending = true;  // suppress next Enter in UpdateCurrentToChapter
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
        } else if (t == TokType::Percent) {
            lex.next(); int64_t r = evalUnary(lex); v = r ? (v%r) : 0;
        } else break;
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

    while (lex.peek().type != TokType::RParen &&
           lex.peek().type != TokType::Eof)
    {
        Token t = lex.peek();

        if (t.type == TokType::Ident &&
            (t.text == "timeout" || t.text == "default" ||
             t.text == "style"   || t.text == "highlight"))
        {
            lex.next();
            if (lex.peek().type == TokType::Colon) {
                lex.next();
                if (t.text == "timeout")       timeout_s   = evalExpr(lex);
                else if (t.text == "default")  default_idx = evalExpr(lex);
                else                           lex.next(); // rendering hints: skip value
            }
        }
        else if (t.type == TokType::Ident && t.text == "Option")
        {
            lex.next();
            if (!expect(lex, TokType::LParen, "Option")) return false;
            Token label_tok = lex.next();
            if (label_tok.type != TokType::StringLit) return false;
            if (!expect(lex, TokType::Comma, "Option")) return false;

            MenuOption opt;
            opt.label = label_tok.text;

            Token callable = lex.peek();
            if (callable.type == TokType::Ident && callable.text == "GotoAndPlay") {
                lex.next();
                if (!expect(lex, TokType::LParen, "GotoAndPlay in Option")) return false;
                opt.uid    = (chapter_uid)evalExpr(lex);
                opt.is_uid = true;
                if (!expect(lex, TokType::RParen, "GotoAndPlay in Option")) return false;
            } else if (callable.type == TokType::Ident) {
                lex.next();
                opt.block_label = callable.text;
                opt.is_uid      = false;
            }

            if (!expect(lex, TokType::RParen, "Option")) return false;
            options.push_back(opt);
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

    // Clamp default index (convert from 1-based to 0-based)
    size_t def = (size_t)(default_idx - 1);
    if (def >= options.size()) def = 0;

    vlc_info(l, "MKVScript: Menu with %zu options (timeout=%" PRId64 "s default=%zu)",
             options.size(), timeout_s, def + 1);
    for (size_t i = 0; i < options.size(); ++i)
        vlc_info(l, "MKVScript:   [%zu] %s", i+1, options[i].label.c_str());

    // ── Set up menu state — non-blocking ─────────────────────────────────────
    // We do NOT block here. Instead we set menu_state.active = true and return.
    // Demux() will stall (returning success with no data) until the menu
    // resolves, then call DispatchMenuResult() to execute the chosen option.
    {
        std::unique_lock<std::mutex> lk(menu_state.mtx);
        menu_state.options     = options;
        menu_state.selected    = def;
        menu_state.default_idx = def;
        menu_state.timeout_s   = timeout_s;
        menu_state.confirmed   = false;
        menu_state.active      = true;
        menu_state.osd_dirty   = true;  // render on next Demux() poll

        if (timeout_s > 0) {
            menu_state.deadline     = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(timeout_s);
            menu_state.has_deadline = true;
        } else {
            menu_state.has_deadline = false;
        }
    }

    renderMenuOSD();
    vlc_info(l, "MKVScript: Menu activated (non-blocking) — Demux() will poll");

    // Return false: no navigation happened yet.
    // Demux() will call DispatchMenuResult() once user input or timeout fires.
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
