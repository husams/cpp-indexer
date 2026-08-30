@e2e
Feature: Direct single-traversal fact extraction
  The routed extractor emits facts during one rooted AST walk and drains each
  canonical function body once, including nested and template constructs.

  Fixture: fixtures/single_traversal_fact_extraction.cpp

  Background:
    Given a clean index workspace for fixture "single_traversal_fact_extraction.cpp"
    When I build the index with the cidx CLI

  Scenario: The direct extraction pipeline publishes the fixture
    Then the index database exists
    And the entity graph is resolved
    And the CLI output contains "index: 1 indexed, 0 failed"

  Scenario: The fixture records routed declarations and definitions
    Then the index holds the symbols:
      | qual_name                              | kind      |
      | routed_fixture                          | namespace |
      | routed_fixture::nested                  | namespace |
      | routed_fixture::nested::Widget          | struct    |
      | routed_fixture::nested::repeated(int)   | function  |
      | routed_fixture::nested::local_method(int) | function |
