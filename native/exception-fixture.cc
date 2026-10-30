static int constructor_args_ok;
static_assert(sizeof(long double)==16 && __LDBL_MANT_DIG__==113,"Linux arm64 binary128 required");

__attribute__((constructor)) static void initialize(int argc, char **argv, char **envp) {
    constructor_args_ok = argc == 1 && argv && argv[0] && envp && envp[0];
}

extern "C" int caught_exception() {
    if (!constructor_args_ok) return -1;
    try {
        throw 42;
    } catch (int value) {
        return value;
    }
}

#define QUAD_CASE(name, value) extern "C" long double quad_##name() { return value; }
QUAD_CASE(tenth, 0.1L)
QUAD_CASE(exact, 1.25L)
QUAD_CASE(negative, -42.75L)
QUAD_CASE(precision, 1.0000000000000000000000000000000001L)
QUAD_CASE(pi, 3.14159265358979323846264338327950288L)
QUAD_CASE(huge, 1e4000L)
QUAD_CASE(tiny, 1e-4000L)
QUAD_CASE(minsub, 0x1p-16494L)
QUAD_CASE(minnormal, 0x1p-16382L)
QUAD_CASE(maxfinite, 0x1.ffffffffffffffffffffffffffffp+16383L)
QUAD_CASE(overflow, __builtin_infl())
QUAD_CASE(underflow, 0.0L)
QUAD_CASE(negativezero, -0.0L)
