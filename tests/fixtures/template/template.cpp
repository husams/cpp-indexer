// template.cpp — fixture implementation for S42 template-extraction tests.
//
// Includes template.h so that the TU contains the template declaration and
// its explicit specialization.
#include <memory>
#include "template.h"

// Force instantiation of a regular (implicit) specialization so that
// SPECIALIZATION nodes appear in the AST.
Container<double, 8, std::allocator> g_container;
