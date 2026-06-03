// Copyright (C) 2003-2024 VLC authors and VideoLAN
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// chapter_command_script.cpp : Matroska Script Codec for Matroska Chapter Codecs
// Authors: Laurent Aimar <fenrir@via.ecp.fr>
//          Steve Lhomme <steve.lhomme@free.fr>

#include "chapter_command_script.hpp"
#include "virtual_segment.hpp"

#include <cctype>
#include <stdexcept>
#include <sstream>

namespace mkv {

const std::string matroska_script_interpretor_c::CMD_MS_GOTO_AND_PLAY = "GotoAndPlay";

// ─────────────────────────────────────────────────────────────────────────────
// Lexer
// ─────────────────────────────────────────────────────────────────────────────

void matroska_script_interpretor_c::Lexer::skipWS()
{
    while (pos < src.size()) {
        if (std::isspace((unsigned char)src[pos])) {
            ++pos;
        } else if (pos + 1 < src.size() && src[pos] == '/' && src[pos+1] == '/') {
            // Line comment — skip to end of line
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
        saved_pos = pos;
        peek_tok  = next();
        has_peek  = true;
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

    // Number literal
    if (std::isdigit((unsigned char)c)) {
        size_t start = pos;
        while (pos < src.size() && std::isdigit((unsigned char)src[pos]))
            ++pos;
        std::string s = src.substr(start, pos - start);
        int64_t v = 0;
        try { v = std::stoll(s); } catch (...) {}
        return {TokType::Number, s, v};
    }

    // Identifier or keyword
    if (std::isalpha((unsigned char)c) || c == '_') {
        size_t start = pos;
        while (pos < src.size() &&
               (std::isalnum((unsigned char)src[pos]) || src[pos] == '_'))
            ++pos;
        return {TokType::Ident, src.substr(start, pos - start)};
    }

    // String literal (double-quoted)
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
        if (pos < src.size()) ++pos; // consume closing "
        return {TokType::StringLit, s};
    }

    // Two-character operators
    if (pos + 1 < src.size()) {
        std::string two = src.substr(pos, 2);
        if (two == "==") { pos += 2; return {TokType::EqEq,  "=="}; }
        if (two == "!=") { pos += 2; return {TokType::NotEq, "!="}; }
        if (two == "<=") { pos += 2; return {TokType::LtEq,  "<="}; }
        if (two == ">=") { pos += 2; return {TokType::GtEq,  ">="}; }
        if (two == "&&") { pos += 2; return {TokType::And,   "&&"}; }
        if (two == "||") { pos += 2; return {TokType::Or,    "||"}; }
    }

    // Single-character tokens
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
// Expression evaluator  (recursive descent, standard C precedence)
// ─────────────────────────────────────────────────────────────────────────────

// expr → or
int64_t matroska_script_interpretor_c::evalExpr(Lexer & lex)
{
    return evalOr(lex);
}

// or → and ( '||' and )*
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

// and → not ( '&&' not )*
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

// not → '!' not | cmp
int64_t matroska_script_interpretor_c::evalNot(Lexer & lex)
{
    if (lex.peek().type == TokType::Not) {
        lex.next();
        return evalNot(lex) ? 0 : 1;
    }
    return evalCmp(lex);
}

// cmp → add ( ('=='|'!='|'<'|'>'|'<='|'>=') add )?
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

// add → mul ( ('+'|'-') mul )*
int64_t matroska_script_interpretor_c::evalAdd(Lexer & lex)
{
    int64_t v = evalMul(lex);
    for (;;) {
        TokType t = lex.peek().type;
        if (t == TokType::Plus) {
            lex.next();
            v += evalMul(lex);
        } else if (t == TokType::Minus) {
            lex.next();
            v -= evalMul(lex);
        } else break;
    }
    return v;
}

// mul → unary ( ('*'|'/'|'%') unary )*
int64_t matroska_script_interpretor_c::evalMul(Lexer & lex)
{
    int64_t v = evalUnary(lex);
    for (;;) {
        TokType t = lex.peek().type;
        if (t == TokType::Star) {
            lex.next();
            v *= evalUnary(lex);
        } else if (t == TokType::Slash) {
            lex.next();
            int64_t r = evalUnary(lex);
            v = r ? (v / r) : 0;   // divide-by-zero → 0
        } else if (t == TokType::Percent) {
            lex.next();
            int64_t r = evalUnary(lex);
            v = r ? (v % r) : 0;
        } else break;
    }
    return v;
}

// unary → '-' unary | primary
int64_t matroska_script_interpretor_c::evalUnary(Lexer & lex)
{
    if (lex.peek().type == TokType::Minus) {
        lex.next();
        return -evalUnary(lex);
    }
    return evalPrim(lex);
}

// primary → Number | Ident | '(' expr ')'
int64_t matroska_script_interpretor_c::evalPrim(Lexer & lex)
{
    Token t = lex.peek();
    if (t.type == TokType::Number) {
        lex.next();
        return t.ival;
    }
    if (t.type == TokType::Ident) {
        lex.next();
        // Variable lookup — default 0 if not set
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
    lex.next(); // consume to avoid infinite loop
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Statement executors
// ─────────────────────────────────────────────────────────────────────────────

// Execute a block: label ':' '{' stmts '}'
// Or if we're called from inside Select/if body: just '{' stmts '}'
// Returns true if a GotoAndPlay was executed.
bool matroska_script_interpretor_c::execBlock(Lexer & lex)
{
    // Skip optional label: ident ':'
    while (true) {
        Token t = lex.peek();
        if (t.type == TokType::Ident) {
            // Could be a label (ident ':') or a statement starting with ident
            // Save position, consume ident, check for ':'
            Token ident = lex.next();
            Token colon = lex.peek();
            if (colon.type == TokType::Colon) {
                lex.next(); // consume ':'
                // It was a label, now expect '{'
                if (!expect(lex, TokType::LBrace, "block after label"))
                    return false;
                break;
            } else {
                // Not a label — it's a statement. We need to "put back" ident.
                // We can't put back into our simple lexer, so handle statement
                // directly here with the pre-consumed ident.
                return execStmtWithIdent(lex, ident);
            }
        } else if (t.type == TokType::LBrace) {
            lex.next(); // consume '{'
            break;
        } else {
            // No block structure — just execute statements until RBrace or Eof
            break;
        }
    }

    // Execute statements until '}'
    bool result = false;
    while (true) {
        Token t = lex.peek();
        if (t.type == TokType::RBrace || t.type == TokType::Eof)
            break;
        result |= execStmt(lex);
        if (result) {
            // A GotoAndPlay fired — skip remaining statements
            // (drain until matching '}')
            int depth = 1;
            while (depth > 0 && lex.peek().type != TokType::Eof) {
                Token drain = lex.next();
                if (drain.type == TokType::LBrace) ++depth;
                else if (drain.type == TokType::RBrace) --depth;
            }
            return true;
        }
    }
    // Consume the closing '}'
    if (lex.peek().type == TokType::RBrace)
        lex.next();
    return result;
}

// Handle statement when we've already consumed the leading ident token
bool matroska_script_interpretor_c::execStmtWithIdent(Lexer & lex, Token ident)
{
    // The only statement starting with an ident (after labels are stripped)
    // is currently unreachable in normal flow — but handle gracefully
    vlc_debug(l, "MKVScript: unexpected ident '%s' as statement",
              ident.text.c_str());
    // Skip to next semicolon
    while (lex.peek().type != TokType::Semicolon &&
           lex.peek().type != TokType::Eof)
        lex.next();
    if (lex.peek().type == TokType::Semicolon) lex.next();
    return false;
}

// Execute one statement
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

        // Could be a block label — let execBlock handle it
        return execBlock(lex);
    }

    // Skip unknown tokens with a warning
    vlc_debug(l, "MKVScript: unexpected token '%s'", t.text.c_str());
    lex.next();
    // Skip to semicolon
    while (lex.peek().type != TokType::Semicolon &&
           lex.peek().type != TokType::Eof)
        lex.next();
    if (lex.peek().type == TokType::Semicolon) lex.next();
    return false;
}

// Let(name, expr);
bool matroska_script_interpretor_c::execLet(Lexer & lex)
{
    lex.next(); // consume 'Let'
    if (!expect(lex, TokType::LParen, "Let")) return false;

    Token name = lex.next();
    if (name.type != TokType::Ident) {
        vlc_debug(l, "MKVScript: Let: expected variable name");
        return false;
    }
    if (!expect(lex, TokType::Comma, "Let")) return false;

    int64_t val = evalExpr(lex);
    vars[name.text] = val;
    vlc_debug(l, "MKVScript: Let(%s, %" PRId64 ")", name.text.c_str(), val);

    if (!expect(lex, TokType::RParen, "Let")) return false;
    if (lex.peek().type == TokType::Semicolon) lex.next();
    return false; // Let never triggers a jump
}

// GotoAndPlay(uid);
bool matroska_script_interpretor_c::execGoto(Lexer & lex)
{
    lex.next(); // consume 'GotoAndPlay'
    if (!expect(lex, TokType::LParen, "GotoAndPlay")) return false;
    int64_t uid = evalExpr(lex);
    if (!expect(lex, TokType::RParen, "GotoAndPlay")) return false;
    if (lex.peek().type == TokType::Semicolon) lex.next();

    vlc_debug(l, "MKVScript: GotoAndPlay(%" PRId64 ")", uid);
    return doGoto((chapter_uid)uid);
}

// if (expr) { stmts }  [ else { stmts } ]
// Note: 'if' is non-terminal in entry scripts (no GotoAndPlay needed)
bool matroska_script_interpretor_c::execIf(Lexer & lex)
{
    lex.next(); // consume 'if'
    if (!expect(lex, TokType::LParen, "if")) return false;
    int64_t cond = evalExpr(lex);
    if (!expect(lex, TokType::RParen, "if")) return false;

    // Parse the then-block regardless (need to consume tokens)
    if (!expect(lex, TokType::LBrace, "if-then")) return false;

    bool result = false;
    if (cond) {
        // Execute then-block
        while (lex.peek().type != TokType::RBrace &&
               lex.peek().type != TokType::Eof) {
            result |= execStmt(lex);
        }
        if (lex.peek().type == TokType::RBrace) lex.next();

        // Skip else-block if present
        if (lex.peek().type == TokType::Ident && lex.peek().text == "else") {
            lex.next(); // consume 'else'
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
        // Skip then-block
        int depth = 1;
        while (depth > 0 && lex.peek().type != TokType::Eof) {
            Token d = lex.next();
            if (d.type == TokType::LBrace) ++depth;
            else if (d.type == TokType::RBrace) --depth;
        }
        // Execute else-block if present
        if (lex.peek().type == TokType::Ident && lex.peek().text == "else") {
            lex.next();
            if (!expect(lex, TokType::LBrace, "if-else")) return false;
            while (lex.peek().type != TokType::RBrace &&
                   lex.peek().type != TokType::Eof) {
                result |= execStmt(lex);
            }
            if (lex.peek().type == TokType::RBrace) lex.next();
        }
    }
    return result;
}

// Select { Case(expr) { stmts } ... Default { stmts } }
bool matroska_script_interpretor_c::execSelect(Lexer & lex)
{
    lex.next(); // consume 'Select'
    if (!expect(lex, TokType::LBrace, "Select")) return false;

    bool fired  = false;
    bool done   = false;

    while (!done) {
        lex.peek(); // force skipWS
        Token t = lex.peek();

        if (t.type == TokType::RBrace || t.type == TokType::Eof)
            break;

        if (t.type != TokType::Ident) {
            lex.next();
            continue;
        }

        if (t.text == "Case") {
            lex.next(); // consume 'Case'
            if (!expect(lex, TokType::LParen, "Case")) return false;
            int64_t cond = evalExpr(lex);
            if (!expect(lex, TokType::RParen, "Case")) return false;
            if (!expect(lex, TokType::LBrace, "Case body")) return false;

            if (!fired && cond) {
                // Execute this case
                fired = true;
                while (lex.peek().type != TokType::RBrace &&
                       lex.peek().type != TokType::Eof) {
                    bool jumped = execStmt(lex);
                    if (jumped) {
                        // Drain remaining tokens in this case body
                        int depth = 1;
                        while (depth > 0 && lex.peek().type != TokType::Eof) {
                            Token d = lex.next();
                            if (d.type == TokType::LBrace) ++depth;
                            else if (d.type == TokType::RBrace) --depth;
                        }
                        // Drain rest of Select
                        depth = 1;
                        while (depth > 0 && lex.peek().type != TokType::Eof) {
                            Token d = lex.next();
                            if (d.type == TokType::LBrace) ++depth;
                            else if (d.type == TokType::RBrace) --depth;
                        }
                        return true;
                    }
                }
                if (lex.peek().type == TokType::RBrace) lex.next();
                done = true; // first-match: stop evaluating cases
            } else {
                // Skip this case body
                int depth = 1;
                while (depth > 0 && lex.peek().type != TokType::Eof) {
                    Token d = lex.next();
                    if (d.type == TokType::LBrace) ++depth;
                    else if (d.type == TokType::RBrace) --depth;
                }
            }
        } else if (t.text == "Default") {
            lex.next(); // consume 'Default'
            if (!expect(lex, TokType::LBrace, "Default body")) return false;

            if (!fired) {
                // Execute default
                while (lex.peek().type != TokType::RBrace &&
                       lex.peek().type != TokType::Eof) {
                    bool jumped = execStmt(lex);
                    if (jumped) {
                        // Drain
                        int depth = 1;
                        while (depth > 0 && lex.peek().type != TokType::Eof) {
                            Token d = lex.next();
                            if (d.type == TokType::LBrace) ++depth;
                            else if (d.type == TokType::RBrace) --depth;
                        }
                        // Drain rest of Select
                        depth = 1;
                        while (depth > 0 && lex.peek().type != TokType::Eof) {
                            Token d = lex.next();
                            if (d.type == TokType::LBrace) ++depth;
                            else if (d.type == TokType::RBrace) --depth;
                        }
                        return true;
                    }
                }
                if (lex.peek().type == TokType::RBrace) lex.next();
            } else {
                // Skip default body
                int depth = 1;
                while (depth > 0 && lex.peek().type != TokType::Eof) {
                    Token d = lex.next();
                    if (d.type == TokType::LBrace) ++depth;
                    else if (d.type == TokType::RBrace) --depth;
                }
            }
            done = true; // Default is always last
        } else {
            // Unknown ident inside Select — skip
            lex.next();
        }
    }

    // Consume closing '}'
    if (lex.peek().type == TokType::RBrace) lex.next();
    return false;
}

// Menu(timeout: N, default: N, Option("label", callable), ...)
// For now: present all options via Log, auto-select default
// (Full interactive UI comes in the player layer — this is the stub)
bool matroska_script_interpretor_c::execMenu(Lexer & lex)
{
    lex.next(); // consume 'Menu'
    if (!expect(lex, TokType::LParen, "Menu")) return false;

    int64_t timeout_s = 10;
    int64_t default_idx = 1;

    // Structures to hold parsed options
    struct MenuOption {
        std::string label;
        chapter_uid uid = 0;
        bool is_uid = false; // true = GotoAndPlay(uid), false = block label
        std::string block_label;
    };
    std::vector<MenuOption> options;

    // Parse named params and Option() entries until ')'
    while (lex.peek().type != TokType::RParen &&
           lex.peek().type != TokType::Eof)
    {
        Token t = lex.peek();

        // Named parameter: timeout: N or default: N
        if (t.type == TokType::Ident &&
            (t.text == "timeout" || t.text == "default" ||
             t.text == "style"   || t.text == "highlight"))
        {
            lex.next(); // consume param name
            if (lex.peek().type == TokType::Colon) {
                lex.next(); // consume ':'
                if (t.text == "timeout") {
                    timeout_s = evalExpr(lex);
                } else if (t.text == "default") {
                    default_idx = evalExpr(lex);
                } else {
                    // style/highlight — rendering hints, consume value
                    lex.next(); // consume the value ident
                }
            }
        }
        // Option("label", callable)
        else if (t.type == TokType::Ident && t.text == "Option")
        {
            lex.next(); // consume 'Option'
            if (!expect(lex, TokType::LParen, "Option")) return false;

            Token label_tok = lex.next();
            if (label_tok.type != TokType::StringLit) {
                vlc_debug(l, "MKVScript: Option: expected string label");
                return false;
            }
            if (!expect(lex, TokType::Comma, "Option")) return false;

            MenuOption opt;
            opt.label = label_tok.text;

            // callable: either GotoAndPlay(uid) or block_label
            Token callable = lex.peek();
            if (callable.type == TokType::Ident &&
                callable.text == "GotoAndPlay")
            {
                lex.next(); // consume 'GotoAndPlay'
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
        // Skip trailing comment fragments and commas
        else if (t.type == TokType::Comma) {
            lex.next();
        }
        else {
            // Unknown — skip
            lex.next();
        }
    }

    // Consume closing ')'
    if (lex.peek().type == TokType::RParen) lex.next();
    if (lex.peek().type == TokType::Semicolon) lex.next();

    if (options.empty()) {
        vlc_debug(l, "MKVScript: Menu: no options");
        return false;
    }

    // Log available options (placeholder until player UI layer is implemented)
    vlc_debug(l, "MKVScript: Menu timeout=%" PRId64 " default=%" PRId64 " options=%zu",
              timeout_s, default_idx, options.size());
    for (size_t i = 0; i < options.size(); ++i) {
        vlc_debug(l, "MKVScript:   Option %zu: \"%s\"",
                  i+1, options[i].label.c_str());
    }

    // For now: auto-select the default option
    size_t sel = (size_t)(default_idx - 1);
    if (sel >= options.size()) sel = 0;

    const MenuOption & chosen = options[sel];
    vlc_debug(l, "MKVScript: Menu auto-selecting option %zu (\"%s\")",
              sel+1, chosen.label.c_str());

    if (chosen.is_uid) {
        return doGoto(chosen.uid);
    }

    // Block label callable — we need to find and execute the named block
    // The block should be defined later in the same script.
    // We store the block name for execBlock to find via label scanning.
    // For now, log and return false (block dispatch not yet implemented).
    vlc_debug(l, "MKVScript: Menu block callable '%s' (not yet dispatched)",
              chosen.block_label.c_str());
    return false;
}

// Panic("message");
bool matroska_script_interpretor_c::execPanic(Lexer & lex)
{
    lex.next(); // consume 'Panic'
    if (!expect(lex, TokType::LParen, "Panic")) return false;
    Token msg = lex.next();
    std::string text = (msg.type == TokType::StringLit) ? msg.text : msg.text;
    if (!expect(lex, TokType::RParen, "Panic")) return false;
    if (lex.peek().type == TokType::Semicolon) lex.next();

    vlc_debug(l, "MKVScript: PANIC: %s", text.c_str());
    // Panic is terminal — return false (don't claim a jump happened,
    // but playback will naturally stop as no chapter was targeted)
    return false;
}

// Log("message {var}");
bool matroska_script_interpretor_c::execLog(Lexer & lex)
{
    lex.next(); // consume 'Log'
    if (!expect(lex, TokType::LParen, "Log")) return false;
    Token msg = lex.next();
    std::string text = (msg.type == TokType::StringLit) ?
                       interpolate(msg.text) : msg.text;
    if (!expect(lex, TokType::RParen, "Log")) return false;
    if (lex.peek().type == TokType::Semicolon) lex.next();

    vlc_debug(l, "MKVScript: LOG: %s", text.c_str());
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
    vlc_info(l, "MKVScript: executing (%s): %.120s%s",
              time == MATROSKA_CHAPPROCESSTIME_BEFORE ? "enter" : "leave",
              script.c_str(),
              script.size() > 120 ? "..." : "");

    Lexer lex(script);
    bool result = false;

    // A script is a sequence of labeled blocks.
    // Execute blocks in order; stop when a GotoAndPlay fires.
    while (lex.peek().type != TokType::Eof) {
        result = execBlock(lex);
        if (result) break;
    }

    return result;
}

} // namespace mkv
