// Minimal runtime linked with x86-64 assembly emitted by `compiler --compile`.
// Mirrors the Value semantics of the C++ interpreter in compiler.cpp
// (STRING / NUMBER / BOOL / EMPTY), so both execution paths agree.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef enum { VT_STRING, VT_NUMBER, VT_BOOL, VT_EMPTY, VT_ERROR, VT_ARRAY } ValueType;

typedef struct Value {
    ValueType type;
    double num;
    char* str;
    struct Value** arr;   // VT_ARRAY: element slots
    long arr_len;         // VT_ARRAY: element count
} Value;

// Values are never individually freed: the language has no destructor, no
// reference count, and no collector, so every Value produced during a run
// lives until the process exits. That makes a bump allocator exactly
// equivalent to calloc here, minus the free-list bookkeeping malloc does for
// a free that never comes. Arithmetic allocates a Value per intermediate
// result, so this sits directly under the hottest loop in any program.
#define ARENA_CHUNK (1u << 20)
static char* arena_next = NULL;
static size_t arena_left = 0;

static Value* alloc_value(void) {
    if (arena_left < sizeof(Value)) {
        arena_next = (char*)malloc(ARENA_CHUNK);
        if (!arena_next) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        arena_left = ARENA_CHUNK;
    }
    Value* v = (Value*)arena_next;
    // sizeof(Value) is a multiple of 8 and malloc hands back 16-aligned
    // blocks, so bumping by it keeps every Value aligned for its double.
    arena_next += sizeof(Value);
    arena_left -= sizeof(Value);
    v->type = 0;      // matches calloc: VT_STRING is 0, overwritten by callers
    v->num = 0;
    v->str = NULL;
    v->arr = NULL;
    v->arr_len = 0;
    return v;
}

Value* rt_make_num(double n) {
    Value* v = alloc_value();
    v->type = VT_NUMBER;
    v->num = n;
    return v;
}

Value* rt_make_str(const char* s) {
    Value* v = alloc_value();
    v->type = VT_STRING;
    v->str = strdup(s ? s : "");
    return v;
}

Value* rt_make_bool(int b) {
    Value* v = alloc_value();
    v->type = VT_BOOL;
    v->num = b ? 1 : 0;
    return v;
}

Value* rt_make_err(const char* msg) {
    Value* v = alloc_value();
    v->type = VT_ERROR;
    v->str = strdup(msg ? msg : "");
    return v;
}

static const char* value_to_string(Value* v, char* buf, size_t bufsize);

// Appends `s` to a growable heap buffer, doubling capacity as needed.
static void str_append(char** buf, size_t* cap, size_t* len, const char* s) {
    size_t slen = strlen(s);
    if (*len + slen + 1 > *cap) {
        while (*len + slen + 1 > *cap) *cap *= 2;
        *buf = (char*)realloc(*buf, *cap);
    }
    strcpy(*buf + *len, s);
    *len += slen;
}

// Builds "[e0, e1, ...]", recursing into value_to_string per element so
// nested arrays print the same way the interpreter's Value::to_string does.
// The returned buffer is never freed -- consistent with every other Value
// payload in this runtime, which lives until the process exits.
static char* array_to_string(Value* v) {
    size_t cap = 64, len = 0;
    char* out = (char*)malloc(cap);
    out[0] = '\0';
    str_append(&out, &cap, &len, "[");
    for (long i = 0; i < v->arr_len; i++) {
        if (i) str_append(&out, &cap, &len, ", ");
        char buf[256];
        str_append(&out, &cap, &len, value_to_string(v->arr[i], buf, sizeof(buf)));
    }
    str_append(&out, &cap, &len, "]");
    return out;
}

static const char* value_to_string(Value* v, char* buf, size_t bufsize) {
    if (!v) return "EMPTY";
    switch (v->type) {
        case VT_STRING: return v->str ? v->str : "";
        case VT_NUMBER: snprintf(buf, bufsize, "%.6f", v->num); return buf;
        case VT_BOOL: return v->num != 0 ? "true" : "false";
        case VT_ERROR: snprintf(buf, bufsize, "ERROR: %s", v->str ? v->str : ""); return buf;
        case VT_ARRAY: return array_to_string(v);
        default: return "EMPTY";
    }
}

void rt_print(Value* v) {
    char buf[256];
    printf("%s\n", value_to_string(v, buf, sizeof(buf)));
}

Value* rt_input(void) {
    char* line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, stdin);
    if (len < 0) {
        free(line);
        return rt_make_str("");
    }
    if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
    Value* v = rt_make_str(line);
    free(line);
    return v;
}

// op: 0 == , 1 != , 2 > , 3 < , 4 >= , 5 <=
Value* rt_cmp(int op, Value* left, Value* right) {
    Value* r = alloc_value();
    r->type = VT_BOOL;
    char lb[64], rb[64];
    int both_numbers = left && right && left->type == VT_NUMBER && right->type == VT_NUMBER;

    switch (op) {
        case 0: // ==
            if (both_numbers) r->num = (left->num == right->num);
            else r->num = strcmp(value_to_string(left, lb, sizeof(lb)),
                                  value_to_string(right, rb, sizeof(rb))) == 0;
            break;
        case 1: // !=
            if (both_numbers) r->num = (left->num != right->num);
            else r->num = strcmp(value_to_string(left, lb, sizeof(lb)),
                                  value_to_string(right, rb, sizeof(rb))) != 0;
            break;
        case 2: r->num = (left ? left->num : 0) > (right ? right->num : 0); break;  // >
        case 3: r->num = (left ? left->num : 0) < (right ? right->num : 0); break;  // <
        case 4: r->num = (left ? left->num : 0) >= (right ? right->num : 0); break; // >=
        default: r->num = (left ? left->num : 0) <= (right ? right->num : 0); break; // <=
    }
    return r;
}

