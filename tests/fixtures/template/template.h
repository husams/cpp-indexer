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
