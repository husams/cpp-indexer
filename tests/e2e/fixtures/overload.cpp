int verify(int x, int y) {
  return x < y;
}

int verify(int x, int y, int z) {
  return x < y && y < z;
}

int verify(double x, double y) {
  return x < y;
}

struct Range {
  int contains(int value) {
    return low <= value && value <= high;
  }

  int contains(double value) {
    return low <= value && value <= high;
  }

  int low;
  int high;
};

int check() {
    int a = 1, b = 2, c = 3;
    double d = 1.0, e = 2.0;

    if (verify(a, b)) {
        return 0;
    }

    if (verify(a, b, c)) {
        return 0;
    }

    if (verify(d, e)) {
        return 0;
    }

    Range r{0, 10};
    if (r.contains(a)) {
        return 0;
    }

    if (r.contains(d)) {
        return 0;
    }

    return -1;
}
