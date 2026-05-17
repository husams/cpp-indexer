// ext_shapes.h — derived classes and friends for m2_cpp_extensions fixture.
#pragma once
#include "ext_base.h"

namespace shapes {

// CLASS with OVERRIDES + FRIEND_OF
class IntContainer : public Container<int> {
public:
    // friend declaration → FRIEND_OF edge
    friend class ContainerFactory;

    // OVERRIDES Container<int>::get
    int get() const override { return value * 2; }
};

// Friend class referenced above
class ContainerFactory {
public:
    IntContainer make(int v);
};

// TEMPLATE_DECL (function template)
template <typename T>
T identity(T x) { return x; }

// SPECIALIZATION: partial specialization of Container for pointer types
template <typename T>
class Container<T*> {
public:
    T* value;
    T* get() const { return value; }
};

} // namespace shapes
