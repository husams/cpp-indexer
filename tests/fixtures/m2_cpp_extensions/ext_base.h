// ext_base.h — base header for m2_cpp_extensions fixture.
#pragma once

// NAMESPACE: outer
namespace shapes {

// TEMPLATE_DECL (class template)
template <typename T>
class Container {
public:
    T value;
    virtual T get() const { return value; }
    virtual ~Container() = default;
};

// ENUM (scoped)
enum class Color { Red, Green, Blue };

// ENUM (unscoped)
enum Direction { North, South, East, West };

// TYPEDEF
typedef int ShapeId;

// using alias
using ShapeCount = unsigned int;

} // namespace shapes
