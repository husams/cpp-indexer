// ext_main.cpp — exercises all M2 extension node/edge kinds.
//
// Constructs exercised:
//   NAMESPACE       — shapes, shapes::inner
//   TEMPLATE_DECL   — Container<T>, identity<T>
//   SPECIALIZATION  — Container<T*>  (partial specialization)
//   TYPEDEF         — ShapeId, ShapeCount, MyId
//   ENUM            — Color (scoped), Direction (unscoped)
//   HEADER          — ext_base.h, ext_shapes.h  (INCLUDES edges)
//   OVERRIDES       — IntContainer::get → Container<int>::get
//   FRIEND_OF       — IntContainer → ContainerFactory
//   SPECIALIZES     — Container<T*> → Container<T>

#include "ext_base.h"
#include "ext_shapes.h"

namespace shapes {
namespace inner {

// Another typedef inside a nested namespace
typedef ShapeId InnerShapeId;

// A function that uses the template → INSTANTIATES (best-effort)
inline int use_identity(int x) {
    return identity<int>(x);
}

} // namespace inner
} // namespace shapes

// Local typedef at file scope
typedef shapes::ShapeId MyId;

int main() {
    shapes::IntContainer c;
    c.value = 42;
    int v = c.get();

    shapes::Color col = shapes::Color::Red;
    shapes::Direction dir = shapes::North;
    (void)v;
    (void)col;
    (void)dir;
    return 0;
}
