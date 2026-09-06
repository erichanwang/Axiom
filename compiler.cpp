// Axiom compiler: lexer -> parser -> AST -> (interpreter | x86-64 codegen)
//
// Language grammar (see syntax.txt):
//   program := (funcdecl | stmt)*
//   funcdecl:= "func" IDENT "(" (IDENT ("," IDENT)*)? ")" block
//   stmt    := "prt" expr
//            | "input" IDENT
//            | IDENT "=" expr
//            | "if" "(" expr ")" block ("else" "if" "(" expr ")" block)* ("else" block)?
//            | "while" "(" expr ")" block
//            | "break" | "continue"
//            | "return" expr?
//            | call
//   block   := "{" stmt* "}"
//   expr    := comparison
//   comparison  := additive (("=="|"!="|">="|"<="|">"|"<") additive)*
//   additive    := multiplicative (("+"|"-") multiplicative)*
//   multiplicative := unary (("*"|"/"|"%") unary)*
//   unary   := "-" unary | postfix
//   postfix := primary ("(" (expr ("," expr)*)? ")")?
//   primary := NUMBER | STRING | IDENT | "(" expr ")"
//
// Any statement that doesn't match one of these shapes (e.g. a stray "let x = 10"
// or "print x" from the old ad-hoc interpreter's undocumented quirks) is silently
// skipped, same as the legacy interpreter did. This is intentional legacy
// compatibility, not new syntax -- see README for the compatibility note.
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <cstdlib>
#include <cstdio>
#include <cmath>

using namespace std;

// ===================== Lexer =====================

enum class Tok {
    IDENT, NUMBER, STRING,
    LPAREN, RPAREN, LBRACE, RBRACE, LBRACKET, RBRACKET, COMMA,
    EQ, EQEQ, NEQ, GT, LT, GE, LE,
    PLUS, MINUS, STAR, SLASH, PERCENT,
    NEWLINE, END
};

struct Token {
    Tok type;
    string text;   // IDENT name / STRING contents
    double num = 0;
};

static vector<Token> tokenize(const string& src) {
    vector<Token> out;
    size_t i = 0, n = src.size();
    while (i < n) {
        char c = src[i];
        if (c == '\n') { out.push_back({Tok::NEWLINE, "\n"}); i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '#') { while (i < n && src[i] != '\n') i++; continue; } // line comment
        if (c == '"') {
            size_t j = i + 1;
            string s;
            while (j < n && src[j] != '"') { s += src[j]; j++; }
            out.push_back({Tok::STRING, s});
            i = (j < n) ? j + 1 : j; // skip closing quote if present
            continue;
        }
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)src[i+1]))) {
            size_t j = i;
            while (j < n && (isdigit((unsigned char)src[j]) || src[j] == '.')) j++;
            Token t{Tok::NUMBER, src.substr(i, j - i)};
            t.num = strtod(t.text.c_str(), nullptr);
            out.push_back(t);
            i = j;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            out.push_back({Tok::IDENT, src.substr(i, j - i)});
            i = j;
            continue;
        }
        if (c == '(') { out.push_back({Tok::LPAREN, "("}); i++; continue; }
        if (c == ')') { out.push_back({Tok::RPAREN, ")"}); i++; continue; }
        if (c == '{') { out.push_back({Tok::LBRACE, "{"}); i++; continue; }
        if (c == '}') { out.push_back({Tok::RBRACE, "}"}); i++; continue; }
        if (c == '[') { out.push_back({Tok::LBRACKET, "["}); i++; continue; }
        if (c == ']') { out.push_back({Tok::RBRACKET, "]"}); i++; continue; }
        if (c == ',') { out.push_back({Tok::COMMA, ","}); i++; continue; }
        if (c == '=' && i + 1 < n && src[i+1] == '=') { out.push_back({Tok::EQEQ, "=="}); i += 2; continue; }
        if (c == '!' && i + 1 < n && src[i+1] == '=') { out.push_back({Tok::NEQ, "!="}); i += 2; continue; }
        if (c == '>' && i + 1 < n && src[i+1] == '=') { out.push_back({Tok::GE, ">="}); i += 2; continue; }
        if (c == '<' && i + 1 < n && src[i+1] == '=') { out.push_back({Tok::LE, "<="}); i += 2; continue; }
        if (c == '=') { out.push_back({Tok::EQ, "="}); i++; continue; }
        if (c == '>') { out.push_back({Tok::GT, ">"}); i++; continue; }
        if (c == '<') { out.push_back({Tok::LT, "<"}); i++; continue; }
        if (c == '+') { out.push_back({Tok::PLUS, "+"}); i++; continue; }
        if (c == '-') { out.push_back({Tok::MINUS, "-"}); i++; continue; }
        if (c == '*') { out.push_back({Tok::STAR, "*"}); i++; continue; }
        if (c == '/') { out.push_back({Tok::SLASH, "/"}); i++; continue; }
        if (c == '%') { out.push_back({Tok::PERCENT, "%"}); i++; continue; }
        // Unknown character: skip it (kept liberal, matches old interpreter's
        // "ignore what it doesn't understand" behavior).
        i++;
    }
    out.push_back({Tok::END, ""});
    return out;
}

// ===================== AST =====================

enum class ExprKind { NUMBER, STRING, IDENT, BINOP, UNARY, CALL, ARRAY, INDEX };

struct Expr {
    ExprKind kind;
    double num = 0;
    string str;                    // string literal / ident name / op text / callee name
    unique_ptr<Expr> left, right;  // BINOP operands; UNARY uses left; INDEX: left=base, right=index
    vector<unique_ptr<Expr>> args; // CALL arguments / ARRAY literal elements
};

enum class StmtKind { PRT, INPUT, ASSIGN, IF, WHILE, FUNC, RETURN, BREAK, CONTINUE, CALLSTMT, INDEXSET };

struct Stmt;
struct IfBranch {
    unique_ptr<Expr> cond;
    vector<unique_ptr<Stmt>> body;
};

struct Stmt {
    StmtKind kind;
    unique_ptr<Expr> expr;              // PRT expr / ASSIGN rhs / RETURN value / CALLSTMT call
    string name;                        // INPUT var / ASSIGN var / FUNC name
    vector<IfBranch> branches;          // IF: if + else-if branches, in order
    vector<unique_ptr<Stmt>> elseBody;  // IF: else body (empty if no else)
    bool hasElse = false;
    unique_ptr<Expr> cond;              // WHILE condition
    vector<unique_ptr<Stmt>> body;      // WHILE body / FUNC body
    vector<string> params;              // FUNC parameters
    unique_ptr<Expr> indexTarget;       // INDEXSET: base array (always an IDENT expr)
    unique_ptr<Expr> indexExpr;         // INDEXSET: index expression
};

