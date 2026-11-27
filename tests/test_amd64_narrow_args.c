#include "src/lower/amd64_abi.h"

#include "src/register/register.h"
#include "src/symbol/symbol.h"

#include <stdio.h>
#include <string.h>

#define NARROW_ARGS_CHECK(condition)                                                \
    do {                                                                            \
        if (!(condition)) {                                                         \
            fprintf(stderr, "narrow args check failed at %s:%d: %s\n", __FILE__,    \
                    __LINE__, #condition);                                          \
            return false;                                                           \
        }                                                                           \
    } while (0)

static lir_var_t *test_var(char *ident, type_t type) {
    lir_var_t *var = NEW(lir_var_t);
    var->ident = ident;
    var->old = ident;
    var->type = type;
    return var;
}

static module_t *test_module(void) {
    module_t *module = NEW(module_t);
    module->ident = "narrow_args_test";
    module->global_symbol_table = table_new();
    module->global_symbols = slice_new();
    module->asm_global_symbols = slice_new();
    return module;
}

/**
 * Lowers `callee(arg)` with the System V ABI. The callee is C built by clang in practice (the runtime, libc), and clang
 * reads a bool, i8/u8 or i16/u16 parameter from the whole 32 bit register, so the argument has to arrive extended to
 * 32 bits: zero for a bool and the unsigned types, sign for the signed ones.
 */
static linked_t *test_lower_call_with_arg(lir_operand_t *arg) {
    ast_fndef_t fndef = {.return_type = type_kind_new(TYPE_VOID)};
    module_t *module = test_module();
    closure_t *closure = lir_closure_new(&fndef);
    closure->module = module;
    module->current_closure = closure;

    lir_op_t *call = lir_call("callee", NULL, 1, arg);
    return amd64_lower_call(closure, call);
}

// the one '~' op of the lowered call, the one that sets the first argument register: its output is a variable hinted to rdi
static lir_op_t *find_arg_op(linked_t *lowered) {
    lir_op_t *arg_op = NULL;
    LINKED_FOR(lowered) {
        lir_op_t *op = node->value;
        if (op->resolve_char == '~') {
            if (arg_op != NULL) {
                return NULL;
            }
            arg_op = op;
        }
    }
    return arg_op;
}

static bool check_arg_reg(lir_op_t *arg_op, type_kind want_kind, uint8_t want_size) {
    NARROW_ARGS_CHECK(arg_op->output->assert_type == LIR_OPERAND_VAR);
    lir_var_t *dst = arg_op->output->value;
    NARROW_ARGS_CHECK(dst->type.kind == want_kind);
    NARROW_ARGS_CHECK(dst->must_hint != NULL);
    NARROW_ARGS_CHECK(dst->must_hint->index == rdi->index);
    NARROW_ARGS_CHECK(dst->must_hint->size == want_size);
    return true;
}

// the extension is the argument op itself: one instruction into the argument register, no extra move
static bool check_narrow_var_arg(type_kind arg_kind, type_kind want_kind, lir_opcode_t want_ext) {
    lir_var_t *var = test_var("arg", type_kind_new(arg_kind));
    lir_operand_t *arg = operand_new(LIR_OPERAND_VAR, var);
    linked_t *lowered = test_lower_call_with_arg(arg);

    lir_op_t *arg_op = find_arg_op(lowered);
    NARROW_ARGS_CHECK(arg_op != NULL);
    NARROW_ARGS_CHECK(arg_op->code == want_ext);
    NARROW_ARGS_CHECK(lir_operand_equal(arg_op->first, arg));
    NARROW_ARGS_CHECK(check_arg_reg(arg_op, want_kind, DWORD));

    // and nothing else is added besides it and the call
    LINKED_FOR(lowered) {
        lir_op_t *op = node->value;
        NARROW_ARGS_CHECK(op == arg_op || op->code == LIR_OPCODE_CALL);
    }
    return true;
}

static bool test_narrow_variables_are_extended(void) {
    NARROW_ARGS_CHECK(check_narrow_var_arg(TYPE_BOOL, TYPE_UINT32, LIR_OPCODE_UEXT));
    NARROW_ARGS_CHECK(check_narrow_var_arg(TYPE_UINT8, TYPE_UINT32, LIR_OPCODE_UEXT));
    NARROW_ARGS_CHECK(check_narrow_var_arg(TYPE_UINT16, TYPE_UINT32, LIR_OPCODE_UEXT));
    NARROW_ARGS_CHECK(check_narrow_var_arg(TYPE_INT8, TYPE_INT32, LIR_OPCODE_SEXT));
    NARROW_ARGS_CHECK(check_narrow_var_arg(TYPE_INT16, TYPE_INT32, LIR_OPCODE_SEXT));
    return true;
}

static lir_operand_t *test_imm(type_kind kind, uint64_t value) {
    lir_imm_t *imm = NEW(lir_imm_t);
    imm->kind = kind;
    imm->uint_value = value;
    lir_operand_t *operand = NEW(lir_operand_t);
    operand->assert_type = LIR_OPERAND_IMM;
    operand->value = imm;
    return operand;
}

// movzx and movsx have no immediate form, a constant is widened instead of extended
static bool check_narrow_imm_arg(lir_operand_t *arg, type_kind want_kind, int64_t want_value) {
    linked_t *lowered = test_lower_call_with_arg(arg);

    LINKED_FOR(lowered) {
        lir_op_t *op = node->value;
        NARROW_ARGS_CHECK(op->code != LIR_OPCODE_UEXT && op->code != LIR_OPCODE_SEXT);
    }

    lir_op_t *arg_op = find_arg_op(lowered);
    NARROW_ARGS_CHECK(arg_op != NULL);
    NARROW_ARGS_CHECK(arg_op->code == LIR_OPCODE_MOVE);
    NARROW_ARGS_CHECK(check_arg_reg(arg_op, want_kind, DWORD));
    NARROW_ARGS_CHECK(arg_op->first->assert_type == LIR_OPERAND_IMM);
    lir_imm_t *imm = arg_op->first->value;
    NARROW_ARGS_CHECK(imm->kind == want_kind);
    NARROW_ARGS_CHECK(imm->int_value == want_value);
    return true;
}

static bool test_narrow_constants_are_widened(void) {
    NARROW_ARGS_CHECK(check_narrow_imm_arg(bool_operand(true), TYPE_UINT32, 1));
    NARROW_ARGS_CHECK(check_narrow_imm_arg(bool_operand(false), TYPE_UINT32, 0));
    NARROW_ARGS_CHECK(check_narrow_imm_arg(test_imm(TYPE_UINT8, 200), TYPE_UINT32, 200));
    NARROW_ARGS_CHECK(check_narrow_imm_arg(test_imm(TYPE_UINT16, 60000), TYPE_UINT32, 60000));
    // a negative constant is held as the two's complement of its 64 bit value
    NARROW_ARGS_CHECK(check_narrow_imm_arg(test_imm(TYPE_INT8, (uint64_t) -5), TYPE_INT32, -5));
    NARROW_ARGS_CHECK(check_narrow_imm_arg(test_imm(TYPE_INT16, (uint64_t) -300), TYPE_INT32, -300));
    return true;
}

// 32 and 64 bit arguments already fill the register (a 32 bit write clears the upper half), nothing is added for them
static bool check_wide_arg_unchanged(type_kind kind, uint8_t want_size) {
    lir_var_t *var = test_var("arg", type_kind_new(kind));
    lir_operand_t *arg = operand_new(LIR_OPERAND_VAR, var);
    linked_t *lowered = test_lower_call_with_arg(arg);

    LINKED_FOR(lowered) {
        lir_op_t *op = node->value;
        NARROW_ARGS_CHECK(op->code != LIR_OPCODE_UEXT && op->code != LIR_OPCODE_SEXT);
    }

    lir_op_t *arg_op = find_arg_op(lowered);
    NARROW_ARGS_CHECK(arg_op != NULL);
    NARROW_ARGS_CHECK(arg_op->code == LIR_OPCODE_MOVE);
    NARROW_ARGS_CHECK(lir_operand_equal(arg_op->first, arg));
    NARROW_ARGS_CHECK(check_arg_reg(arg_op, kind, want_size));
    return true;
}

static bool test_wide_arguments_are_untouched(void) {
    NARROW_ARGS_CHECK(check_wide_arg_unchanged(TYPE_INT32, DWORD));
    NARROW_ARGS_CHECK(check_wide_arg_unchanged(TYPE_UINT32, DWORD));
    NARROW_ARGS_CHECK(check_wide_arg_unchanged(TYPE_INT64, QWORD));
    NARROW_ARGS_CHECK(check_wide_arg_unchanged(TYPE_ANYPTR, QWORD));
    return true;
}

int main(void) {
    BUILD_OS = OS_LINUX;
    BUILD_ARCH = ARCH_AMD64;
    symbol_init();
    reg_init();

    if (!test_narrow_variables_are_extended()) return 1;
    if (!test_narrow_constants_are_widened()) return 1;
    if (!test_wide_arguments_are_untouched()) return 1;
    return 0;
}
