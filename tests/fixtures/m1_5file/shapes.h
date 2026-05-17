// shapes.h — class declarations for m1_5file fixture.
#pragma once
#include "base.h"

// A concrete class with fields and methods.
class Circle : public Base {
public:
    double radius;   // FIELD
    int color_id;    // FIELD

    Circle(double r, int c);
    ~Circle() override;

    void process() override;
    int compute(int x) override;
    double area() const;
};

// A free-standing helper function.
double pi_value();
