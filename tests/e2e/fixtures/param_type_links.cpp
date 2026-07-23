namespace geo {

enum class Unit { Meters, Feet };

struct Point {
    int x;
    int y;
};

using Distance = double;
typedef Point Location;
using Mapper = Point(const Point &, Unit);
extern Mapper default_mapper;

class Ruler {
public:
    Ruler(const Point &origin, Unit unit = Unit::Meters) : origin_(origin), unit_(unit) {}
    Distance measure(const Point &to) const { return to.x - origin_.x; }
    void shift(Point *target = nullptr, int count = 1) { if (target) target->x += count; }
    void shift(Point &target) { target.x += 1; }
    static Distance scale(Distance value, Unit unit = Unit::Meters) { return value; }

private:
    Point origin_;
    Unit unit_;
};

union Payload {
    int count;
    double distance;
};

using MemberMapper = Distance (Ruler::*)(const Point &) const;

void adjust(const Point points[], Mapper mapper, const int limit,
            Point *const cursor) {
    (void)points; (void)mapper; (void)limit; (void)cursor;
}

Distance span(const Point points[], int count = 1, bool absolute = false) {
    return count > 0 && absolute ? points[0].x : 0.0;
}

int unnamed(int = 0, double = 1.0) { return 0; }

const Point &origin() {
    static Point value{0, 0};
    return value;
}

Point *find(Unit unit) { (void)unit; return nullptr; }

void pointer_forms(
    Point value, const Point const_value,
    Point *pointer, Point **pointer_to_pointer,
    const Point *pointer_to_const, Point *const const_pointer,
    const Point *const const_pointer_to_const,
    Point *&pointer_ref, const Point *&pointer_to_const_ref,
    Point *const &const_pointer_ref,
    const Point *const &const_pointer_to_const_ref,
    Point *&&pointer_rvalue_ref) {
    (void)value; (void)const_value; (void)pointer; (void)pointer_to_pointer;
    (void)pointer_to_const; (void)const_pointer; (void)const_pointer_to_const;
    (void)pointer_ref; (void)pointer_to_const_ref; (void)const_pointer_ref;
    (void)const_pointer_to_const_ref; (void)pointer_rvalue_ref;
}

void compound_forms(
    Point (&array_ref)[4], const Point (&const_array_ref)[4],
    Mapper &function_ref, Mapper &&function_rvalue_ref,
    MemberMapper member_function, int Point::*member_data) {
    (void)array_ref; (void)const_array_ref; (void)function_ref;
    (void)function_rvalue_ref; (void)member_function; (void)member_data;
}

void free_reset(Point *&slot) { slot = nullptr; }

void qualified(const volatile Point *const &input,
               Point *__restrict output) {
    (void)input; (void)output;
}

void configure(Location origin = Location{}, Ruler *owner = nullptr,
               Payload payload = Payload{0},
               Mapper &mapper = default_mapper) {
    (void)origin; (void)owner; (void)payload; (void)mapper;
}

void consume(Payload &&payload) { (void)payload; }

void apply(Point (*mapper)(const Point &, Unit) = nullptr) { (void)mapper; }

int sample(int limit, Unit unit = Unit::Meters);
int sample(int limit = 10, Unit unit);
int sample(int limit, Unit unit) { return limit + static_cast<int>(unit == Unit::Feet); }

class Bindings {
public:
    Bindings(Point *const &initial) { (void)initial; }
    void reset(Point *&slot) { slot = nullptr; }
    void observe(const Point *&slot) const { (void)slot; }
    static void consume(Point *&&slot) { (void)slot; }
};

void nothing() {}

void callThem() {
    Point start{0, 0};
    Ruler ruler(start, Unit::Meters);
    Point other{3, 4};
    Distance d = ruler.measure(other);
    ruler.shift(&other, 2);
    ruler.shift(other);
    d = Ruler::scale(d, Unit::Feet);
    d = span(&other, 1, true);
    (void)unnamed(1, 2.0);
    nothing();
    Point *ptr = nullptr;
    Bindings bindings(ptr);
    bindings.reset(ptr);
    const Point *cptr = nullptr;
    bindings.observe(cptr);
    Bindings::consume(static_cast<Point *>(nullptr));
    configure(start, &ruler, Payload{0}, *static_cast<Mapper *>(nullptr));
    apply(nullptr);
    (void)geo::origin();
    (void)find(Unit::Meters);
    (void)sample(10, Unit::Meters);
    (void)d;
}

} // namespace geo
