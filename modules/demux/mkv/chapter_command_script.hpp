// Copyright (C) 2003-2024 VLC authors and VideoLAN
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// chapter_command_script.hpp : MatroskaScript codec for Matroska Chapter Codecs

#ifndef VLC_MKV_CHAPTER_COMMAND_SCRIPT_HPP_
#define VLC_MKV_CHAPTER_COMMAND_SCRIPT_HPP_

#include "chapter_command_script_common.hpp"
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace mkv {

// ── Token types ───────────────────────────────────────────────────────────────

enum class TokType {
    Number, Ident, StringLit,
    LParen, RParen, LBrace, RBrace, Comma, Semicolon, Colon,
    Plus, Minus, Star, Slash, Percent,
    EqEq, NotEq, Lt, Gt, LtEq, GtEq,
    And, Or, Not,
    Assign,
    Eof,
    Error
};

struct Token {
    TokType  type;
    std::string text;
    int64_t  ival = 0;
};

// ── Menu state ────────────────────────────────────────────────────────────────
// Holds a pending interactive menu waiting for user input.

struct IndicatorSpec {
    enum class Type { None, Image, LibASS } type = Type::None;
    std::string    attach_name;   // filename of attachment (image type)
    int64_t        attach_uid = 0;// resolved attachment UID
    std::string    markup;        // ASS override tags (libass type)
    float          x_offset = 0.f;// % of frame width
    float          y_offset = 0.f;// % of frame height
};

struct TimerBarSpec {
    float   x           = 50.f;   // center % of frame width
    float   y           = 80.2f;  // center % of frame height
    float   width       = 100.f;  // % of frame width
    float   height      = 0.4f;   // % of frame height
    float   min_pct     = 0.f;    // 0.0-1.0
    int     steps       = 0;      // 0 = continuous
    bool    has_background = false;
    std::string background; // ASS markup or empty
    std::string fill;       // ASS markup for draining fill
    bool    enabled     = false;
};

struct MenuOption {
    std::string label;
    chapter_uid uid    = 0;
    bool        is_uid = false;
    std::string block_label;
    // Presentation
    float       x      = -1.f;   // center % of frame, -1 = auto
    float       y      = -1.f;
    float       width  = -1.f;
    float       height = -1.f;
    std::string image_attach;    // filename of image attachment (image style)
    int64_t     image_uid = 0;   // resolved UID
};

enum class MenuStyle { None, LibASS, Image };

struct MenuState {
    std::vector<MenuOption> options;
    size_t   selected    = 0;
    size_t   default_idx = 0;
    int64_t  timeout_s   = 10;
    bool     active      = false;
    bool     confirmed   = false;
    bool     osd_dirty   = false;
    bool     jump_pending = false;

    // Presentation
    MenuStyle    style       = MenuStyle::None;
    IndicatorSpec selected_ind;
    IndicatorSpec unselected_ind;
    TimerBarSpec  timer_bar;       // per-menu override (if enabled)

    std::chrono::steady_clock::time_point deadline;
    bool     has_deadline = false;

    std::mutex mtx;
};

// ── Interpreter ───────────────────────────────────────────────────────────────

class matroska_script_interpretor_c : public matroska_script_interpreter_common_c
{
public:
    matroska_script_interpretor_c( struct vlc_logger *log, chapter_codec_vm & vm_ )
    :matroska_script_interpreter_common_c(log, vm_)
    {}

    bool Interpret( MatroskaChapterProcessTime, const binary * p_command, size_t i_size ) override;

    static const std::string CMD_MS_GOTO_AND_PLAY;

    // Session-persistent variable store
    std::unordered_map<std::string, int64_t> vars;

    // Current script text — set during Interpret(), used for block dispatch
    std::string current_script;

    // Pending interactive menu (protected by menu_state.mtx)
    MenuState menu_state;

    // Non-owning pointer to the MenuOSDSys backing the currently-live menu
    // subpicture (if any). Owned by the subpicture's updater and freed by
    // MenuOSDDestroy when the channel is unregistered; never freed here.
    // Guarded by osd_sys_mtx since nav events arrive on the input/event
    // thread while renderMenuOSD()/clearMenuOSD() run on the demux thread.
    void       *p_live_osd_sys = nullptr;
    std::mutex  osd_sys_mtx;

    // Persistent OSD state (set by SetFont / SetTimerBar, applied to all menus)
    TimerBarSpec default_timer_bar;  // set by SetTimerBar(), inherited by all menus

    // Called from event thread (HandleKeyEvent) to navigate/confirm menu
    bool HandleNavEvent( bool up, bool activate );

    // Called from Demux() to dispatch menu result once confirmed/timed-out
    // Returns true if a GotoAndPlay was executed (caller should return immediately)
    bool DispatchMenuResult();

    void renderMenuOSD();
    void clearMenuOSD();

    // Mutate the already-live menu subpicture's selection state in place,
    // without tearing down and recreating the SPU channel. Used by nav
    // events so that changing the highlighted option does not trigger a
    // fresh ES_OUT_VOUT_DEL_OVERLAY/ADD_OVERLAY round trip — each such
    // round trip created a brief window where the outgoing and incoming
    // subpicture channels could both be alive and rendering simultaneously
    // (each running its own copy of the timer countdown), which is what
    // produced the doubled/tripled timer bar artifacts. Returns false if
    // there is no live menu subpicture to update (caller should fall back
    // to a full renderMenuOSD()).
    bool updateMenuOSDSelection();

    // Called by MenuOSDDestroy (free function, runs when a menu subpicture
    // is torn down by any path, including ones outside our control such as
    // a vout flush) to null out p_live_osd_sys if it currently points at
    // the subpicture being destroyed. Prevents a dangling non-owning
    // pointer if teardown happens somewhere other than clearMenuOSD().
    void onMenuOSDDestroyed( void *osd_sys );

private:
    // ── Tokenizer ─────────────────────────────────────────────────────────────
    struct Lexer {
        const std::string & src;
        size_t pos = 0;

        explicit Lexer(const std::string & s) : src(s) {}

        void skipWS();
        Token next();
        Token peek();
    private:
        bool   has_peek  = false;
        Token  peek_tok;
    };

    // ── Parser / evaluator ────────────────────────────────────────────────────
    bool execBlock   ( Lexer & lex );
    bool execStmt    ( Lexer & lex );
    bool execStmtWithIdent( Lexer & lex, Token ident );
    bool execSelect  ( Lexer & lex );
    bool execMenu    ( Lexer & lex );
    bool execIf      ( Lexer & lex );
    bool execLet     ( Lexer & lex );
    bool execGoto    ( Lexer & lex );
    bool execPanic   ( Lexer & lex );
    bool execLog     ( Lexer & lex );
    bool execSetFont     ( Lexer & lex );
    bool execSetTimerBar  ( Lexer & lex );
    bool execOption       ( Lexer & lex, MenuOption & opt );
    bool parseIndicator   ( Lexer & lex, IndicatorSpec & ind );
    bool parseTimerBar    ( Lexer & lex, TimerBarSpec & tb );
    bool dispatchBlock( const std::string & label );
    bool dispatchCallable( const MenuOption & opt );

    int64_t evalExpr ( Lexer & lex );
    int64_t evalOr   ( Lexer & lex );
    int64_t evalAnd  ( Lexer & lex );
    int64_t evalNot  ( Lexer & lex );
    int64_t evalCmp  ( Lexer & lex );
    int64_t evalAdd  ( Lexer & lex );
    int64_t evalMul  ( Lexer & lex );
    int64_t evalUnary( Lexer & lex );
    int64_t evalPrim ( Lexer & lex );

    bool doGoto( chapter_uid uid );
    std::string interpolate( const std::string & tmpl );
    bool expect( Lexer & lex, TokType t, const char * ctx );
};

// ── Codec wrapper ─────────────────────────────────────────────────────────────

class matroska_script_codec_c : public matroska_script_codec_common_c
{
public:
    matroska_script_codec_c( struct vlc_logger *log, chapter_codec_vm & vm_,
                             matroska_script_interpretor_c & interpreter_)
    :matroska_script_codec_common_c( log, vm_, MATROSKA_CHAPTER_CODEC_NATIVE )
    ,interpreter( interpreter_ )
    {}

    matroska_script_interpreter_common_c & get_interpreter() override
    {
        return interpreter;
    }

protected:
    matroska_script_interpretor_c & interpreter;
};

} // namespace mkv

#endif // VLC_MKV_CHAPTER_COMMAND_SCRIPT_HPP_
