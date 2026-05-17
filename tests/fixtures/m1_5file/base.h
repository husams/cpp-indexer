// base.h — abstract base class for m1_5file fixture.
#pragma once

class Base {
public:
    Base() = default;
    virtual ~Base() = default;

    virtual void process() = 0;
    virtual int compute(int x) = 0;
};
