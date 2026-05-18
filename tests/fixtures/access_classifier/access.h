// access.h — fixture for S43 access-classifier unit tests.
#pragma once

struct Counter {
    int value;
    Counter(int v) : value(v) {}
    Counter& operator+=(int rhs) { value += rhs; return *this; }
};

void take_ref(int& x);
void take_ptr(int* x);
int global_val = 0;
