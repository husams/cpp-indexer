// boost/optional.hpp — minimal vendored fixture for S16 M2 exit gate.
//
// This is NOT a real Boost header. It contains just enough C++ to exercise
// the template-definition + partial-specialization path that the Phase 1
// visitor uses to emit TEMPLATE_DECL, SPECIALIZATION, and SPECIALIZES edges.
//
// Design constraints:
//   - No system includes (avoids cross_repo_candidate=true edges).
//   - Defines primary template boost::optional<T>.
//   - Defines partial specialization boost::optional<T*> (pointer variant).
//   - Both definitions are self-contained in this single header.
#pragma once

namespace boost {

/// Primary class template: boost::optional<T>.
/// Maps to NodeKind::TemplateDef via EntityKind::ClassTemplate.
template <typename T>
class optional {
public:
    optional() : has_value_(false) {}
    explicit optional(const T& value) : value_(value), has_value_(true) {}

    bool has_value() const { return has_value_; }

    const T& get() const { return value_; }
    T& get() { return value_; }

private:
    T    value_;
    bool has_value_;
};

/// Partial specialization: boost::optional<T*> (pointer variant).
/// Maps to NodeKind::Specialization via EntityKind::ClassTemplatePartialSpecialization.
/// The visitor emits a SPECIALIZES edge from this node to the primary template above.
template <typename T>
class optional<T*> {
public:
    optional() : ptr_(nullptr) {}
    explicit optional(T* ptr) : ptr_(ptr) {}

    bool has_value() const { return ptr_ != nullptr; }

    T* get() const { return ptr_; }

private:
    T* ptr_;
};

} // namespace boost
