// B-006 E2E fixture B (deep_templates_b.cpp).
//
// A second, independently-named unit with the same instantiation depth.
//
// Its purpose is structural, not semantic: the runner collapses a one-worker
// plan onto the serial path, so a corpus needs at least two units before any
// worker thread is created at all. Both units are made deep on purpose --
// pairing a deep unit with a trivial one still reproduces, but two deep units
// make the crash independent of which rank happens to be dispatched first.
//
// The names are deliberately distinct from fixture A's so the assertions can
// tell which unit a symbol came from.

#include <cstddef>
#include <concepts>

namespace other {

template <std::size_t N> struct Wrapped {
  using type = Wrapped<N - 1>;
  static constexpr std::size_t depth = N;
};

template <> struct Wrapped<0> {
  using type = void;
  static constexpr std::size_t depth = 0;
};

template <typename T>
concept satisfies_depth = requires {
  typename T::type;
  { T::depth } -> std::convertible_to<std::size_t>;
  requires T::depth == 0 || sizeof(typename T::type) >= 0;
};

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

} // namespace other
