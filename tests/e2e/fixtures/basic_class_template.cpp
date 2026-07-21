template<class T>
class MyClass {
private:
    T value;
public:
    MyClass(T value) : value(value) {}
    T getValue() const { return value; }
    void setValue(T value) { this->value = value; }
};


template class MyClass<int>;


void testMyClass() {
    MyClass<double> myDouble(42.0);
    double value = myDouble.getValue();
    myDouble.setValue(100.0);
}
