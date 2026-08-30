#include "single_traversal_fact_extraction.hpp"

namespace routed_fixture {
namespace detail {
struct Base {
  int value = 1;
};
} // namespace detail

namespace nested {
using namespace detail;

struct Widget : Base {
  int member(int value) const;
};

int Widget::member(int value) const {
  owned::HeaderType header;
  return header.value + value + Base::value;
}

int repeated(int);
int repeated(int value) { return value + 1; }

template <typename T> T templated(T value) { return value; }

int local_method(int value) {
  struct Local {
    int run(int input) const { return input + 2; }
  } local;
  const auto lambda = [value](int input) { return value + input; };
  return local.run(lambda(value));
}

} // namespace nested
} // namespace routed_fixture
