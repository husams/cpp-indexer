// shapes.cpp — Circle implementation (m1_5file fixture).
#include "shapes.h"

// GLOBAL_VARIABLE
static const double kPi = 3.14159265358979;

double pi_value() {
    return kPi;
}

Circle::Circle(double r, int c) : radius(r), color_id(c) {}

Circle::~Circle() {}

void Circle::process() {
    radius = radius * 2.0;
}

int Circle::compute(int x) {
    return static_cast<int>(radius) + x;
}

double Circle::area() const {
    return kPi * radius * radius;
}
