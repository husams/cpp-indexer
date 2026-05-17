// macros.h — macro-heavy + template-heavy fixture for S29 M5 exit gate.
//
// This is a synthetic fixture that exercises:
//   - Object-like macros (ASSERT_EQ, MAX, VERSION_STR).
//   - Function-like macros (MAKE_PAIR, CHECK_NULL).
//   - X-macro .def pattern (included via HANDLE_EVENT macro).
//   - Template class (Container<T>) and function template (clamp<T>).
//   - Macro expansion at function scope → EXPANDS_TO edges.
//
// No system includes; all references stay within-repo so there are
// zero cross_repo_candidate=true edges.
#pragma once

// --- Object-like macros ---

/// Version constant (object-like).
#define VERSION_MAJOR 1
#define VERSION_MINOR 2
#define VERSION_STR "1.2"

/// Common assertion helper (object-like).
#define ASSERT_TRUE(x) ((void)(x))

/// Numeric helpers.
#define MAX_VALUE 1024
#define MIN_VALUE 0

// --- Function-like macros ---

/// Wrap two values into a trivial pair-struct initialiser.
#define MAKE_PAIR(a, b) \
    { (a), (b) }

/// Null-check wrapper with a no-op body for fixture purposes.
#define CHECK_NULL(ptr) \
    do { (void)(ptr); } while (0)

/// Clamp helper macro (exercises nested expansion).
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

// --- X-macro definition ---
// Callers #define HANDLE_EVENT, #include "macros.h" again, then #undef.
// Here we provide the list as a second inclusion guard block.
#ifndef HANDLE_EVENT
#   define HANDLE_EVENT(name, id)  /* no-op default */
#endif
HANDLE_EVENT(ClickEvent, 1)
HANDLE_EVENT(KeyEvent, 2)
HANDLE_EVENT(ResizeEvent, 3)
#undef HANDLE_EVENT

// --- Template class ---

/// Primary class template: Container<T>.
/// Maps to NodeKind::TemplateDef via EntityKind::ClassTemplate.
template <typename T>
class Container {
public:
    Container() : size_(0) {}
    explicit Container(const T& val) : val_(val), size_(1) {}

    bool empty() const { return size_ == 0; }
    int size() const { return size_; }
    const T& value() const { return val_; }

private:
    T   val_;
    int size_;
};

/// Partial specialisation: Container<T*> (pointer variant).
/// Maps to NodeKind::Specialization + SPECIALIZES edge to Container<T>.
template <typename T>
class Container<T*> {
public:
    Container() : ptr_(nullptr) {}
    explicit Container(T* ptr) : ptr_(ptr) {}

    bool empty() const { return ptr_ == nullptr; }
    T*   value() const { return ptr_; }

private:
    T* ptr_;
};

// --- Function template ---

/// Clamp a value between lo and hi (function-template form).
template <typename T>
T clamp(T v, T lo, T hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
