int g_counter = 0;
static int s_hidden = 1;
const double kPi = 3.14159;
constexpr int kMaxSize = 1'024;
constexpr unsigned kMask = 0b1010'0101 ^ 0xFFu;
inline constexpr long kShifted = 1L << 20;
constinit int g_startup = kMaxSize / 4;
constexpr auto kCount = 42uz;

consteval int square(int n) { return n * n; }

constexpr int cube(int n) {
  if consteval {
    return n * n * n;
  } else {
    int result = 1;
    for (int i = 0; i < 3; ++i) {
      result *= n;
    }
    return result;
  }
}

constexpr int kSquare = square(5);
constexpr int kCube = cube(3);
constexpr auto kDecay = auto(kMaxSize);
constexpr int kPicked = kMaxSize > 100 ? kSquare : kCube;
