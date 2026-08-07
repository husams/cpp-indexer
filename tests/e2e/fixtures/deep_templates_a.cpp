// B-006 E2E fixture A (deep_templates_a.cpp).
//
// Drives the exact Clang recursion that appears in the B-006 faulting stack:
// ConstraintSatisfactionChecker::Evaluate -> TransformRequiresExpr ->
// TransformTemplateSpecializationType -> CheckTemplateIdType, repeating.
//
// The depth is a compile-time recursion over a class template whose partial
// specialisation is selected through a constrained requires-expression, so
// each instantiation step pushes a full TreeTransform frame. 200 levels is far
// past a 512 KiB stack and far inside 8 MiB; it costs well under a second to
// compile, which keeps the E2E suite fast.
//
// Keep the depth in both fixtures identical so the two units are comparable.

#include <cstddef>
#include <concepts>

namespace deep {

template <std::size_t N> struct Wrapped {
  using type = Wrapped<N - 1>;
  static constexpr std::size_t depth = N;
};

template <> struct Wrapped<0> {
  using type = void;
  static constexpr std::size_t depth = 0;
};

// The constraint is what forces constraint-satisfaction checking to recurse
// alongside the instantiation, rather than the instantiation alone.
template <typename T>
concept satisfies_depth = requires {
  typename T::type;
  { T::depth } -> std::convertible_to<std::size_t>;
  requires T::depth == 0 || sizeof(typename T::type) >= 0;
};

// Peeled<T> recurses through T::type, re-checking the constraint at every
// level. Each level is one TransformTemplateSpecializationType frame.
template <typename T, bool AtBottom = (T::depth == 0)> struct Peeled;

template <typename T> struct Peeled<T, true> {
  static constexpr std::size_t levels = 0;
};

template <typename T>
  requires satisfies_depth<T>
struct Peeled<T, false> {
  static constexpr std::size_t levels =
      1 + Peeled<typename T::type>::levels;
};

constexpr std::size_t kDepth = 200;

auto unwrap() -> std::size_t { return Peeled<Wrapped<kDepth>>::levels; }

} // namespace deep
