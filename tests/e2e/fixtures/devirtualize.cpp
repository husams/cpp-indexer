struct Base {
  int last = 0;

  virtual void print(int a) = 0;

  void doSomething(int x) {
    print(x + 1);
  }
};

struct X : Base {
  void print(int a) override {
    last = a;
  }
};

struct Y : Base {
  void print(int a) override {
    last = -a;
  }
};

int main() {
  X x;
  x.doSomething(10);
  return x.last;
}
