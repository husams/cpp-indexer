// template.h — fixture for S42 template-extraction tests.
//
// Exercises:
//   - A class template with all three template-parameter kinds (AC-S42-1):
//     * type parameter (typename T)
//     * non-type parameter (int N)
//     * template-template parameter (template<typename> class Alloc)
//   - A (partial) specialization for the struct to exercise template_args (AC-S42-2).
#pragma once

// Primary template: three parameter kinds.
template <typename T, int N, template <typename> class Alloc>
struct Container {
    T data[N];
};

// Explicit specialization for T=int, N=4, Alloc=std::allocator — exercises
// get_template_arguments() for SPECIALIZATION nodes.
template <>
struct Container<int, 4, std::allocator> {
    int data[4];
};

// Complex order template use-case
#define LOG_ORDER(order_id, status) do { \
    int temp_status = status; \
    (void)temp_status; \
} while(0)

template <typename T>
struct USATaxCalculator {
    static double calculate(double price) { return price * 0.08; }
};

template <typename P, int Qty, template <typename> class TaxCalc>
class Order {
public:
    Order(P product, double price) : product_(product), price_(price) {}
    double getTotalPrice() const {
        double base = price_ * Qty;
        return base + TaxCalc<P>::calculate(base);
    }
private:
    P product_;
    double price_;
};

struct DigitalProduct {
    int id;
};

// Specialization for DigitalProduct
template <int Qty, template <typename> class TaxCalc>
class Order<DigitalProduct, Qty, TaxCalc> {
public:
    Order(DigitalProduct product, double price) : product_(product), price_(price) {}
    double getTotalPrice() const {
        double base = price_ * Qty;
        return base + TaxCalc<DigitalProduct>::calculate(base);
    }
private:
    DigitalProduct product_;
    double price_;
};

