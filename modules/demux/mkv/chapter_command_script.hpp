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

struct MenuOption {
    std::string label;
    chapter_uid uid    = 0;
    bool        is_uid = false;       // true = direct GotoAndPlay
    std::string block_label;          // non-empty if block dispatch
};

struct MenuState {
    std::vector<MenuOption> options;
    size_t   selected    = 0;         // 0-based index of highlighted option
    size_t   default_idx = 0;         // 0-based index of default
    int64_t  timeout_s   = 10;
    bool     active      = false;     // true while waiting for input
    bool     confirmed   = false;     // set by HandleNavEvent when user activates
    bool     osd_dirty   = false;     // set when selection changes, cleared after render
    bool     jump_pending = false;    // set after dispatch, cleared after UpdateCurrentToChapter runs

    // Non-blocking: deadline for timeout (set when menu becomes active)
    std::chrono::steady_clock::time_point deadline;
    bool     has_deadline = false;    // false = no timeout

    std::mutex mtx;                   // protects selected/confirmed/active
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

    // Called from event thread (HandleKeyEvent) to navigate/confirm menu
    bool HandleNavEvent( bool up, bool activate );

    // Called from Demux() to dispatch menu result once confirmed/timed-out
    // Returns true if a GotoAndPlay was executed (caller should return immediately)
    bool DispatchMenuResult();

    void renderMenuOSD();
    void clearMenuOSD();

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
