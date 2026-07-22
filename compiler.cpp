// Axiom compiler: lexer -> parser -> AST -> (interpreter | x86-64 codegen)
//
// Language grammar (see syntax.txt) is small on purpose:
//   stmt   := "prt" expr
//           | "input" IDENT
//           | IDENT "=" expr
//           | "if" "(" expr ")" block ("else" "if" "(" expr ")" block)* ("else" block)?
//   block  := "{" stmt* "}"
//   expr   := primary (("==" | "!=" | ">=" | "<=" | ">" | "<") primary)?
//   primary:= NUMBER | STRING | IDENT
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
#include <memory>
#include <cstdlib>
#include <cstdio>

using namespace std;

// ===================== Lexer =====================

enum class Tok {
    IDENT, NUMBER, STRING,
    LPAREN, RPAREN, LBRACE, RBRACE,
    EQ, EQEQ, NEQ, GT, LT, GE, LE,
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
        if (c == '=' && i + 1 < n && src[i+1] == '=') { out.push_back({Tok::EQEQ, "=="}); i += 2; continue; }
        if (c == '!' && i + 1 < n && src[i+1] == '=') { out.push_back({Tok::NEQ, "!="}); i += 2; continue; }
        if (c == '>' && i + 1 < n && src[i+1] == '=') { out.push_back({Tok::GE, ">="}); i += 2; continue; }
        if (c == '<' && i + 1 < n && src[i+1] == '=') { out.push_back({Tok::LE, "<="}); i += 2; continue; }
        if (c == '=') { out.push_back({Tok::EQ, "="}); i++; continue; }
        if (c == '>') { out.push_back({Tok::GT, ">"}); i++; continue; }
        if (c == '<') { out.push_back({Tok::LT, "<"}); i++; continue; }
        // Unknown character: skip it (kept liberal, matches old interpreter's
        // "ignore what it doesn't understand" behavior).
        i++;
    }
    out.push_back({Tok::END, ""});
    return out;
}

// ===================== AST =====================

enum class ExprKind { NUMBER, STRING, IDENT, BINOP };

struct Expr {
    ExprKind kind;
    double num = 0;
    string str;               // string literal / ident name / op text ("==", "<", ...)
    unique_ptr<Expr> left, right; // BINOP operands
};

enum class StmtKind { PRT, INPUT, ASSIGN, IF };

struct Stmt;
struct IfBranch {
    unique_ptr<Expr> cond;
    vector<unique_ptr<Stmt>> body;
};

struct Stmt {
    StmtKind kind;
    unique_ptr<Expr> expr;              // PRT expr / ASSIGN rhs
    string name;                        // INPUT var / ASSIGN var
    vector<IfBranch> branches;          // IF: if + else-if branches, in order
    vector<unique_ptr<Stmt>> elseBody;  // IF: else body (empty if no else)
    bool hasElse = false;
};

