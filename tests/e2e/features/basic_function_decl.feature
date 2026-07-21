@e2e
Feature: Indexing a plain free function
  As a cidx user
  I index a single translation unit that declares and defines one free
  function with a trailing return type, and I expect the index to describe
  that function completely and to contain nothing else.

  Fixture: fixtures/basic_function_decl.cpp

      1  auto add(int a, int b) -> int {
      2    return a+b;
      3  }

  Background:
    Given a clean index workspace for fixture "basic_function_decl.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the CLI output contains "index: 1 indexed, 0 failed"

  Scenario: add() is the only symbol, and every recorded field is as expected
    Then the index holds exactly 1 symbol
    And the index holds exactly these symbols:
      | usr             | spelling | qual_name | kind     | type_info              | file                     | line | col | end_line | end_col | is_definition | is_instantiation | is_static | is_pure | is_stub |
      | c:@F@add#I#I#   | add      | add       | function | auto (int, int) -> int | basic_function_decl.cpp  | 1    | 1   | 3        | 2       | true          | false            | false     | false   | false   |

  Scenario: The signature tier records the return type and both parameters
    Then symbol "add" returns "int"
    And symbol "add" takes the parameters:
      | position | name | type |
      | 0        | a    | int  |
      | 1        | b    | int  |

  Scenario: add() spans its whole body and has one definition
    Then symbol "add" spans lines 1 to 3
    And symbol "add" has the definitions:
      | file                    | line | end_line | component |
      | basic_function_decl.cpp | 1    | 3        | fixture   |

  Scenario: A leaf function participates in no relationships
    Then the index holds no edges
    And symbol "add" is called by nothing

  Scenario: Local parameters are not promoted to index symbols
    Then the index holds exactly 1 symbol