using Program = vector<unique_ptr<Stmt>>;

// Names a function treats as locals: its parameters plus everything it assigns
// to or reads via `input`. Both backends call this, so the interpreter and the
// codegen agree on exactly which names are frame slots and which are globals.
static void collectAssigned(const vector<unique_ptr<Stmt>>& block, set<string>& out);

static void collectAssignedStmt(const Stmt* s, set<string>& out) {
    if (s->kind == StmtKind::ASSIGN || s->kind == StmtKind::INPUT) out.insert(s->name);
    if (s->kind == StmtKind::IF) {
        for (auto& b : s->branches) collectAssigned(b.body, out);
        collectAssigned(s->elseBody, out);
    }
    if (s->kind == StmtKind::WHILE) collectAssigned(s->body, out);
}

static void collectAssigned(const vector<unique_ptr<Stmt>>& block, set<string>& out) {
    for (auto& s : block) collectAssignedStmt(s.get(), out);
}

static vector<string> functionLocals(const Stmt* fn) {
    vector<string> ordered(fn->params);          // params first: they map to arg registers
    set<string> seen(fn->params.begin(), fn->params.end());
    set<string> assigned;
    collectAssigned(fn->body, assigned);
    for (const string& n : assigned)
        if (!seen.count(n)) { seen.insert(n); ordered.push_back(n); }
    return ordered;
}

// ===================== Parser =====================

struct Parser {
    const vector<Token>& toks;
    size_t pos = 0;
    Parser(const vector<Token>& t) : toks(t) {}

    const Token& cur() const { return toks[pos]; }
    bool isIdent(const string& s) const { return cur().type == Tok::IDENT && cur().text == s; }
    void advance() { if (toks[pos].type != Tok::END) pos++; }

    void skipNewlines() { while (cur().type == Tok::NEWLINE) advance(); }

    unique_ptr<Expr> parsePrimary() {
        auto e = make_unique<Expr>();
        if (cur().type == Tok::NUMBER) {
            e->kind = ExprKind::NUMBER; e->num = cur().num; advance();
        } else if (cur().type == Tok::STRING) {
            e->kind = ExprKind::STRING; e->str = cur().text; advance();
        } else if (cur().type == Tok::LPAREN) {
            advance();
            e = parseExpr();
            if (cur().type == Tok::RPAREN) advance();
        } else if (cur().type == Tok::LBRACKET) {   // array literal: [expr, expr, ...]
            advance();
            e->kind = ExprKind::ARRAY;
            skipNewlines();
            if (cur().type != Tok::RBRACKET) {
                for (;;) {
                    e->args.push_back(parseExpr());
                    if (cur().type == Tok::COMMA) { advance(); skipNewlines(); continue; }
                    break;
                }
            }
            skipNewlines();
            if (cur().type == Tok::RBRACKET) advance();
        } else if (cur().type == Tok::IDENT) {
            string name = cur().text;
            advance();
            if (cur().type == Tok::LPAREN) {      // call
                advance();
                e->kind = ExprKind::CALL;
                e->str = name;
                skipNewlines();
                if (cur().type != Tok::RPAREN) {
                    for (;;) {
                        e->args.push_back(parseExpr());
                        if (cur().type == Tok::COMMA) { advance(); skipNewlines(); continue; }
                        break;
                    }
                }
                if (cur().type == Tok::RPAREN) advance();
            } else {
                e->kind = ExprKind::IDENT;
                e->str = name;
            }
        } else {
            // Nothing recognizable; produce an empty string literal so
            // callers still get a well-formed (if useless) Expr node.
            e->kind = ExprKind::STRING; e->str = "";
        }
        // Single-level indexing: `expr[index]`. Not chained (no `a[i][j]` in
        // one go) -- index into a nested array with a temporary instead. That
        // keeps the assignment side (which only ever targets `IDENT[index]`)
        // symmetric with what reads can do.
        if (cur().type == Tok::LBRACKET) {
            advance();
            auto idx = parseExpr();
            if (cur().type == Tok::RBRACKET) advance();
            auto ie = make_unique<Expr>();
            ie->kind = ExprKind::INDEX;
            ie->left = std::move(e);
            ie->right = std::move(idx);
            e = std::move(ie);
        }
        return e;
    }

    unique_ptr<Expr> parseUnary() {
        if (cur().type == Tok::MINUS) {
            advance();
            auto e = make_unique<Expr>();
            e->kind = ExprKind::UNARY;
            e->str = "-";
            e->left = parseUnary();
            return e;
        }
        return parsePrimary();
    }

    static unique_ptr<Expr> makeBinop(const string& op, unique_ptr<Expr> l, unique_ptr<Expr> r) {
        auto bin = make_unique<Expr>();
        bin->kind = ExprKind::BINOP;
        bin->str = op;
        bin->left = std::move(l);
        bin->right = std::move(r);
        return bin;
    }

    unique_ptr<Expr> parseMultiplicative() {
        auto left = parseUnary();
        for (;;) {
            string op;
            switch (cur().type) {
                case Tok::STAR:    op = "*"; break;
                case Tok::SLASH:   op = "/"; break;
                case Tok::PERCENT: op = "%"; break;
                default: return left;
            }
            advance();
            left = makeBinop(op, std::move(left), parseUnary());
        }
    }

    unique_ptr<Expr> parseAdditive() {
        auto left = parseMultiplicative();
        for (;;) {
            string op;
            switch (cur().type) {
                case Tok::PLUS:  op = "+"; break;
                case Tok::MINUS: op = "-"; break;
                default: return left;
            }
            advance();
            left = makeBinop(op, std::move(left), parseMultiplicative());
        }
    }

    unique_ptr<Expr> parseComparison() {
        auto left = parseAdditive();
        for (;;) {
            string op;
            switch (cur().type) {
                case Tok::EQEQ: op = "=="; break;
                case Tok::NEQ:  op = "!="; break;
                case Tok::GE:   op = ">="; break;
                case Tok::LE:   op = "<="; break;
                case Tok::GT:   op = ">"; break;
                case Tok::LT:   op = "<"; break;
                default: return left;
            }
            advance();
            left = makeBinop(op, std::move(left), parseAdditive());
        }
    }

