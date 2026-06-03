// Copyright (C) 2003-2024 VLC authors and VideoLAN
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// chapter_command_script.hpp : MatroskaScript codec for Matroska Chapter Codecs
// Authors: Laurent Aimar <fenrir@via.ecp.fr>
//          Steve Lhomme <steve.lhomme@free.fr>

#ifndef VLC_MKV_CHAPTER_COMMAND_SCRIPT_HPP_
#define VLC_MKV_CHAPTER_COMMAND_SCRIPT_HPP_

#include "chapter_command_script_common.hpp"
#include <unordered_map>
#include <string>
#include <vector>
#include <cstdint>

namespace mkv {

// ── Token types ───────────────────────────────────────────────────────────────

enum class TokType {
    // Literals / identifiers
    Number, Ident, StringLit,
    // Punctuation
    LParen, RParen, LBrace, RBrace, Comma, Semicolon, Colon,
    // Arithmetic operators
    Plus, Minus, Star, Slash, Percent,
    // Comparison operators
    EqEq, NotEq, Lt, Gt, LtEq, GtEq,
    // Logical operators
    And, Or, Not,
    // Assignment (used in Let)
    Assign,
    // End of input
    Eof,
    // Error sentinel
    Error
};

struct Token {
    TokType  type;
    std::string text;   // raw text for Ident, StringLit, Number
    int64_t  ival = 0;  // pre-parsed value for Number tokens
};

// ── Interpreter ───────────────────────────────────────────────────────────────

class matroska_script_interpretor_c : public matroska_script_interpreter_common_c
{
public:
    matroska_script_interpretor_c( struct vlc_logger *log, chapter_codec_vm & vm_ )
    :matroska_script_interpreter_common_c(log, vm_)
    {}

    bool Interpret( MatroskaChapterProcessTime, const binary * p_command, size_t i_size ) override;

    // Legacy command string (kept for compatibility)
    static const std::string CMD_MS_GOTO_AND_PLAY;

    // Session-persistent variable store: name → integer value
    // Variables are initialized to 0 on first access (default-constructed int64)
    std::unordered_map<std::string, int64_t> vars;

    // Current script text — set during Interpret(), used for block dispatch
    std::string current_script;

private:
    // ── Tokenizer ─────────────────────────────────────────────────────────────
    struct Lexer {
        const std::string & src;
        size_t pos = 0;

        explicit Lexer(const std::string & s) : src(s) {}

        void skipWS();
        void skipLineComment();
        Token next();
        Token peek();
    private:
        size_t saved_pos = 0;
        bool   has_peek  = false;
        Token  peek_tok;
    };

    // ── Parser / evaluator ────────────────────────────────────────────────────
    // Returns true if a GotoAndPlay was executed (playback should jump)
    bool execBlock   ( Lexer & lex );
    bool execStmt    ( Lexer & lex );
    bool execSelect  ( Lexer & lex );
    bool execMenu    ( Lexer & lex );
    bool execIf      ( Lexer & lex );
    bool execLet     ( Lexer & lex );
    bool execGoto    ( Lexer & lex );
    bool execPanic   ( Lexer & lex );
    bool execLog     ( Lexer & lex );
    bool dispatchBlock( const std::string & label );  // find and execute a named block
    bool execStmtWithIdent( Lexer & lex, Token ident );

    int64_t evalExpr ( Lexer & lex );           // arithmetic / boolean expression
    int64_t evalOr   ( Lexer & lex );
    int64_t evalAnd  ( Lexer & lex );
    int64_t evalNot  ( Lexer & lex );
    int64_t evalCmp  ( Lexer & lex );
    int64_t evalAdd  ( Lexer & lex );
    int64_t evalMul  ( Lexer & lex );
    int64_t evalUnary( Lexer & lex );
    int64_t evalPrim ( Lexer & lex );

    // Execute a GotoAndPlay(uid) — shared by execGoto and Menu/Select branches
    bool doGoto( chapter_uid uid );

    // Interpolate {varname} in a string for Log()
    std::string interpolate( const std::string & tmpl );

    // Expect and consume a specific token, return false on mismatch
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
