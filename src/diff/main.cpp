// cidx-diff entry point — syntax-aware and semantic source diff over the
// cidx index (docs/diff.md). Thin wrapper: diff::run() is the tool; this
// catch block is the safety net for anything escaping it (exit codes mirror
// cidx main.cpp: usage=2, any other error=1).
#include <iostream>
#include <string>
#include <vector>

#include "diff/driver.hpp"
#include "util/errors.hpp"

int main(int argc, char **argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  try {
    return cidx::diff::run(args, std::cout, std::cerr);
  } catch (const cidx::UsageError &e) {
    std::cerr << e.what();
    return e.exit_code();
  } catch (const cidx::CidxError &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  } catch (...) {
    return 1;
  }
}
