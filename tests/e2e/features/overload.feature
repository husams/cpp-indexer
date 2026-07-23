@e2e
Feature: Indexing overloaded free functions and methods
  I index a translation unit holding an overload set of three free functions
  (differing by arity and by parameter type), a struct with an overloaded
  method pair, and one caller that exercises every overload once. Each
  overload must keep its own identity -- distinct USR, signature-qualified
  name and type -- and every call must resolve to exactly the overload that
  overload resolution picks in the source, never to a sibling.

  Fixture: fixtures/overload.cpp

       1  int verify(int x, int y) { ... }           (lines 1-3)
       5  int verify(int x, int y, int z) { ... }    (lines 5-7)
       9  int verify(double x, double y) { ... }     (lines 9-11)
      13  struct Range {                             (lines 13-24)
      14    int contains(int value) { ... }          (lines 14-16)
      18    int contains(double value) { ... }       (lines 18-20)
      22    int low;  int high;                      (lines 22-23)
      26  int check() { ... }                        (lines 26-52, calls all five)

  Background:
    Given a clean index workspace for fixture "overload.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the CLI output contains "index: 1 indexed, 0 failed"
    And the index holds exactly 0 unresolved symbols

  Scenario: Every symbol in the file is accounted for with its key facts
    Then the index holds exactly 9 symbols
    And the index holds exactly these symbols:
      | usr                       | spelling | qual_name               | kind     | type_info            | file         | line | col | end_line | end_col | access | is_definition | is_instantiation | is_static | is_pure | is_stub |
      | c:@F@verify#I#I#          | verify   | verify(int, int)        | function | int (int, int)       | overload.cpp | 1    | 1   | 3        | 2       | -      | true          | false            | false     | false   | false   |
      | c:@F@verify#I#I#I#        | verify   | verify(int, int, int)   | function | int (int, int, int)  | overload.cpp | 5    | 1   | 7        | 2       | -      | true          | false            | false     | false   | false   |
      | c:@F@verify#d#d#          | verify   | verify(double, double)  | function | int (double, double) | overload.cpp | 9    | 1   | 11       | 2       | -      | true          | false            | false     | false   | false   |
      | c:@S@Range                | Range    | Range                   | struct   | Range                | overload.cpp | 13   | 1   | 24       | 2       | -      | true          | false            | false     | false   | false   |
      | c:@S@Range@F@contains#I#  | contains | Range::contains(int)    | method   | int (int)            | overload.cpp | 14   | 3   | 16       | 4       | public | true          | false            | false     | false   | false   |
      | c:@S@Range@F@contains#d#  | contains | Range::contains(double) | method   | int (double)         | overload.cpp | 18   | 3   | 20       | 4       | public | true          | false            | false     | false   | false   |
      | c:@S@Range@FI@low         | low      | Range::low              | member   | int                  | overload.cpp | 22   | 3   | 22       | 10      | public | true          | false            | false     | false   | false   |
      | c:@S@Range@FI@high        | high     | Range::high             | member   | int                  | overload.cpp | 23   | 3   | 23       | 11      | public | true          | false            | false     | false   | false   |
      | c:@F@check#               | check    | check()                 | function | int ()               | overload.cpp | 26   | 1   | 52       | 2       | -      | true          | false            | false     | false   | false   |

  Scenario: Same-spelling overloads keep distinct identities
    Then the index holds the symbols:
      | usr                      | spelling | qual_name               | type_info            |
      | c:@F@verify#I#I#         | verify   | verify(int, int)        | int (int, int)       |
      | c:@F@verify#I#I#I#       | verify   | verify(int, int, int)   | int (int, int, int)  |
      | c:@F@verify#d#d#         | verify   | verify(double, double)  | int (double, double) |
      | c:@S@Range@F@contains#I# | contains | Range::contains(int)    | int (int)            |
      | c:@S@Range@F@contains#d# | contains | Range::contains(double) | int (double)         |

  Scenario: The signature tier keeps each free-function overload's own parameter list
    Then symbol "verify(int, int)" returns "int"
    And symbol "verify(int, int)" takes the parameters:
      | position | name | type |
      | 0        | x    | int  |
      | 1        | y    | int  |
    And symbol "verify(int, int, int)" returns "int"
    And symbol "verify(int, int, int)" takes the parameters:
      | position | name | type |
      | 0        | x    | int  |
      | 1        | y    | int  |
      | 2        | z    | int  |
    And symbol "verify(double, double)" returns "int"
    And symbol "verify(double, double)" takes the parameters:
      | position | name | type   |
      | 0        | x    | double |
      | 1        | y    | double |

  Scenario: The signature tier keeps each method overload's own parameter list
    Then symbol "Range::contains(int)" returns "int"
    And symbol "Range::contains(int)" takes the parameters:
      | position | name  | type |
      | 0        | value | int  |
    And symbol "Range::contains(double)" returns "int"
    And symbol "Range::contains(double)" takes the parameters:
      | position | name  | type   |
      | 0        | value | double |

  Scenario: Each overload spans only its own definition
    Then symbol "verify(int, int)" spans lines 1 to 3
    And symbol "verify(int, int, int)" spans lines 5 to 7
    And symbol "verify(double, double)" spans lines 9 to 11
    And symbol "Range::contains(int)" has the definitions:
      | file         | line | end_line | component |
      | overload.cpp | 14   | 16       | fixture   |
    And symbol "Range::contains(double)" has the definitions:
      | file         | line | end_line | component |
      | overload.cpp | 18   | 20       | fixture   |

  Scenario: Both method overloads belong to Range, and its fields do too
    Then callable "Range::contains(int)" belongs to "Range" through `method_of`
    And callable "Range::contains(double)" belongs to "Range" through `method_of`
    And the index holds the edges:
      | src         | kind     | dst   |
      | Range::low  | field_of | Range |
      | Range::high | field_of | Range |

  Scenario: Every relationship in the file is accounted for
    Then the index holds exactly 14 edges
    And the edge kind totals are:
      | kind      | total |
      | calls     | 5     |
      | uses      | 5     |
      | field_of  | 2     |
      | method_of | 2     |
    And the index holds exactly these edges:
      | src                     | kind      | dst                     | count | sites |
      | Range::contains(int)    | method_of | Range                   | 1     | -     |
      | Range::contains(double) | method_of | Range                   | 1     | -     |
      | Range::low              | field_of  | Range                   | 1     | -     |
      | Range::high             | field_of  | Range                   | 1     | -     |
      | Range::contains(int)    | uses      | Range::low              | 1     | 15:12 |
      | Range::contains(int)    | uses      | Range::high             | 1     | 15:37 |
      | Range::contains(double) | uses      | Range::low              | 1     | 19:12 |
      | Range::contains(double) | uses      | Range::high             | 1     | 19:37 |
      | check()                 | calls     | verify(int, int)        | 1     | 30:9  |
      | check()                 | calls     | verify(int, int, int)   | 1     | 34:9  |
      | check()                 | calls     | verify(double, double)  | 1     | 38:9  |
      | check()                 | uses      | Range                   | 1     | 42:11 |
      | check()                 | calls     | Range::contains(int)    | 1     | 43:9  |
      | check()                 | calls     | Range::contains(double) | 1     | 47:9  |
    And no edge points at an unresolved symbol

  Scenario: Each call site resolves to exactly the overload the source selects
    Then the "calls" edge sites are:
      | src     | dst                     | file         | line | col | token      |
      | check() | verify(int, int)        | overload.cpp | 30   | 9   | verify(a, b) |
      | check() | verify(int, int, int)   | overload.cpp | 34   | 9   | verify(a, b, c) |
      | check() | verify(double, double)  | overload.cpp | 38   | 9   | verify(d, e) |
      | check() | Range::contains(int)    | overload.cpp | 43   | 9   | r.contains(a) |
      | check() | Range::contains(double) | overload.cpp | 47   | 9   | r.contains(d) |

  Scenario: The caller sees all five overloads as distinct callees
    Then symbol "check()" calls:
      | qual_name               | kind     | line |
      | verify(int, int)        | function | 1    |
      | verify(int, int, int)   | function | 5    |
      | verify(double, double)  | function | 9    |
      | Range::contains(int)    | method   | 14   |
      | Range::contains(double) | method   | 18   |

  Scenario: Each overload is called only through its own signature
    Then symbol "verify(int, int)" is called by:
      | qual_name | kind     | line |
      | check()   | function | 26   |
    And symbol "verify(int, int, int)" is called by:
      | qual_name | kind     | line |
      | check()   | function | 26   |
    And symbol "verify(double, double)" is called by:
      | qual_name | kind     | line |
      | check()   | function | 26   |
    And symbol "Range::contains(int)" is called by:
      | qual_name | kind     | line |
      | check()   | function | 26   |
    And symbol "Range::contains(double)" is called by:
      | qual_name | kind     | line |
      | check()   | function | 26   |
    And symbol "check()" is called by nothing
