template<class T>
T add(T a, T b) {
    return a + b;
}

template<> int add<int>(int a, int b) {
    return a + b + 1; // Specialization for int type
}


float call(double a) {
    auto result = add(a, a);
    return result;
}