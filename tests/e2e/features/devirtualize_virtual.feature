@e2e @hse_33
Feature: Receiver-aware QueryPlan traversal through an inherited virtual body
  HSE-33 must also preserve the concrete receiver when the inherited outer
  method is itself virtual but has no override. The selected outer body remains
  Base::doSomething(int), while its call on `this` resolves to X::print(int).

  Scenario: HSE-33 AC3 - Python QueryPlan handles a virtual inherited outer method
    Given a clean index workspace for fixture "devirtualize_virtual.cpp"
    When I build the index with the cidx CLI
    And I run the receiver-aware Python QueryPlan from "main()" over calls at depths 1 through 2
    Then receiver-aware traversal is explicit in the canonical QueryPlan
    And the QueryPlan result is complete
    And the QueryPlan result contains:
      | name                   | kind   | line |
      | Base::doSomething(int) | method | 6    |
      | X::print(int)          | method | 12   |
    And the QueryPlan result excludes:
      | name          |
      | Y::print(int) |
