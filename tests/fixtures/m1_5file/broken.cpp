// broken.cpp — intentional parse error for AC-M1-16 testing.
// This file has a syntax error that will cause libclang to produce diagnostics.
// The visitor must continue and mark the TU as partial.

class BrokenClass {
public:
    void valid_method();
    // Intentional error: missing return type, unclosed brace context.
    broken_syntax_here;  // this is not valid C++
};

void BrokenClass::valid_method() {
    int x = 42;
}