    // 'and' binds tighter than 'or', both looser than comparison -- matches
    // the usual boolean-operator precedence so `a == b or c and d` parses as
    // `(a == b) or (c and d)` without needing parentheses.
    unique_ptr<Expr> parseAnd() {
        auto left = parseComparison();
        while (isIdent("and")) {
            advance();
            left = makeBinop("and", std::move(left), parseComparison());
        }
        return left;
    }

    unique_ptr<Expr> parseExpr() {
        auto left = parseAnd();
        while (isIdent("or")) {
            advance();
            left = makeBinop("or", std::move(left), parseAnd());
        }
        return left;
    }

    // Skip an unrecognized statement's tokens (legacy no-op compatibility).
    void skipStatement() {
        while (cur().type != Tok::NEWLINE && cur().type != Tok::RBRACE && cur().type != Tok::END) advance();
    }

    void parseBlockInto(vector<unique_ptr<Stmt>>& out) {
        skipNewlines();
        if (cur().type == Tok::LBRACE) advance();
        skipNewlines();
        while (cur().type != Tok::RBRACE && cur().type != Tok::END) {
            auto st = parseStatement();
            if (st) out.push_back(std::move(st));
            skipNewlines();
        }
        if (cur().type == Tok::RBRACE) advance();
    }

    unique_ptr<Stmt> parseIf() {
        advance(); // 'if'
        auto s = make_unique<Stmt>();
        s->kind = StmtKind::IF;
        auto parseOneBranch = [&]() -> IfBranch {
            IfBranch b;
            if (cur().type == Tok::LPAREN) advance();
            b.cond = parseExpr();
            if (cur().type == Tok::RPAREN) advance();
            parseBlockInto(b.body);
            return b;
        };
        s->branches.push_back(parseOneBranch());

        for (;;) {
            size_t save = pos;
            skipNewlines();
            if (isIdent("else")) {
                advance();
                if (isIdent("if")) {
                    advance();
                    s->branches.push_back(parseOneBranch());
                    continue;
                } else {
                    parseBlockInto(s->elseBody);
                    s->hasElse = true;
                    break;
                }
            } else {
                pos = save; // no else/else-if follows; don't consume the newlines
                break;
            }
        }
        return s;
    }

    unique_ptr<Stmt> parseWhile() {
        advance(); // 'while'
        auto s = make_unique<Stmt>();
        s->kind = StmtKind::WHILE;
        if (cur().type == Tok::LPAREN) advance();
        s->cond = parseExpr();
        if (cur().type == Tok::RPAREN) advance();
        parseBlockInto(s->body);
        return s;
    }

    unique_ptr<Stmt> parseFunc() {
        advance(); // 'func'
        auto s = make_unique<Stmt>();
        s->kind = StmtKind::FUNC;
        if (cur().type == Tok::IDENT) { s->name = cur().text; advance(); }
        if (cur().type == Tok::LPAREN) {
            advance();
            while (cur().type != Tok::RPAREN && cur().type != Tok::END) {
                if (cur().type == Tok::IDENT) s->params.push_back(cur().text);
                advance();
                if (cur().type == Tok::COMMA) advance();
            }
            if (cur().type == Tok::RPAREN) advance();
        }
        parseBlockInto(s->body);
        return s;
    }

    unique_ptr<Stmt> parseStatement() {
        skipNewlines();
        if (cur().type == Tok::END || cur().type == Tok::RBRACE) return nullptr;

        if (isIdent("if")) return parseIf();
        if (isIdent("while")) return parseWhile();
        if (isIdent("func")) return parseFunc();

        if (isIdent("break") || isIdent("continue")) {
            auto s = make_unique<Stmt>();
            s->kind = isIdent("break") ? StmtKind::BREAK : StmtKind::CONTINUE;
            advance();
            return s;
        }

        if (isIdent("return")) {
            advance();
            auto s = make_unique<Stmt>();
            s->kind = StmtKind::RETURN;
            if (cur().type != Tok::NEWLINE && cur().type != Tok::RBRACE && cur().type != Tok::END)
                s->expr = parseExpr();
            return s;
        }

        if (isIdent("prt")) {
            advance();
            auto s = make_unique<Stmt>();
            s->kind = StmtKind::PRT;
            s->expr = parseExpr();
            return s;
        }

        if (isIdent("input")) {
            advance();
            auto s = make_unique<Stmt>();
            s->kind = StmtKind::INPUT;
            if (cur().type == Tok::IDENT) { s->name = cur().text; advance(); }
            return s;
        }

        if (cur().type == Tok::IDENT && toks[pos + 1].type == Tok::LBRACKET) {
            size_t save = pos;
            string base = cur().text;
            advance(); // ident
            advance(); // '['
            auto idxExpr = parseExpr();
            if (cur().type == Tok::RBRACKET) advance();
            if (cur().type == Tok::EQ) {
                advance();
                auto s = make_unique<Stmt>();
                s->kind = StmtKind::INDEXSET;
                auto tgt = make_unique<Expr>();
                tgt->kind = ExprKind::IDENT;
                tgt->str = base;
                s->indexTarget = std::move(tgt);
                s->indexExpr = std::move(idxExpr);
                s->expr = parseExpr();
                return s;
            }
            // Not an assignment (e.g. a bare `arr[i]` statement, which has no
            // effect) -- rewind and let the generic fallback skip it.
            pos = save;
        }

        if (cur().type == Tok::IDENT && toks[pos + 1].type == Tok::EQ) {
            auto s = make_unique<Stmt>();
            s->kind = StmtKind::ASSIGN;
            s->name = cur().text;
            advance(); // ident
            advance(); // '='
            s->expr = parseExpr();
            return s;
        }

        // A bare call used as a statement: `greet("hi")`.
        if (cur().type == Tok::IDENT && toks[pos + 1].type == Tok::LPAREN) {
            auto s = make_unique<Stmt>();
            s->kind = StmtKind::CALLSTMT;
            s->expr = parseExpr();
            return s;
        }

        // Unrecognized statement shape: skip it silently (legacy behavior).
        skipStatement();
        return nullptr;
    }

    Program parseProgram() {
        Program prog;
        for (;;) {
            skipNewlines();
            if (cur().type == Tok::END) break;
            auto s = parseStatement();
            if (s) prog.push_back(std::move(s));
        }
        return prog;
    }
};

// ===================== Interpreter =====================

enum class ValueType { STRING, NUMBER, BOOL, EMPTY, ERROR, ARRAY };

