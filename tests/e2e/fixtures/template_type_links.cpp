struct Widget {
    int id;
};

enum class Mode { Read, Write };

template <class T>
struct Box {};

template <template <class> class C = Box, class... Ts>
struct Pipeline {};

template <Mode M = Mode::Read>
struct Access {};

template <class T = Widget, int N = 4>
class Buffer {
public:
    T slot{};
    Buffer(T seed = T{}) : slot(seed) {}
    T get(T fallback = T{}) { return fallback; }
    int size() const { return N; }
};

template <class T>
class Buffer<T *, 1> {
public:
    T *get(T *fallback = nullptr) { return fallback; }
};

template <>
class Buffer<Widget, 1> {
public:
    Widget get(Widget fallback = Widget{}) { return fallback; }
};

template <class T>
T identity(T value = T{}) { return value; }

template <>
Widget identity<Widget>(Widget value) { return value; }

struct Holder {
    template <class U>
    U keep(U item = U{}) { return item; }

    template <class U>
    U &&relay(U &&item) { return static_cast<U &&>(item); }

    template <class U>
    void bind(const U *const &item) { (void)item; }
};

template <class T>
T &&forward_value(T &&value) { return static_cast<T &&>(value); }

template <class T>
const T &&exact(const T &&value) { return static_cast<const T &&>(value); }

template <class T>
struct Sink {
    void consume(T &&value) { (void)value; }
};

template <class T>
void pointer_template(
    T value, T *pointer, T **pointer_to_pointer,
    const T *pointer_to_const, T *const const_pointer,
    const T *const const_pointer_to_const,
    T *&pointer_ref, const T *&pointer_to_const_ref,
    T *const &const_pointer_ref,
    const T *const &const_pointer_to_const_ref,
    T *&&pointer_rvalue_ref) {
    (void)value; (void)pointer; (void)pointer_to_pointer;
    (void)pointer_to_const; (void)const_pointer;
    (void)const_pointer_to_const; (void)pointer_ref;
    (void)pointer_to_const_ref; (void)const_pointer_ref;
    (void)const_pointer_to_const_ref; (void)pointer_rvalue_ref;
}

template <>
void pointer_template<Widget>(
    Widget value, Widget *pointer, Widget **pointer_to_pointer,
    const Widget *pointer_to_const, Widget *const const_pointer,
    const Widget *const const_pointer_to_const, Widget *&pointer_ref,
    const Widget *&pointer_to_const_ref, Widget *const &const_pointer_ref,
    const Widget *const &const_pointer_to_const_ref,
    Widget *&&pointer_rvalue_ref) {
    (void)value; (void)pointer; (void)pointer_to_pointer;
    (void)pointer_to_const; (void)const_pointer;
    (void)const_pointer_to_const; (void)pointer_ref;
    (void)pointer_to_const_ref; (void)const_pointer_ref;
    (void)const_pointer_to_const_ref; (void)pointer_rvalue_ref;
}

template <class T>
struct PointerBox {
    PointerBox(T *const &initial) { (void)initial; }
    void reset(T *&slot) { slot = nullptr; }
    void observe(const T *&slot) const { (void)slot; }
};

template <>
struct PointerBox<Widget> {
    PointerBox(Widget *const &initial) { (void)initial; }
    void reset(Widget *&slot) { slot = nullptr; }
    void observe(const Widget *&slot) const { (void)slot; }
};

template <class T, int N>
void array_template(T (&values)[N]) { (void)values; }

template <class T>
void callback_template(T (&callback)(const T &)) { (void)callback; }

Widget callback(const Widget &value) { return value; }

template <class... Ts>
void visit(Ts... values) { (void)sizeof...(values); }

template class Buffer<Widget, 4>;
template double identity<double>(double value);
template void pointer_template<int>(
    int, int *, int **, const int *, int *const, const int *const,
    int *&, const int *&, int *const &, const int *const &, int *&&);

void use() {
    Buffer<int, 2> plain;
    Buffer<Widget *, 1> pointing;
    Buffer<Widget, 1> full;
    Widget w{7};
    Widget same = identity<Widget>(w);
    int integer = identity<int>(1);
    double d = identity<double>(2.0);
    Holder h;
    int kept = h.keep<int>(3);
    h.relay<Widget>(static_cast<Widget &&>(w));
    const int *ci = nullptr;
    h.bind<int>(ci);
    forward_value<Widget &>(w);
    forward_value<Widget>(Widget{});
    double *dp = nullptr;
    double **dpp = &dp;
    const double *cdp = nullptr;
    pointer_template<double>(0.0, dp, dpp, cdp, dp, cdp, dp, cdp, dp, cdp,
                             static_cast<double *&&>(dp));
    exact<int>(1);
    Sink<Widget> sink;
    sink.consume(Widget{});
    int *ptr = nullptr;
    PointerBox<int> ibox(ptr);
    ibox.reset(ptr);
    const int *cint = ptr;
    ibox.observe(cint);
    Widget *wptr = nullptr;
    PointerBox<Widget> wbox(wptr);
    wbox.reset(wptr);
    const Widget *cwidget = wptr;
    wbox.observe(cwidget);
    int values[4]{};
    array_template<int, 4>(values);
    array_template<Widget, 4>(*reinterpret_cast<Widget (*)[4]>(values));
    callback_template<Widget>(callback);
    visit<int, Widget>(1, w);
    Pipeline<Box, Widget, int> pipeline;
    Access<Mode::Write> access;
    (void)plain.size(); (void)plain.get(); (void)pointing.get(nullptr); (void)full.get();
    (void)same; (void)integer; (void)d; (void)kept; (void)pipeline; (void)access; (void)sink;
}
