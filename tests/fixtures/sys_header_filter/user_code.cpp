// User code that includes a (fake) system header.
// When skip_system_headers=true, SysStruct and sys_function must NOT appear
// in Phase 1 output.  UserClass and user_func must always appear.

#include "fake_sys.h"

class UserClass {
public:
    int field;
    void method() {}
};

int user_func(int x) {
    return x + 1;
}
