// callable.cpp — fixture implementations for S41 callable-extraction tests.
#include "callable.h"

int add(int a, int b) {
    return a + b;
}

int Calc::multiply(int x) {
    return x * 2;
}

double Calc::area() const {
    return 3.14159;
}
