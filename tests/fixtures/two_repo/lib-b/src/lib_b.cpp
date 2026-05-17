#include "lib_b.h"

/// Definition of compute — lives in lib-b.
/// lib-a calls this function; Phase 5 should materialise an EXTERNAL_REF edge.
int compute(int x) {
    return x * 2;
}