struct Value {
    ValueType type = ValueType::EMPTY;
    string s_val;
    double n_val = 0;
    bool b_val = false;
    // Arrays are heap objects with reference semantics: copying a Value
    // (assignment, passing as a call argument, storing an element) copies
    // this shared_ptr, not the vector, so `b = a` aliases the same backing
    // storage the same way the compiled backend's Value* pointer does.
    shared_ptr<vector<Value>> arr_val;

    string to_string() const {
        switch (type) {
            case ValueType::STRING: return s_val;
            case ValueType::NUMBER: return std::to_string(n_val);
            case ValueType::BOOL: return b_val ? "true" : "false";
            case ValueType::ERROR: return "ERROR: " + s_val;
            case ValueType::ARRAY: {
                string out = "[";
                for (size_t i = 0; i < arr_val->size(); i++) {
                    if (i) out += ", ";
                    out += (*arr_val)[i].to_string();
                }
                out += "]";
                return out;
            }
            default: return "EMPTY";
        }
    }
};

// Arithmetic op codes, shared with the runtime's rt_arith (see runtime.c).
static int arithCode(const string& op) {
    if (op == "+") return 0;
    if (op == "-") return 1;
    if (op == "*") return 2;
    if (op == "/") return 3;
    return 4; // "%"
}
static bool isArith(const string& op) {
    return op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
}

// Non-local exits out of a block, checked by the statement loop.
enum class Flow { NORMAL, BREAK, CONTINUE, RETURN };

struct Interpreter {
    map<string, Value> globals;
    map<string, const Stmt*> functions;
    vector<map<string, Value>*> frames;   // innermost function frame, if any
    map<string, set<string>> localNames;  // function name -> its local/param names
    Value returnValue;

    const set<string>* currentLocals = nullptr;

    Value* lookup(const string& name) {
        if (!frames.empty() && currentLocals && currentLocals->count(name)) {
            auto& f = *frames.back();
            auto it = f.find(name);
            return it == f.end() ? nullptr : &it->second;
        }
        auto it = globals.find(name);
        return it == globals.end() ? nullptr : &it->second;
    }

    void store(const string& name, const Value& v) {
        if (!frames.empty() && currentLocals && currentLocals->count(name)) (*frames.back())[name] = v;
        else globals[name] = v;
    }

    static Value makeErr(const string& msg) {
        Value v; v.type = ValueType::ERROR; v.s_val = msg; return v;
    }

    // Mirrors rt_array_get in runtime.c exactly, including check order:
    // propagate errors first, then type-check the base and the index, then
    // bounds-check. See the semantics note above runtime.c's array section.
    static Value arrayIndex(const Value& base, const Value& idx) {
        if (base.type == ValueType::ERROR) return base;
        if (idx.type == ValueType::ERROR) return idx;
        if (base.type != ValueType::ARRAY) return makeErr("not an array");
        if (idx.type != ValueType::NUMBER) return makeErr("non-numeric index");
        long i = (long)idx.n_val;
        if (i < 0 || i >= (long)base.arr_val->size()) return makeErr("index out of bounds");
        return (*base.arr_val)[i];
    }

    Value evalArith(const string& op, const Value& l, const Value& r) {
        if (l.type == ValueType::ERROR) return l;
        if (r.type == ValueType::ERROR) return r;
        Value out;
        bool bothNum = l.type == ValueType::NUMBER && r.type == ValueType::NUMBER;
        if (op == "+" && !bothNum) {   // string concatenation
            out.type = ValueType::STRING;
            out.s_val = l.to_string() + r.to_string();
            return out;
        }
        if (!bothNum) return makeErr("non-numeric operand");
        out.type = ValueType::NUMBER;
        if (op == "+") out.n_val = l.n_val + r.n_val;
        else if (op == "-") out.n_val = l.n_val - r.n_val;
        else if (op == "*") out.n_val = l.n_val * r.n_val;
        else {
            if (r.n_val == 0) return makeErr("division by zero");
            out.n_val = (op == "/") ? l.n_val / r.n_val : fmod(l.n_val, r.n_val);
        }
        return out;
    }

    Value callFunction(const string& name, vector<Value>& args) {
        auto it = functions.find(name);
        if (it == functions.end()) return makeErr("Unknown function: '" + name + "'");
        const Stmt* fn = it->second;

        map<string, Value> frame;
        for (size_t i = 0; i < fn->params.size(); i++)
            frame[fn->params[i]] = (i < args.size()) ? args[i] : Value{};

        const set<string>* savedLocals = currentLocals;
        currentLocals = &localNames[name];
        frames.push_back(&frame);

        Value saveRet = returnValue;
        returnValue = Value{};
        Flow f = execBlock(fn->body);
        Value result = (f == Flow::RETURN) ? returnValue : Value{};
        returnValue = saveRet;

        frames.pop_back();
        currentLocals = savedLocals;
        return result;
    }

