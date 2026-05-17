// Minimal C++20 module interface unit used only by the runtime capability probe.
// This file is embedded into the binary via include_bytes!() and written to a
// temporary file during probe execution.  It must be valid C++20 module syntax.
export module cxg_probe;
export int cxg_probe_value = 1;
