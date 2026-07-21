// before.cpp — the "left" side of the cidx-diff example.
//
// Paired with after.cpp, this shows all three verdicts cidx-diff can produce:
//   * area()   — refactored with NO behavioral change (rename + reformat +
//                comment). syntax: edits; semantic: equivalent/unknown.
//   * combine() — a real behavioral change (+ becomes -). semantic: different.
//   * Point    — a class whose method body is reformatted only.
#include <cstddef>

struct Point {
  int x;
  int y;

  int manhattan() const { return x + y; }
};

// Rectangle area from two corner points.
int area(int w, int h) {
  int result = w * h;
  return result;
}

// Combine two measurements.
int combine(int a, int b) { return a + b; }

std::size_t count_positive(const int *xs, std::size_t n) {
  std::size_t total = 0;
  for (std::size_t i = 0; i < n; ++i) {
    if (xs[i] > 0) {
      total += 1;
    }
  }
  return total;
}
