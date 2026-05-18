// access_extended.cpp — boundary fixture for QA-added access-mode coverage.
//
// Covers the three access modes not exercised by the primary access.cpp fixture:
//  - addr_of_var:  address-of operator (&x) → "addr_of"
//  - return_var:   operand of return statement → "return"
//  - decl_ref_var: variable used in a VarDecl initializer → "decl_ref"
//
// These three modes complete end-to-end coverage of all 7 AC-S43-1 values.

#include "access.h"

// Case 5: address-of operator — &global_val → "addr_of"
int* addr_of_var() {
    return &global_val;  // UnaryOperator(&) wrapping DeclRefExpr(global_val)
}

// Case 6: return operand — global_val appears as the operand of ReturnStmt
int return_var() {
    return global_val;   // ReturnStmt wrapping DeclRefExpr(global_val)
}

// Case 7: VarDecl initializer — x is initialized from global_val → "decl_ref"
void decl_ref_var() {
    int x = global_val;  // VarDecl(x) initializer contains DeclRefExpr(global_val)
    (void)x;
}
