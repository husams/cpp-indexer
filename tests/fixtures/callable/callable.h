// callable.h — fixture for S41 callable-extraction tests.
#pragma once

// Free function with two parameters (AC-S41-1/2/4/8).
int add(int a, int b);

// Class with a const-qualified method (AC-S41-3/8).
class Calc {
public:
    // Non-const method: returns int, takes int.
    int multiply(int x);

    // Const method: return type double, no params (AC-S41-3).
    double area() const;
};
