// access.cpp — fixture for S43 access-classifier unit tests.
//
// Each function exercises one access mode:
//  - read_member:    member field read (RHS of struct assignment) → "read"
//  - field_write:    assignment to field → "write"
//  - arg_pass:       pass variable as function argument (by ref) → "call_arg"
//  - overloaded_op:  overloaded operator+= → "unknown"

#include "access.h"

// Case 1: member field read (RHS of binary operator, no implicit cast for struct member)
void read_member(Counter& a, Counter& b) {
    a.value = b.value;  // b.value (MemberRefExpr) — RHS of BinaryOperator (=) → "read"
                        // a.value (MemberRefExpr) — LHS of BinaryOperator (=) → "write"
}

// Case 2: field write (LHS of assignment) — verified from read_member above
// (same fixture, a.value is the write case)

// Case 3: pass as call argument (conservative call_arg)
void arg_pass(int x) {
    take_ref(x);  // DeclRefExpr for x — child of CallExpr → "call_arg"
}

// Case 4: overloaded operator (unknown)
void overloaded_op(Counter& c, int v) {
    c += v;       // overloaded operator+= — unclassifiable → "unknown"
}
