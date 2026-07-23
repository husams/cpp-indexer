// Aliases whose target is a class-template instantiation. Naming Box<int>
// in an alias is what makes the implicit instantiation exist: the alias
// mints the concrete Box<int> record, links to it with a `uses` edge, and
// the record links to the pattern with `instantiates`. Starting from the
// alias alone, a client must reach the concrete instance, its substituted
// members, and the pattern: IntBox -> Box<int> -> Box<T>.

template <class T>
class Box {
  T item;

public:
  explicit Box(T value) : item(value) {}
  T get() const { return item; }
};

using IntBox = Box<int>;

typedef Box<double> RealBox;

int readBox(const IntBox &box) { return box.get(); }