    Value evalExpr(const Expr* e) {
        Value v;
        switch (e->kind) {
            case ExprKind::NUMBER: v.type = ValueType::NUMBER; v.n_val = e->num; return v;
            case ExprKind::STRING: v.type = ValueType::STRING; v.s_val = e->str; return v;
            case ExprKind::IDENT: {
                Value* found = lookup(e->str);
                if (found) return *found;
                return makeErr("Unknown identifier: '" + e->str + "'");
            }
            case ExprKind::UNARY: {
                Value operand = evalExpr(e->left.get());
                if (operand.type == ValueType::ERROR) return operand;
                if (operand.type != ValueType::NUMBER) return makeErr("non-numeric operand");
                v.type = ValueType::NUMBER; v.n_val = -operand.n_val;
                return v;
            }
            case ExprKind::CALL: {
                // `len` is a reserved builtin, not a user function: it is
                // intercepted here before any lookup in `functions`, so a
                // program cannot define its own `len`.
                if (e->str == "len" && e->args.size() == 1) {
                    Value arg = evalExpr(e->args[0].get());
                    if (arg.type == ValueType::ERROR) return arg;
                    if (arg.type != ValueType::ARRAY) return makeErr("not an array");
                    Value result; result.type = ValueType::NUMBER;
                    result.n_val = (double)arg.arr_val->size();
                    return result;
                }
                vector<Value> args;
                for (auto& a : e->args) args.push_back(evalExpr(a.get()));
                return callFunction(e->str, args);
            }
            case ExprKind::ARRAY: {
                v.type = ValueType::ARRAY;
                v.arr_val = make_shared<vector<Value>>();
                for (auto& a : e->args) v.arr_val->push_back(evalExpr(a.get()));
                return v;
            }
            case ExprKind::INDEX:
                return arrayIndex(evalExpr(e->left.get()), evalExpr(e->right.get()));
            case ExprKind::BINOP: {
                const string& op = e->str;
                // Short-circuit: the right operand must not be evaluated at
                // all when the left already decides the result, matching
                // what the codegen's branches do.
                if (op == "and" || op == "or") {
                    Value left = evalExpr(e->left.get());
                    bool lt = truthy(left);
                    Value result;
                    result.type = ValueType::BOOL;
                    if (op == "and" ? !lt : lt) { result.b_val = lt; return result; }
                    result.b_val = truthy(evalExpr(e->right.get()));
                    return result;
                }
                Value left = evalExpr(e->left.get());
                Value right = evalExpr(e->right.get());
                if (isArith(op)) return evalArith(op, left, right);
                Value result;
                result.type = ValueType::BOOL;
                if (op == "==") {
                    if (left.type == ValueType::NUMBER && right.type == ValueType::NUMBER) result.b_val = left.n_val == right.n_val;
                    else result.b_val = left.to_string() == right.to_string();
                } else if (op == "!=") {
                    if (left.type == ValueType::NUMBER && right.type == ValueType::NUMBER) result.b_val = left.n_val != right.n_val;
                    else result.b_val = left.to_string() != right.to_string();
                } else if (op == ">") result.b_val = left.n_val > right.n_val;
                else if (op == "<") result.b_val = left.n_val < right.n_val;
                else if (op == ">=") result.b_val = left.n_val >= right.n_val;
                else if (op == "<=") result.b_val = left.n_val <= right.n_val;
                return result;
            }
        }
        return v;
    }

    // Truthiness must match the runtime's rt_truthy exactly, or the two
    // backends will disagree on branch and loop conditions.
    static bool truthy(const Value& v) {
        switch (v.type) {
            case ValueType::BOOL:   return v.b_val;
            case ValueType::NUMBER: return v.n_val != 0;
            default: return false;
        }
    }

    Flow execBlock(const vector<unique_ptr<Stmt>>& block) {
        for (auto& s : block) {
            Flow f = execStmt(s.get());
            if (f != Flow::NORMAL) return f;
        }
        return Flow::NORMAL;
    }

    Flow execStmt(const Stmt* s) {
        switch (s->kind) {
            case StmtKind::FUNC:
                return Flow::NORMAL; // hoisted before the run
            case StmtKind::PRT:
                cout << evalExpr(s->expr.get()).to_string() << "\n";
                return Flow::NORMAL;
            case StmtKind::INPUT: {
                string line;
                getline(cin, line);
                Value v; v.type = ValueType::STRING; v.s_val = line;
                store(s->name, v);
                return Flow::NORMAL;
            }
            case StmtKind::ASSIGN:
                store(s->name, evalExpr(s->expr.get()));
                return Flow::NORMAL;
            case StmtKind::CALLSTMT:
                evalExpr(s->expr.get());
                return Flow::NORMAL;
            case StmtKind::RETURN:
                returnValue = s->expr ? evalExpr(s->expr.get()) : Value{};
                return Flow::RETURN;
            case StmtKind::BREAK:    return Flow::BREAK;
            case StmtKind::CONTINUE: return Flow::CONTINUE;
            case StmtKind::INDEXSET: {
                Value* target = lookup(s->indexTarget->str);
                Value idxv = evalExpr(s->indexExpr.get());
                Value val = evalExpr(s->expr.get());
                if (!target || target->type != ValueType::ARRAY) return Flow::NORMAL;
                if (idxv.type != ValueType::NUMBER) return Flow::NORMAL;
                long i = (long)idxv.n_val;
                if (i < 0 || i >= (long)target->arr_val->size()) return Flow::NORMAL;
                (*target->arr_val)[i] = val;
                return Flow::NORMAL;
            }
            case StmtKind::WHILE: {
                while (truthy(evalExpr(s->cond.get()))) {
                    Flow f = execBlock(s->body);
                    if (f == Flow::BREAK) break;
                    if (f == Flow::RETURN) return f;
                }
                return Flow::NORMAL;
            }
            case StmtKind::IF: {
                for (auto& branch : s->branches) {
                    if (truthy(evalExpr(branch.cond.get())))
                        return execBlock(branch.body);
                }
                if (s->hasElse) return execBlock(s->elseBody);
                return Flow::NORMAL;
            }
        }
        return Flow::NORMAL;
    }

    void run(const Program& prog) {
        for (auto& s : prog)                       // hoist declarations first, so
            if (s->kind == StmtKind::FUNC) {       // calls may precede definitions
                functions[s->name] = s.get();
                set<string> locals;
                for (const string& l : functionLocals(s.get())) locals.insert(l);
                localNames[s->name] = locals;
            }
        execBlock(prog);
    }
};

// ===================== x86-64 code generator =====================
// Emits GAS (AT&T) assembly. Values are heap-allocated Value* handles built
// and inspected through a small C runtime (runtime.c) -- the same way a real
// compiler leans on libc rather than hand-rolling printf/string-compare in
// assembly. The codegen itself does the real work: the System V AMD64 calling
// convention, stack frames with slot-allocated locals, label-based control
// flow for if/while/break/continue, and a callee-saved register pool for
// expression temporaries (with stack-spill fallback when it runs dry).

// Callee-saved, so a temp held here survives the rt_* and user-function calls
// that a surrounding expression makes. That is the whole reason for the choice:
// caller-saved registers would have to be spilled around every call anyway.
static const char* kTempRegs[] = { "%rbx", "%r12", "%r13", "%r14", "%r15" };
static const int kNumTempRegs = 5;

// System V AMD64 integer argument registers, in order.
static const char* kArgRegs[] = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9" };
static const int kMaxArgs = 6;

struct LoopLabels { string breakLabel, continueLabel; };

struct CodeGen {
    ostringstream rodata, bss, funcs;
    ostringstream* text;                 // current output stream
    map<string, bool> declaredVars;
    map<string, int> localSlots;         // active function's name -> frame offset
    set<string> knownFuncs;
    vector<LoopLabels> loopStack;
    string returnLabel;
    bool regallocEnabled = true;

    bool tempInUse[kNumTempRegs] = {false, false, false, false, false};
    bool tempEverUsed[kNumTempRegs] = {false, false, false, false, false};
    int labelCounter = 0;
    int litCounter = 0;