using Program = vector<unique_ptr<Stmt>>;

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
        } else if (cur().type == Tok::IDENT) {
            e->kind = ExprKind::IDENT; e->str = cur().text; advance();
        } else {
            // Nothing recognizable; produce an empty string literal so
            // callers still get a well-formed (if useless) Expr node.
            e->kind = ExprKind::STRING; e->str = "";
        }
        return e;
    }

    unique_ptr<Expr> parseExpr() {
        auto left = parsePrimary();
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
        auto right = parsePrimary();
        auto bin = make_unique<Expr>();
        bin->kind = ExprKind::BINOP;
        bin->str = op;
        bin->left = std::move(left);
        bin->right = std::move(right);
        return bin;
    }

    // Skip an unrecognized statement's tokens (legacy no-op compatibility).
    void skipStatement() {
        while (cur().type != Tok::NEWLINE && cur().type != Tok::RBRACE && cur().type != Tok::END) advance();
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
            skipNewlines();
            if (cur().type == Tok::LBRACE) advance();
            skipNewlines();
            while (cur().type != Tok::RBRACE && cur().type != Tok::END) {
                auto st = parseStatement();
                if (st) b.body.push_back(std::move(st));
                skipNewlines();
            }
            if (cur().type == Tok::RBRACE) advance();
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
                    skipNewlines();
                    if (cur().type == Tok::LBRACE) advance();
                    skipNewlines();
                    while (cur().type != Tok::RBRACE && cur().type != Tok::END) {
                        auto st = parseStatement();
                        if (st) s->elseBody.push_back(std::move(st));
                        skipNewlines();
                    }
                    if (cur().type == Tok::RBRACE) advance();
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

    unique_ptr<Stmt> parseStatement() {
        skipNewlines();
        if (cur().type == Tok::END || cur().type == Tok::RBRACE) return nullptr;

        if (isIdent("if")) return parseIf();

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

        if (cur().type == Tok::IDENT && toks[pos + 1].type == Tok::EQ) {
            auto s = make_unique<Stmt>();
            s->kind = StmtKind::ASSIGN;
            s->name = cur().text;
            advance(); // ident
            advance(); // '='
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

enum class ValueType { STRING, NUMBER, BOOL, EMPTY, ERROR };

struct Value {
    ValueType type = ValueType::EMPTY;
    string s_val;
    double n_val = 0;
    bool b_val = false;

    string to_string() const {
        switch (type) {
            case ValueType::STRING: return s_val;
            case ValueType::NUMBER: return std::to_string(n_val);
            case ValueType::BOOL: return b_val ? "true" : "false";
            case ValueType::ERROR: return "ERROR: " + s_val;
            default: return "EMPTY";
        }
    }
};

struct Interpreter {
    map<string, Value> variables;

    Value evalExpr(const Expr* e) {
        Value v;
        switch (e->kind) {
            case ExprKind::NUMBER: v.type = ValueType::NUMBER; v.n_val = e->num; return v;
            case ExprKind::STRING: v.type = ValueType::STRING; v.s_val = e->str; return v;
            case ExprKind::IDENT: {
                auto it = variables.find(e->str);
                if (it != variables.end()) return it->second;
                v.type = ValueType::ERROR;
                v.s_val = "Unknown identifier: '" + e->str + "'";
                return v;
            }
            case ExprKind::BINOP: {
                Value left = evalExpr(e->left.get());
                Value right = evalExpr(e->right.get());
                Value result;
                result.type = ValueType::BOOL;
                const string& op = e->str;
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

    void execBlock(const vector<unique_ptr<Stmt>>& block) {
        for (auto& s : block) execStmt(s.get());
    }

    void execStmt(const Stmt* s) {
        switch (s->kind) {
            case StmtKind::PRT:
                cout << evalExpr(s->expr.get()).to_string() << "\n";
                return;
            case StmtKind::INPUT: {
                string line;
                getline(cin, line);
                variables[s->name] = { ValueType::STRING, line };
                return;
            }
            case StmtKind::ASSIGN:
                variables[s->name] = evalExpr(s->expr.get());
                return;
            case StmtKind::IF: {
                for (auto& branch : s->branches) {
                    if (evalExpr(branch.cond.get()).b_val) {
                        execBlock(branch.body);
                        return;
                    }
                }
                if (s->hasElse) execBlock(s->elseBody);
                return;
            }
        }
    }

    void run(const Program& prog) {
        for (auto& s : prog) execStmt(s.get());
    }
};

// ===================== x86-64 code generator =====================
// Emits GAS (AT&T) assembly. Values are heap-allocated Value* handles built
// and inspected through a small C runtime (runtime.c) -- the same way a real
// compiler leans on libc rather than hand-rolling printf/string-compare in
// assembly. The codegen itself does the real work: register moves, the
// System V calling convention, stack-slot spilling for binop operands, and
// label-based control flow for if/else-if/else.

struct CodeGen {
    ostringstream text, rodata, bss;
    map<string, bool> declaredVars;
    int labelCounter = 0;
    int litCounter = 0;

    string newLabel(const string& base) { return "." + base + std::to_string(labelCounter++); }

    void declareVar(const string& name) {
        if (declaredVars.count(name)) return;
        declaredVars[name] = true;
        bss << "var_" << name << ": .quad 0\n";
    }

    // Collect every variable name touched (assign or input) so .bss slots exist
    // before codegen references them.
    void collectVars(const vector<unique_ptr<Stmt>>& block) {
        for (auto& s : block) {
            if (s->kind == StmtKind::ASSIGN || s->kind == StmtKind::INPUT) declareVar(s->name);
            if (s->kind == StmtKind::IF) {
                for (auto& b : s->branches) collectVars(b.body);
                collectVars(s->elseBody);
            }
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

    // Emits code that leaves a Value* result in %rax.
    void genExpr(const Expr* e) {
        switch (e->kind) {
            case ExprKind::NUMBER: {
                string lbl = ".LCnum" + std::to_string(litCounter++);
                rodata << lbl << ": .double " << e->num << "\n";
                text << "    movsd " << lbl << "(%rip), %xmm0\n";
                text << "    call rt_make_num\n";
                return;
            }
            case ExprKind::STRING: {
                string lbl = ".LCstr" + std::to_string(litCounter++);
                rodata << lbl << ": .string \"" << escapeAsm(e->str) << "\"\n";
                text << "    lea " << lbl << "(%rip), %rdi\n";
                text << "    call rt_make_str\n";
                return;
            }
            case ExprKind::IDENT: {
                declareVar(e->str); // reading before writing: 0 == uninitialized (rt_* handles null)
                text << "    mov var_" << e->str << "(%rip), %rax\n";
                return;
            }
            case ExprKind::BINOP: {
                genExpr(e->left.get());
                text << "    sub $16, %rsp\n";
                text << "    mov %rax, (%rsp)\n";
                genExpr(e->right.get());
                text << "    mov %rax, %rdx\n";      // right -> arg3
                text << "    mov (%rsp), %rsi\n";     // left  -> arg2
                text << "    add $16, %rsp\n";
                text << "    mov $" << opCode(e->str) << ", %edi\n"; // op -> arg1
                text << "    call rt_cmp\n";
                return;
            }
        }
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

    void genStmt(const Stmt* s) {
        switch (s->kind) {
            case StmtKind::PRT:
                genExpr(s->expr.get());
                text << "    mov %rax, %rdi\n";
                text << "    call rt_print\n";
                return;
            case StmtKind::INPUT:
                text << "    call rt_input\n";
                text << "    mov %rax, var_" << s->name << "(%rip)\n";
                return;
            case StmtKind::ASSIGN:
                genExpr(s->expr.get());
                text << "    mov %rax, var_" << s->name << "(%rip)\n";
                return;
            case StmtKind::IF: {
                string end = newLabel("Lend");
                for (size_t i = 0; i < s->branches.size(); i++) {
                    string next = (i + 1 < s->branches.size() || s->hasElse) ? newLabel("Lnext") : end;
                    genExpr(s->branches[i].cond.get());
                    text << "    mov %rax, %rdi\n";
                    text << "    call rt_truthy\n";
                    text << "    test %eax, %eax\n";
                    text << "    je " << next << "\n";
                    genBlock(s->branches[i].body);
                    text << "    jmp " << end << "\n";
                    if (next != end) text << next << ":\n";
                }
                if (s->hasElse) genBlock(s->elseBody);
                text << end << ":\n";
                return;
            }
        }
    }

    string generate(const Program& prog) {
        collectVars(prog);
        genBlock(prog);

        ostringstream out;
        out << "    .extern rt_make_num\n"
            << "    .extern rt_make_str\n"
            << "    .extern rt_print\n"
            << "    .extern rt_input\n"
            << "    .extern rt_cmp\n"
            << "    .extern rt_truthy\n"
            << "    .section .rodata\n"
            << rodata.str()
            << "    .section .bss\n"
            << bss.str()
            << "    .section .text\n"
            << "    .globl main\n"
            << "main:\n"
            << "    push %rbp\n"
            << "    mov %rsp, %rbp\n"
            << text.str()
            << "    mov $0, %eax\n"
            << "    pop %rbp\n"
            << "    ret\n";
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
    string srcPath, outPath;

    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--compile") compileMode = true;
        else if (args[i] == "--interpret") compileMode = false;
        else if (args[i] == "-o" && i + 1 < args.size()) outPath = args[++i];
        else if (srcPath.empty()) srcPath = args[i];
    }

    if (srcPath.empty()) {
        cerr << "Usage: " << argv[0] << " [--interpret|--compile] <source_file.lang> [-o output.s]" << endl;
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
