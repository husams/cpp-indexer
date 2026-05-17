// main.cpp — exercises macros and templates from macros.h.
//
// Expands function-like and object-like macros at function scope to generate
// EXPANDS_TO edges from the enclosing function's USR to each MACRO node.
// Also instantiates the Container<T> and Container<T*> templates.
#include "macros.h"

// --- Functions that expand macros at function scope ---

/// Uses ASSERT_TRUE, VERSION_STR, and MAKE_PAIR (AC-M5-11: EXPANDS_TO edges).
int use_object_like_macros() {
    ASSERT_TRUE(1 == 1);
    const char* v = VERSION_STR;
    (void)v;
    return VERSION_MAJOR * 100 + VERSION_MINOR;
}

/// Uses function-like macros CHECK_NULL and CLAMP.
int use_function_like_macros(int* p, int val) {
    CHECK_NULL(p);
    return CLAMP(val, MIN_VALUE, MAX_VALUE);
}

/// Uses the Container<T> primary template.
int use_container_int() {
    Container<int> c(42);
    ASSERT_TRUE(!c.empty());
    return c.size();
}

/// Uses the Container<T*> partial specialisation.
int use_container_ptr(int* p) {
    Container<int*> c(p);
    CHECK_NULL(c.value());
    return c.empty() ? 0 : 1;
}

/// Uses the clamp function template and MAKE_PAIR macro.
int use_template_and_macro() {
    int x = clamp(5, MIN_VALUE, MAX_VALUE);
    // MAKE_PAIR expands to a braced initialiser; assign to a trivial aggregate.
    struct Pair { int a; int b; };
    Pair pair = MAKE_PAIR(x, VERSION_MAJOR);
    (void)pair;
    return x;
}