// op: 0 + , 1 - , 2 * , 3 / , 4 %
// Mirrors Interpreter::evalArith in compiler.cpp exactly; run_tests.sh diffs the
// two backends on every .lang program, so any drift here fails the suite.
Value* rt_arith(int op, Value* left, Value* right) {
    if (left && left->type == VT_ERROR) return left;
    if (right && right->type == VT_ERROR) return right;

    int both_numbers = left && right && left->type == VT_NUMBER && right->type == VT_NUMBER;

    if (op == 0 && !both_numbers) {   // '+' on anything but two numbers concatenates
        char lb[256], rb[256];
        const char* ls = value_to_string(left, lb, sizeof(lb));
        const char* rs = value_to_string(right, rb, sizeof(rb));
        char* joined = (char*)malloc(strlen(ls) + strlen(rs) + 1);
        strcpy(joined, ls);
        strcat(joined, rs);
        Value* v = alloc_value();
        v->type = VT_STRING;
        v->str = joined;
        return v;
    }
    if (!both_numbers) return rt_make_err("non-numeric operand");

    if ((op == 3 || op == 4) && right->num == 0) return rt_make_err("division by zero");

    Value* r = alloc_value();
    r->type = VT_NUMBER;
    switch (op) {
        case 0: r->num = left->num + right->num; break;
        case 1: r->num = left->num - right->num; break;
        case 2: r->num = left->num * right->num; break;
        case 3: r->num = left->num / right->num; break;
        default: r->num = fmod(left->num, right->num); break;
    }
    return r;
}

// Reading a name that was never assigned. The interpreter reports this and
// keeps going, so the compiled path has to produce the same error value
// rather than treat the empty slot as a usable operand: a null Value* used to
// fall through to rt_arith, which stringified it and quietly concatenated
// "EMPTY" into the output instead of failing.
Value* rt_undef(const char* name) {
    char buf[256];
    snprintf(buf, sizeof(buf), "Unknown identifier: '%s'", name ? name : "");
    return rt_make_err(buf);
}

Value* rt_neg(Value* v) {
    if (v && v->type == VT_ERROR) return v;
    if (!v || v->type != VT_NUMBER) return rt_make_err("non-numeric operand");
    return rt_make_num(-v->num);
}

// --- Arrays --------------------------------------------------------------
// Semantics (must match Interpreter::arrayIndex / execStmt INDEXSET exactly,
// see compiler.cpp and syntax.txt):
//   - Indices truncate toward zero via a C (long) cast, same as the
//     interpreter's (long) cast on the same double.
//   - Reading with a negative or too-large index, or a non-numeric index,
//     or indexing a non-array, produces an ERROR value.
//   - Writing (arr[i] = v) to an invalid index or through a non-array is a
//     silent no-op: the index and value expressions still evaluate (so any
//     side effects, e.g. a call that prints, still happen), only the store
//     itself is skipped.

// Allocates an array of `n` slots, all initially NULL. Construction always
// fills every slot immediately afterward (rt_array_set_elem), so a NULL slot
// is never observed by rt_array_get in a running program.
Value* rt_array_new(long n) {
    Value* v = alloc_value();
    v->type = VT_ARRAY;
    v->arr_len = n;
    v->arr = n > 0 ? (Value**)malloc(n * sizeof(Value*)) : NULL;
    for (long i = 0; i < n; i++) v->arr[i] = NULL;
    return v;
}

// Raw slot store used only during array-literal construction: the index is
// compiler-generated and always in bounds, so no validation here.
void rt_array_set_elem(Value* arr, long idx, Value* val) {
    if (arr && arr->arr && idx >= 0 && idx < arr->arr_len) arr->arr[idx] = val;
}

Value* rt_array_get(Value* arr, Value* idx) {
    if (arr && arr->type == VT_ERROR) return arr;
    if (idx && idx->type == VT_ERROR) return idx;
    if (!arr || arr->type != VT_ARRAY) return rt_make_err("not an array");
    if (!idx || idx->type != VT_NUMBER) return rt_make_err("non-numeric index");
    long i = (long)idx->num;
    if (i < 0 || i >= arr->arr_len) return rt_make_err("index out of bounds");
    return arr->arr[i];
}

void rt_array_set(Value* arr, Value* idx, Value* val) {
    if (!arr || arr->type != VT_ARRAY) return;      // not an array: no-op
    if (!idx || idx->type != VT_NUMBER) return;      // non-numeric index: no-op
    long i = (long)idx->num;
    if (i < 0 || i >= arr->arr_len) return;          // out of bounds: no-op
    arr->arr[i] = val;
}

Value* rt_array_len(Value* arr) {
    if (arr && arr->type == VT_ERROR) return arr;
    if (!arr || arr->type != VT_ARRAY) return rt_make_err("not an array");
    return rt_make_num((double)arr->arr_len);
}

int rt_truthy(Value* v) {
    if (!v) return 0;
    if (v->type == VT_NUMBER || v->type == VT_BOOL) return v->num != 0;
    return 0;
}
