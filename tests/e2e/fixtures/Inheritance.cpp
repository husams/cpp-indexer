struct AbstractBase {
  int x = 10;
  int y = 20;

  virtual int calc() = 0;
};

struct Derived : public AbstractBase {
  int calc() override {
    return x + y;
  }
};

class Derived2 : virtual public Derived {
public:
  int calc() override {
    return x * y;
  }
};

template <typename T>
struct Singleton {
  static T& getInstance() {
    static T instance;
    return instance;
  }
};

struct Cache : public Singleton<Cache> {
  int value = 0;
  int getValue() {
    return value;
  }
};

template <typename T>
struct Cache2 : public Singleton<Cache2<T>> {
  T calc() {
    return T();
  }
};

int useClasses() {
  Derived d;
  Derived2 d2;
  AbstractBase& base = d;

  int total = base.calc();
  total += d2.calc();
  total += Cache::getInstance().getValue();
  total += Cache2<int>::getInstance().calc();
  return total;
}
