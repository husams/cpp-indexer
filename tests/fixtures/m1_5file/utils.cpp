// utils.cpp — utility function implementations (m1_5file fixture).
// Avoids standard library headers to remain portable across toolchains.
#include "utils.h"

// GLOBAL_VARIABLE
static int g_call_count = 0;

// Simple sqrt approximation to avoid <cmath> dependency.
static double my_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double guess = x / 2.0;
    for (int i = 0; i < 20; ++i) {
        guess = (guess + x / guess) / 2.0;
    }
    return guess;
}

double distance(const Point& a, const Point& b) {
    ++g_call_count;
    double dx = a.x - b.x;
    double dy = a.y - b.y;
    return my_sqrt(dx * dx + dy * dy);
}

Point midpoint(const Point& a, const Point& b) {
    ++g_call_count;
    return Point{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0};
}
