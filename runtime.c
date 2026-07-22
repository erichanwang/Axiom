// Minimal runtime linked with x86-64 assembly emitted by `compiler --compile`.
// Mirrors the Value semantics of the C++ interpreter in compiler.cpp
// (STRING / NUMBER / BOOL / EMPTY), so both execution paths agree.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { VT_STRING, VT_NUMBER, VT_BOOL, VT_EMPTY } ValueType;

typedef struct {
    ValueType type;
    double num;
    char* str;
} Value;

static Value* alloc_value(void) {
    return (Value*)calloc(1, sizeof(Value));
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

static const char* value_to_string(Value* v, char* buf, size_t bufsize) {
    if (!v) return "EMPTY";
    switch (v->type) {
        case VT_STRING: return v->str ? v->str : "";
        case VT_NUMBER: snprintf(buf, bufsize, "%.6f", v->num); return buf;
        case VT_BOOL: return v->num != 0 ? "true" : "false";
        default: return "EMPTY";
    }
}

void rt_print(Value* v) {
    char buf[64];
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

int rt_truthy(Value* v) {
    return v && v->num != 0;
}
