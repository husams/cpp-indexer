// utils.h — utility functions for m1_5file fixture.
#pragma once

// A plain struct (mapped to CLASS node).
struct Point {
    double x;  // FIELD
    double y;  // FIELD
};

// Free functions.
double distance(const Point& a, const Point& b);
Point midpoint(const Point& a, const Point& b);
