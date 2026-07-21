struct Calculator {
    template<typename T>
    T add(T a, T b) {
        return a + b;
    }
};


template<>
int Calculator::add<int>(int a, int b) {
    return a + b;
}

double double_number(double a) {
    Calculator calc;
    return calc.add(a, a);
}
