// v7 full-AST schema coverage example.
//
// One translation unit that exercises every NEW v7 node kind and edge relation
// (plus the pre-existing ones it depends on). Index it and the graph should
// contain at least one instance of each construct annotated below.
//
//   Build/parse standard: C++20 (required for the Concept / CONSTRAINED_BY path).
//
// NODE kinds exercised:  Type, Parameter, Enumerator, Concept (+ Class, Method,
//                        Function, Field, Enum, TemplateDef, Specialization, Typedef, Namespace)
// EDGE relations:        RETURNS, HAS_PARAM, OF_TYPE, POINTS_TO, REFERS_TO,
//                        ALIAS_OF, TEMPLATE_PARAM, TEMPLATE_ARG, CONSTRAINED_BY,
//                        USES_NAMESPACE, USES_DECLARATION, ENUMERATOR_OF,
//                        UNDERLYING_TYPE (+ INHERITS, OVERRIDES, INSTANTIATES,
//                        SPECIALIZES, CALLS, HAS_METHOD, HAS_FIELD)

#include <cstdint>

namespace geo {

// Enum  +  UNDERLYING_TYPE (-> Type uint8_t)  +  ENUMERATOR_OF (each -> Enum)
enum class Color : std::uint8_t {
    Red,
    Green = 5,   // Enumerator carries enum_value = 5
    Blue,
};

// Typedef chain  ->  ALIAS_OF (Length -> Type double)
using Length = double;

// Pointer typedef  ->  POINTS_TO (Type Length* -> Type Length)
typedef Length* LengthPtr;

// C++20 Concept node  (constraint target for CONSTRAINED_BY)
template <typename T>
concept Number = sizeof(T) > 0;

// Base class: virtual method -> RETURNS (Type double); target of OVERRIDES.
// NOTE: RETURNS / HAS_PARAM / REFERS_TO emit on function DEFINITIONS — every
// function below is defined inline so the relations actually fire.
struct Shape {
    virtual double area() const { return 0.0; }   // RETURNS double
    virtual ~Shape() = default;
};

// Class template: TEMPLATE_PARAM (T, N) ; CONSTRAINED_BY (T -> Concept Number)
template <Number T, int N>
struct Vec {
    T data[N];                                     // Field, OF_TYPE
    T& at(int i) { return data[i]; }               // RETURNS T& (REFERS_TO); HAS_PARAM i:int
};

// Partial specialization  ->  SPECIALIZES (Vec primary) ; TEMPLATE_ARG (0)
template <Number T>
struct Vec<T, 0> {
    T* ptr;                                        // Field, OF_TYPE pointer (POINTS_TO)
};

// Derived: INHERITS (public, non-virtual) ; OVERRIDES Shape::area
class Circle : public Shape {
public:
    explicit Circle(Length r) : radius_(r) {}      // HAS_PARAM r:Length (OF_TYPE)
    double area() const override {                 // OVERRIDES + RETURNS double
        return 3.14159 * radius_ * radius_;
    }
private:
    Length radius_;                                // HAS_FIELD + OF_TYPE Length
};

// Free function (defined):
//   RETURNS Vec<double,3>  (INSTANTIATES Vec ; TEMPLATE_ARG double, 3)
//   HAS_PARAM c:const Circle&  (REFERS_TO Circle)  ;  HAS_PARAM factor:Length (OF_TYPE)
Vec<double, 3> scale(const Circle& c, Length factor) {
    Vec<double, 3> out;
    (void)c;
    (void)factor;
    return out;
}

}  // namespace geo

namespace app {

using namespace geo;     // USES_NAMESPACE  (app -> geo)
using geo::scale;        // USES_DECLARATION (app -> geo::scale)

void run() {
    geo::Circle c(1.0);
    (void)c.area();      // CALLS geo::Circle::area
}

}  // namespace app
