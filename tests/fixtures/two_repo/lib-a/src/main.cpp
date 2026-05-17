/// main.cpp — lib-a entry point.
///
/// Calls compute() from lib-b.  The compile command does NOT add lib-b's include
/// directory so the declaration is written inline here.  This ensures lib-a's
/// Phase 1 pass emits compute's USR only as an edge target (not as a node
/// definition), causing Phase 3 to flag the CALLS edge as cross_repo_candidate.
extern int compute(int x);

int main() {
    return compute(21) - 42;
}
