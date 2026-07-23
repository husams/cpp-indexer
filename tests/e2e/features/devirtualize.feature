@e2e
Feature: Receiver provenance devirtualizes the over-approximated dispatch
  The stored graph over-approximates a virtual call: `dispatch_calls` edges
  name every override the call could reach in any calling context. But the
  index also records, per call site, where the receiver came from. In this
  fixture main() passes a concrete local X into Base::doSomething(), whose
  print() call dispatches on that same receiver -- so a context-aware trace
  must prune the candidate set {X::print, Y::print} down to X::print alone,
  by joining the recorded receiver provenance with the selection map at
  query time.

  Fixture: fixtures/devirtualize.cpp

       1  struct Base { int last; virtual void print(int) = 0;
       6    void doSomething(int x) { print(x + 1); } }
      11  struct X : Base   (overrides print)
      17  struct Y : Base   (overrides print, never used)
      23  int main() { X x; x.doSomething(10); return x.last; }

  Background:
    Given a clean index workspace for fixture "devirtualize.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the CLI output contains "index: 1 indexed, 0 failed"
    And the index holds exactly 10 symbols
    And the index holds exactly 0 unresolved symbols

  Scenario: Both subclasses override the pure virtual
    Then the full derived subtree of "Base" is:
      | qual_name | kind   | line |
      | X         | struct | 11   |
      | Y         | struct | 17   |
    And method "Base::print(int)" is overridden by:
      | qual_name     | kind   | line |
      | X::print(int) | method | 12   |
      | Y::print(int) | method | 18   |

  Scenario: The stored graph over-approximates the virtual call
    Then the index holds the edges:
      | src                    | kind           | dst              | sites |
      | main()                 | calls          | Base::doSomething(int) | 25:3 |
      | Base::doSomething(int) | calls          | Base::print(int) | 7:5   |
      | Base::doSomething(int) | dispatch_calls | X::print(int)    | -     |
      | Base::doSomething(int) | dispatch_calls | Y::print(int)    | -     |
    And a virtual call to "Base::print(int)" can land on:
      | qual_name     | kind   | line |
      | X::print(int) | method | 12   |
      | Y::print(int) | method | 18   |

  Scenario: The selection map pairs every candidate with its selecting receiver type
    Then the virtual call to "Base::print(int)" has the selection map:
      | receiver_type | target        |
      | X             | X::print(int) |
      | Y             | Y::print(int) |
    And the virtual call to "Base::print(int)" is prunable

  Scenario: Every call site records where its receiver came from
    Then the call from "main()" to "Base::doSomething(int)" has receiver provenance:
      | kind  | type | decl |
      | local | X    | x    |
    And the call from "Base::doSomething(int)" to "Base::print(int)" has receiver provenance:
      | kind | type | decl |
      | this | Base | Base |
    And the call from "main()" to "X::X()" has receiver provenance:
      | kind | type | decl |
      | this | X    | X    |

  Scenario: Joining provenance with the selection map prunes the trace to the real target
    Then threading the receiver from "main()" through "Base::doSomething(int)", the virtual call to "Base::print(int)" devirtualizes to:
      | qual_name     | kind   | line |
      | X::print(int) | method | 12   |

  @hse_33
  Scenario: HSE-33 AC1 - Default Python QueryPlan traversal remains static
    When I run the static Python QueryPlan from "main()" over calls at depths 1 through 2
    Then the QueryPlan result is complete
    And the QueryPlan result contains:
      | name                   | kind   | line |
      | Base::doSomething(int) | method | 6    |
      | Base::print(int)       | method | 4    |
    And the QueryPlan result excludes:
      | name          |
      | X::print(int) |
      | Y::print(int) |

  @hse_33
  Scenario: HSE-33 AC2 - Python QueryPlan follows the concrete receiver through an inherited body
    When I run the receiver-aware Python QueryPlan from "main()" over calls at depths 1 through 2
    Then receiver-aware traversal is explicit in the canonical QueryPlan
    And the QueryPlan result is complete
    And the QueryPlan result contains:
      | name                   | kind   | line |
      | Base::doSomething(int) | method | 6    |
      | X::print(int)          | method | 12   |
    And the QueryPlan result excludes:
      | name          |
      | Y::print(int) |

  Scenario: Every relationship in the file is accounted for
    Then the index holds exactly 19 edges
    And the edge kind totals are:
      | kind            | total |
      | calls           | 3     |
      | inherits        | 2     |
      | overrides       | 2     |
      | uses            | 4     |
      | field_of        | 1     |
      | method_of       | 4     |
      | construct-value | 1     |
      | dispatch_calls  | 2     |
    And no edge points at an unresolved symbol
