@e2e
Feature: Indexing template-heavy units with more than one extraction worker
  B-006. `cidx index` plans more than one extraction worker on any multi-core
  host, and each worker runs a complete Clang front-end invocation. The front
  end's recursion during template instantiation and C++20 constraint
  satisfaction is far deeper than the 512 KiB stack a default-constructed
  worker thread gets on macOS, so the process dies on the stack guard page
  with SIGBUS -- no diagnostic, no report, exit 138.

  This is the end-to-end shape of the defect: the real CLI, the real Clang
  front end, two real translation units. Two units are load-bearing. A
  one-unit run plans one worker and the runner collapses a one-worker plan
  onto the serial path, which parses on the main thread's 8 MiB stack and
  therefore cannot reproduce the bug.

  The serial control scenario is what makes this discriminating: identical
  sources, identical assertions, `--jobs 1`. It passes both before and after
  the fix. If the parallel scenario fails while the control passes, the fault
  is in how the worker was created, not in the sources or the extractor.

  Measured on cidx 0.53.0 (Release, b911463), Darwin 25.1.0 arm64:
    --jobs 2  ->  exit 138 (SIGBUS), no stdout, nothing indexed
    --jobs 1  ->  exit 0, "index: 2 indexed, 0 failed", 28 symbols

  Fixtures: fixtures/deep_templates_a.cpp
            fixtures/deep_templates_b.cpp

  Background:
    Given a clean index workspace for fixtures "deep_templates_a.cpp, deep_templates_b.cpp" compiled as c++20

  Scenario: Two workers index constraint-heavy units without crashing
    When I build the index with the cidx CLI passing "--jobs 2"
    Then the cidx CLI exited cleanly
    And the index database exists
    And the entity graph is resolved
    And the index holds 2 indexed files
    And the CLI output contains "index: 2 indexed, 0 failed"

  Scenario: The parallel run records the same symbols as the serial run
    When I build the index with the cidx CLI passing "--jobs 2"
    Then the index holds the symbols:
      | qual_name                 | kind           |
      | deep                      | namespace      |
      | deep::unwrap()            | function       |
      | deep::Wrapped<N>          | class-template |
      | deep::Peeled<T, false>    | class-template |
      | deep::kDepth              | variable       |
      | other                     | namespace      |
      | other::unwrap()           | function       |
      | other::Wrapped<N>         | class-template |
      | other::Peeled<T, false>   | class-template |
      | other::kDepth             | variable       |

  Scenario: Serial control -- the same corpus on one worker
    When I build the index with the cidx CLI passing "--jobs 1"
    Then the cidx CLI exited cleanly
    And the index holds 2 indexed files
    And the CLI output contains "index: 2 indexed, 0 failed"

  # AC 4238. The previous revision of this scenario ran the default policy with
  # no telemetry flag and asserted only a clean exit, so it could not show that
  # the run wrote its --profile-json output -- which is half of what the
  # criterion claims (review C-1649). Asking for the profile and inspecting it
  # also proves the run really took the parallel path: `parallel.*` counters are
  # emitted only by the parallel runner, so their presence distinguishes "the
  # automatic policy planned more than one worker and survived" from "the
  # automatic policy quietly planned one and took the serial path".
  Scenario: The automatic worker policy writes its telemetry and survives
    When I build the index with the cidx CLI passing "--profile-json profile.json"
    Then the cidx CLI exited cleanly
    And the index holds 2 indexed files
    And the profile JSON "profile.json" exists and parses
    And the profile JSON "profile.json" shows the automatic policy ran more than one worker
    And the profile JSON "profile.json" reports 2 published units
