// Global variables declared through aliases: a builtin alias, a record
// alias, and a class-template-instantiation alias. Each variable keeps the
// written alias spelling as its type, holds a `uses` edge to the alias
// declaration (not to the canonical type), and the alias itself still leads
// to the canonical type -- so distance -> Meters -> double and
// segment -> PointPair -> Pair<Point> -> Pair<T> are both walkable.

struct Point {
  double x;
  double y;
};

template <class T>
struct Pair {
  T first;
  T second;
};

using Meters = double;

using Location = Point;

using PointPair = Pair<Point>;

Meters distance = 12.5;

Location origin{0.0, 0.0};

PointPair segment{};