    CodeGen() : text(&mainText) {}
    ostringstream mainText;

    string newLabel(const string& base) { return "." + base + std::to_string(labelCounter++); }

    void declareVar(const string& name) {
        if (declaredVars.count(name)) return;
        declaredVars[name] = true;
        bss << "var_" << name << ": .quad 0\n";
    }

    // Collect every global variable name touched (assign or input) so .bss slots
    // exist before codegen references them. Function bodies are skipped: their
    // names live in stack frames, not .bss.
    void collectGlobals(const vector<unique_ptr<Stmt>>& block) {
        for (auto& s : block) {
            if (s->kind == StmtKind::FUNC) continue;
            if (s->kind == StmtKind::ASSIGN || s->kind == StmtKind::INPUT) declareVar(s->name);
            if (s->kind == StmtKind::IF) {
                for (auto& b : s->branches) collectGlobals(b.body);
                collectGlobals(s->elseBody);
            }
            if (s->kind == StmtKind::WHILE) collectGlobals(s->body);
        }
    }

    static int opCode(const string& op) {
        if (op == "==") return 0;
        if (op == "!=") return 1;
        if (op == ">") return 2;
        if (op == "<") return 3;
        if (op == ">=") return 4;
        return 5; // "<="
    }

    // --- expression temporaries -------------------------------------------
    // Allocation follows the evaluation stack's shape: a temp is claimed while
    // the left operand's value waits for the right operand to be computed, and
    // released the moment it is consumed. Nesting deeper than the pool falls
    // back to the stack, so correctness never depends on pool size.
    int allocTemp() {
        if (!regallocEnabled) return -1;
        for (int i = 0; i < kNumTempRegs; i++)
            if (!tempInUse[i]) { tempInUse[i] = true; tempEverUsed[i] = true; return i; }
        return -1; // pool exhausted: caller spills
    }
    void freeTemp(int i) { if (i >= 0) tempInUse[i] = false; }

    // Evaluate `e` into %rax, then park it where it survives the next
    // subexpression. Returns the temp index, or -1 if it was spilled.
    int genAndHold(const Expr* e) {
        genExpr(e);
        int t = allocTemp();
        if (t >= 0) *text << "    mov %rax, " << kTempRegs[t] << "\n";
        else { *text << "    sub $16, %rsp\n" << "    mov %rax, (%rsp)\n"; }
        return t;
    }
    // Move a held value into `dest` and release its storage.
    void releaseInto(int t, const char* dest) {
        if (t >= 0) { *text << "    mov " << kTempRegs[t] << ", " << dest << "\n"; freeTemp(t); }
        else { *text << "    mov (%rsp), " << dest << "\n" << "    add $16, %rsp\n"; }
    }
    // Like releaseInto, but keeps the value held -- for array-literal
    // construction, which needs the same array pointer across every element
    // store, not a one-shot consume.
    void peekInto(int t, const char* dest) {
        if (t >= 0) *text << "    mov " << kTempRegs[t] << ", " << dest << "\n";
        else *text << "    mov (%rsp), " << dest << "\n";
    }

    // Emits code that leaves a Value* result in %rax.
    void genExpr(const Expr* e) {
        switch (e->kind) {
            case ExprKind::NUMBER: {
                string lbl = ".LCnum" + std::to_string(litCounter++);
                rodata << lbl << ": .double " << e->num << "\n";
                *text << "    movsd " << lbl << "(%rip), %xmm0\n";
                *text << "    call rt_make_num\n";
                return;
            }
            case ExprKind::STRING: {
                string lbl = ".LCstr" + std::to_string(litCounter++);
                rodata << lbl << ": .string \"" << escapeAsm(e->str) << "\"\n";
                *text << "    lea " << lbl << "(%rip), %rdi\n";
                *text << "    call rt_make_str\n";
                return;
            }
            case ExprKind::IDENT: {
                auto it = localSlots.find(e->str);
                if (it != localSlots.end()) {
                    *text << "    mov " << it->second << "(%rbp), %rax\n";
                } else {
                    declareVar(e->str);
                    *text << "    mov var_" << e->str << "(%rip), %rax\n";
                }
                // An unassigned slot is a null Value*. The interpreter reports
                // that as an error and carries on, so the compiled path has to
                // do the same. Letting the null reach rt_arith instead makes
                // '+' stringify it, so "prt undefined + 1" printed "EMPTY1.000000"
                // where the interpreter printed an error. Both backends agreeing
                // is the whole point of run_tests.sh, and no .lang file in the
                // corpus happened to read an unassigned name, so the differential
                // suite never caught it.
                string lbl = ".LCund" + std::to_string(litCounter);
                string ok = ".Ldef" + std::to_string(litCounter++);
                rodata << lbl << ": .string \"" << escapeAsm(e->str) << "\"\n";
                *text << "    test %rax, %rax\n";
                *text << "    jne " << ok << "\n";
                *text << "    lea " << lbl << "(%rip), %rdi\n";
                *text << "    call rt_undef\n";
                *text << ok << ":\n";
                return;
            }
            case ExprKind::UNARY: {
                genExpr(e->left.get());
                *text << "    mov %rax, %rdi\n";
                *text << "    call rt_neg\n";
                return;
            }
            case ExprKind::CALL: {
                genCall(e);
                return;
            }
            case ExprKind::BINOP: {
                if (e->str == "and" || e->str == "or") {
                    genShortCircuit(e);
                    return;
                }
                int t = genAndHold(e->left.get());
                genExpr(e->right.get());
                *text << "    mov %rax, %rdx\n";     // right -> arg3
                releaseInto(t, "%rsi");              // left  -> arg2
                if (isArith(e->str)) {
                    *text << "    mov $" << arithCode(e->str) << ", %edi\n";
                    *text << "    call rt_arith\n";
                } else {
                    *text << "    mov $" << opCode(e->str) << ", %edi\n";
                    *text << "    call rt_cmp\n";
                }
                return;
            }
            case ExprKind::ARRAY: {
                // Allocate all n slots up front, then fill them one at a time.
                // The array pointer is held across every element's evaluation
                // (which may itself spill/call), same discipline as any other
                // held temporary.
                size_t n = e->args.size();
                *text << "    mov $" << n << ", %edi\n";
                *text << "    call rt_array_new\n";
                int t = allocTemp();
                if (t >= 0) *text << "    mov %rax, " << kTempRegs[t] << "\n";
                else { *text << "    sub $16, %rsp\n" << "    mov %rax, (%rsp)\n"; }
                for (size_t i = 0; i < n; i++) {
                    genExpr(e->args[i].get());
                    *text << "    mov %rax, %rdx\n";        // element value -> arg3
                    peekInto(t, "%rdi");                     // array        -> arg1
                    *text << "    mov $" << i << ", %esi\n"; // index        -> arg2
                    *text << "    call rt_array_set_elem\n";
                }
                releaseInto(t, "%rax");
                return;
            }
            case ExprKind::INDEX: {
                int t = genAndHold(e->left.get());   // array
                genExpr(e->right.get());              // index -> %rax
                *text << "    mov %rax, %rsi\n";
                releaseInto(t, "%rdi");
                *text << "    call rt_array_get\n";
                return;
            }
        }
    }

