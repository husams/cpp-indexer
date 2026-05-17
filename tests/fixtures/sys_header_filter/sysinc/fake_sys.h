// Fake system header — treated as a system header when the parent directory
// is passed via -isystem.  Declares a function and a struct that must NOT
// appear in Parquet output when skip_system_headers = true.

struct SysStruct {
    int value;
};

int sys_function(int x);
