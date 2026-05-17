// main.cpp — AC-M5-4 boundary test fixture.
//
// This TU expands the 20-entry events.def X-macro list exactly once at
// function scope, generating ≤ 20 EXPANDS_TO edges from visit_macro_expansion.
// The TU has 38 source lines (L=38), so the AC-M5-4 bound is 380 edges.
//
// No system headers; all declarations are inline.

// --------------------------------------------------------------------------
// Event id enum — built via X-macro first pass
// --------------------------------------------------------------------------

enum EventId {
#define HANDLE_EVENT(name, id) name = id,
#include "events.def"
    EventId_Count
};

// --------------------------------------------------------------------------
// Dispatch table — built via X-macro second pass
// --------------------------------------------------------------------------

static const char* event_names[] = {
#define HANDLE_EVENT(name, id) #name,
#include "events.def"
    nullptr
};

// --------------------------------------------------------------------------
// Helper that reads from the dispatch table (avoids unused-variable warnings)
// --------------------------------------------------------------------------

const char* get_event_name(int id) {
    if (id < 1 || id > 20) return "unknown";
    return event_names[id - 1];
}