    // 'and'/'or' must not evaluate their right operand when the left already
    // decides the result -- that's the entire point of the feature, not just
    // an optimization, so this is branches rather than a runtime call.
    // Leaves a Value* (built via rt_make_bool) in %rax, like any other genExpr.
    void genShortCircuit(const Expr* e) {
        bool isAnd = e->str == "and";
        string trueLbl = newLabel("Lbtrue");
        string falseLbl = newLabel("Lbfalse");
        string done = newLabel("Lbend");
        genCondition(e->left.get());                // test %eax, %eax <- truthy(left)
        // 'and' with a falsy left, or 'or' with a truthy left, already knows
        // the answer and must not touch the right operand at all.
        *text << "    " << (isAnd ? "je" : "jne") << " " << (isAnd ? falseLbl : trueLbl) << "\n";
        genCondition(e->right.get());
        *text << "    je " << falseLbl << "\n";
        *text << "    jmp " << trueLbl << "\n";
        *text << falseLbl << ":\n";
        *text << "    mov $0, %edi\n    call rt_make_bool\n";
        *text << "    jmp " << done << "\n";
        *text << trueLbl << ":\n";
        *text << "    mov $1, %edi\n    call rt_make_bool\n";
        *text << done << ":\n";
    }

    void genCall(const Expr* e) {
        // `len` is a reserved builtin, intercepted before the user-function
        // checks below -- a program cannot declare its own `len`.
        if (e->str == "len" && e->args.size() == 1) {
            genExpr(e->args[0].get());
            *text << "    mov %rax, %rdi\n";
            *text << "    call rt_array_len\n";
            return;
        }
        size_t n = e->args.size();
        if (n > (size_t)kMaxArgs) {
            cerr << "Error: '" << e->str << "' called with " << n
                 << " arguments; the codegen supports at most " << kMaxArgs
                 << " (System V register arguments)." << endl;
            exit(1);
        }
        if (!knownFuncs.count(e->str)) {
            cerr << "Error: call to undefined function '" << e->str << "'." << endl;
            exit(1);
        }
        // Evaluate arguments left to right, each parked in a temp, then load the
        // argument registers. Loading them as we go would not survive the next
        // argument's own rt_* calls, which clobber the caller-saved arg registers.
        vector<int> held;
        for (auto& a : e->args) held.push_back(genAndHold(a.get()));
        for (size_t i = n; i-- > 0; ) releaseInto(held[i], kArgRegs[i]);
        *text << "    call fn_" << e->str << "\n";
    }

    static string escapeAsm(const string& s) {
        string out;
        for (char c : s) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out;
    }

    void genBlock(const vector<unique_ptr<Stmt>>& block) {
        for (auto& s : block) genStmt(s.get());
    }

    void storeToVar(const string& name) {
        auto it = localSlots.find(name);
        if (it != localSlots.end()) *text << "    mov %rax, " << it->second << "(%rbp)\n";
        else *text << "    mov %rax, var_" << name << "(%rip)\n";
    }

    // Leaves the condition's truth value in the flags via `test %eax, %eax`.
    void genCondition(const Expr* cond) {
        genExpr(cond);
        *text << "    mov %rax, %rdi\n";
        *text << "    call rt_truthy\n";
        *text << "    test %eax, %eax\n";
    }

    void genStmt(const Stmt* s) {
        switch (s->kind) {
            case StmtKind::FUNC: return; // emitted separately
            case StmtKind::PRT:
                genExpr(s->expr.get());
                *text << "    mov %rax, %rdi\n";
                *text << "    call rt_print\n";
                return;
            case StmtKind::INPUT:
                *text << "    call rt_input\n";
                storeToVar(s->name);
                return;
            case StmtKind::ASSIGN:
                genExpr(s->expr.get());
                storeToVar(s->name);
                return;
            case StmtKind::CALLSTMT:
                genExpr(s->expr.get());
                return;
            case StmtKind::INDEXSET: {
                int t1 = genAndHold(s->indexTarget.get());  // array
                int t2 = genAndHold(s->indexExpr.get());    // index
                genExpr(s->expr.get());                      // value -> %rax
                *text << "    mov %rax, %rdx\n";
                releaseInto(t2, "%rsi");
                releaseInto(t1, "%rdi");
                *text << "    call rt_array_set\n";
                return;
            }
            case StmtKind::RETURN:
                if (s->expr) genExpr(s->expr.get());
                else *text << "    xor %eax, %eax\n";
                *text << "    jmp " << returnLabel << "\n";
                return;
            case StmtKind::BREAK:
                if (!loopStack.empty()) *text << "    jmp " << loopStack.back().breakLabel << "\n";
                return;
            case StmtKind::CONTINUE:
                if (!loopStack.empty()) *text << "    jmp " << loopStack.back().continueLabel << "\n";
                return;
            case StmtKind::WHILE: {
                string top = newLabel("Lwhile");
                string end = newLabel("Lwend");
                loopStack.push_back({end, top});
                *text << top << ":\n";
                genCondition(s->cond.get());
                *text << "    je " << end << "\n";
                genBlock(s->body);
                *text << "    jmp " << top << "\n";
                *text << end << ":\n";
                loopStack.pop_back();
                return;
            }
            case StmtKind::IF: {
                string end = newLabel("Lend");
                for (size_t i = 0; i < s->branches.size(); i++) {
                    string next = (i + 1 < s->branches.size() || s->hasElse) ? newLabel("Lnext") : end;
                    genCondition(s->branches[i].cond.get());
                    *text << "    je " << next << "\n";
                    genBlock(s->branches[i].body);
                    *text << "    jmp " << end << "\n";
                    if (next != end) *text << next << ":\n";
                }
                if (s->hasElse) genBlock(s->elseBody);
                *text << end << ":\n";
                return;
            }
        }
    }

