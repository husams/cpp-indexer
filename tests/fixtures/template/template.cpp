// template.cpp — fixture implementation for S42 template-extraction tests.
//
// Includes template.h so that the TU contains the template declaration and
// its explicit specialization.
#include <memory>
#include "template.h"

// Force instantiation of a regular (implicit) specialization so that
// SPECIALIZATION nodes appear in the AST.
Container<double, 8, std::allocator> g_container;

struct Book {
    int id;
};

void process_orders() {
    Book physical_book{101};
    // Force implicit template instantiation of Order with Book, 5, USATaxCalculator
    Order<Book, 5, USATaxCalculator> book_order(physical_book, 20.0);
    double price = book_order.getTotalPrice();

    DigitalProduct pdf_book{202};
    // Force instantiation of specialization for DigitalProduct, 10, USATaxCalculator
    Order<DigitalProduct, 10, USATaxCalculator> digital_order(pdf_book, 15.0);
    double digital_price = digital_order.getTotalPrice();

    LOG_ORDER(101, 1);
}

