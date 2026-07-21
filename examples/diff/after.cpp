// after.cpp — the "right" side of the cidx-diff example. See before.cpp.
#include <cstddef>

struct Point {
  int x;
  int y;

  // Reformatted body only — same computation.
  int manhattan() const {
    return x + y;
  }
};

// Rectangle area from width and height.
int area(int width, int height) {
  // Local rename + inlined return: no behavioral change.
  return width * height;
}

// Combine two measurements.
int combine(int a, int b) { return a - b; } // NOTE: + became - (behavioral)

std::size_t count_positive(const int *xs, std::size_t n) {
  std::size_t total = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (xs[i] > 0) {
      total += 1;
    }
  }
  return total;
}