    // Wraps a body in a System V-conformant frame. `bodyStream` has already been
    // generated, so the exact set of callee-saved registers it touched is known
    // and only those get saved.
    string frameWrap(const string& label, const string& body, int numLocals,
                     const vector<string>& params, const string& retLabel,
                     const bool usedRegs[kNumTempRegs], bool isMain) {
        vector<int> saved;
        for (int i = 0; i < kNumTempRegs; i++) if (usedRegs[i]) saved.push_back(i);

        // Frame layout, top to bottom:
        //   saved %rbp            <- %rbp points here
        //   locals                   -8(%rbp) .. -8*numLocals(%rbp)
        //   saved callee-saved regs
        // The callee-saved registers must go *below* the locals. Pushing them
        // first would put them at -8(%rbp) and onward, which is exactly where
        // the local slots live -- the two would silently alias.
        //
        // Alignment: the `call` that got us here pushed a return address, so at
        // entry %rsp is 8 mod 16, and `push %rbp` brings it to 0. Whatever we
        // subtract and push after that must therefore be a multiple of 16, or
        // the SSE moves inside libc fault on the misaligned stack.
        int frame = numLocals * 8;
        frame += (16 - (frame + 8 * (int)saved.size()) % 16) % 16;

        ostringstream out;
        out << label << ":\n";
        out << "    push %rbp\n";
        out << "    mov %rsp, %rbp\n";
        if (frame) out << "    sub $" << frame << ", %rsp\n";
        for (int i : saved) out << "    push " << kTempRegs[i] << "\n";

        // Parameters arrive in registers; spill them into their frame slots.
        for (size_t i = 0; i < params.size() && i < (size_t)kMaxArgs; i++)
            out << "    mov " << kArgRegs[i] << ", " << -8 * (int)(i + 1) << "(%rbp)\n";
        // Non-parameter locals start as null; rt_* treats null as EMPTY, but
        // stack garbage would be dereferenced as a Value*.
        for (int i = (int)params.size(); i < numLocals; i++)
            out << "    movq $0, " << -8 * (i + 1) << "(%rbp)\n";

        out << body;
        if (isMain) out << "    mov $0, %eax\n";
        else out << "    xor %eax, %eax\n";   // fall off the end == return EMPTY
        out << retLabel << ":\n";
        for (size_t i = saved.size(); i-- > 0; ) out << "    pop " << kTempRegs[saved[i]] << "\n";
        out << "    mov %rbp, %rsp\n";   // discards the frame regardless of depth
        out << "    pop %rbp\n";
        out << "    ret\n";
        return out.str();
    }

    void genFunction(const Stmt* fn) {
        vector<string> locals = functionLocals(fn);
        localSlots.clear();
        for (size_t i = 0; i < locals.size(); i++) localSlots[locals[i]] = -8 * (int)(i + 1);

        ostringstream body;
        ostringstream* savedText = text;
        text = &body;
        bool savedEverUsed[kNumTempRegs];
        for (int i = 0; i < kNumTempRegs; i++) { savedEverUsed[i] = tempEverUsed[i]; tempEverUsed[i] = false; }
        string savedRet = returnLabel;
        returnLabel = ".Lret_" + fn->name;

        genBlock(fn->body);

        funcs << frameWrap("fn_" + fn->name, body.str(), (int)locals.size(),
                           fn->params, returnLabel, tempEverUsed, false);

        returnLabel = savedRet;
        for (int i = 0; i < kNumTempRegs; i++) tempEverUsed[i] = savedEverUsed[i];
        text = savedText;
        localSlots.clear();
    }

    string generate(const Program& prog) {
        for (auto& s : prog)
            if (s->kind == StmtKind::FUNC) knownFuncs.insert(s->name);
        for (auto& s : prog)
            if (s->kind == StmtKind::FUNC) genFunction(s.get());

        collectGlobals(prog);
        returnLabel = ".Lret_main";
        text = &mainText;
        genBlock(prog);
        string mainBody = frameWrap("main", mainText.str(), 0, {}, returnLabel, tempEverUsed, true);

        ostringstream out;
        out << "    .extern rt_make_num\n"
            << "    .extern rt_make_str\n"
            << "    .extern rt_print\n"
            << "    .extern rt_input\n"
            << "    .extern rt_cmp\n"
            << "    .extern rt_arith\n"
            << "    .extern rt_neg\n"
            << "    .extern rt_undef\n"
            << "    .extern rt_truthy\n"
            << "    .extern rt_make_bool\n"
            << "    .extern rt_array_new\n"
            << "    .extern rt_array_set_elem\n"
            << "    .extern rt_array_get\n"
            << "    .extern rt_array_set\n"
            << "    .extern rt_array_len\n"
            << "    .section .rodata\n"
            << rodata.str()
            << "    .section .bss\n"
            << bss.str()
            << "    .section .text\n"
            << funcs.str()
            << "    .globl main\n"
            << mainBody;
        return out.str();
    }
};

// ===================== main =====================

static string readFile(const string& path) {
    ifstream f(path);
    ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char* argv[]) {
    vector<string> args(argv + 1, argv + argc);
    bool compileMode = false;
    bool regalloc = true;
    string srcPath, outPath;

    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--compile") compileMode = true;
        else if (args[i] == "--interpret") compileMode = false;
        else if (args[i] == "--no-regalloc") regalloc = false;
        else if (args[i] == "-o" && i + 1 < args.size()) outPath = args[++i];
        else if (srcPath.empty()) srcPath = args[i];
    }

    if (srcPath.empty()) {
        cerr << "Usage: " << argv[0]
             << " [--interpret|--compile] [--no-regalloc] <source_file.lang> [-o output.s]" << endl;
        return 1;
    }

    ifstream check(srcPath);
    if (!check.is_open()) {
        cerr << "Error: Could not open file " << srcPath << endl;
        return 1;
    }
    check.close();

    string src = readFile(srcPath);
    auto tokens = tokenize(src);
    Parser parser(tokens);
    Program prog = parser.parseProgram();

    if (compileMode) {
        if (outPath.empty()) outPath = srcPath + ".s";
        CodeGen cg;
        cg.regallocEnabled = regalloc;
        string asmOut = cg.generate(prog);
        ofstream out(outPath);
        out << asmOut;
        out.close();
        cerr << "Wrote assembly to " << outPath << endl;
        return 0;
    }

    Interpreter interp;
    interp.run(prog);
    return 0;
}
