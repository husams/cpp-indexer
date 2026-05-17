// optional_user.cpp — uses boost::optional<T> and boost::optional<T*>.
//
// This file exercises the fixture so that libclang sees both the primary
// template (TEMPLATE_DECL) and the partial specialization (SPECIALIZATION)
// and emits the SPECIALIZES edge between them.
//
// No system headers are included to keep all references within-repo, which
// ensures zero cross_repo_candidate=true edges (AC-M2-16).
#include "boost/optional.hpp"

// Use the primary template: instantiates boost::optional<int>.
boost::optional<int> make_int_optional(int v) {
    return boost::optional<int>(v);
}

// Use the pointer specialization: exercises boost::optional<T*>.
boost::optional<int*> make_ptr_optional(int* p) {
    return boost::optional<int*>(p);
}
