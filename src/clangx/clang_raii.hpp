#pragma once

#include <clang-c/Index.h>

#include <string>

namespace cidx {

class CxString {
public:
  explicit CxString(CXString value) : value_(value) {}
  ~CxString() { ::clang_disposeString(value_); }
  CxString(const CxString &) = delete;
  CxString &operator=(const CxString &) = delete;
  CxString(CxString &&) = delete;
  CxString &operator=(CxString &&) = delete;

  std::string str() const {
    const char *value = ::clang_getCString(value_);
    return value != nullptr ? std::string(value) : std::string();
  }

private:
  CXString value_;
};

class CxOverriddenCursors {
public:
  explicit CxOverriddenCursors(CXCursor cursor) {
    ::clang_getOverriddenCursors(cursor, &cursors_, &size_);
  }
  ~CxOverriddenCursors() {
    if (cursors_ != nullptr) {
      ::clang_disposeOverriddenCursors(cursors_);
    }
  }
  CxOverriddenCursors(const CxOverriddenCursors &) = delete;
  CxOverriddenCursors &operator=(const CxOverriddenCursors &) = delete;
  CxOverriddenCursors(CxOverriddenCursors &&) = delete;
  CxOverriddenCursors &operator=(CxOverriddenCursors &&) = delete;

  unsigned size() const noexcept { return size_; }
  CXCursor operator[](unsigned index) const { return cursors_[index]; }

private:
  CXCursor *cursors_ = nullptr;
  unsigned size_ = 0;
};

} // namespace cidx
