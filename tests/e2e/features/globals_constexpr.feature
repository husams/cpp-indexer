@e2e
Feature: Indexing global variables, constants and constexpr initializers
  As a cidx user
  I index one C++23 translation unit made only of namespace-scope variables --
  mutable globals, file-static and const globals, constexpr / constinit /
  inline constexpr constants -- whose initializers exercise C++23 expression
  forms: digit separators, binary and hex literals, the `uz` size literal,
  shift and xor arithmetic, a consteval call, a constexpr call whose body
  branches with `if consteval`, `auto(x)` decay-copy, and the conditional
  operator. I expect every variable and both callables to be described
  completely, and nothing else to be indexed.

  Fixture: fixtures/globals_constexpr.cpp

       1  int g_counter = 0;
       2  static int s_hidden = 1;
       3  const double kPi = 3.14159;
       4  constexpr int kMaxSize = 1'024;
       5  constexpr unsigned kMask = 0b1010'0101 ^ 0xFFu;
       6  inline constexpr long kShifted = 1L << 20;
       7  constinit int g_startup = kMaxSize / 4;
       8  constexpr auto kCount = 42uz;
       9
      10  consteval int square(int n) { return n * n; }
      11
      12  constexpr int cube(int n) {
      13    if consteval {
      14      return n * n * n;
      15    } else {
      16      int result = 1;
      17      for (int i = 0; i < 3; ++i) {
      18        result *= n;
      19      }
      20      return result;
      21    }
      22  }
      23
      24  constexpr int kSquare = square(5);
      25  constexpr int kCube = cube(3);
      26  constexpr auto kDecay = auto(kMaxSize);
      27  constexpr int kPicked = kMaxSize > 100 ? kSquare : kCube;

  Background:
    Given a clean index workspace for fixture "globals_constexpr.cpp" compiled as C++23
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the CLI output contains "index: 1 indexed, 0 failed"

  Scenario: Every global variable and constexpr callable is indexed, and nothing else
    Then the index holds exactly 14 symbols
    And the index holds exactly these symbols:
      | usr                              | spelling  | qual_name   | kind     | type_info          | file                  | line | col | end_line | end_col | is_definition | is_instantiation | is_static | is_pure | is_stub |
      | c:@g_counter                     | g_counter | g_counter   | variable | int                | globals_constexpr.cpp | 1    | 1   | 1        | 18      | true          | false            | false     | false   | false   |
      | c:globals_constexpr.cpp@s_hidden | s_hidden  | s_hidden    | variable | int                | globals_constexpr.cpp | 2    | 1   | 2        | 24      | true          | false            | false     | false   | false   |
      | c:globals_constexpr.cpp@kPi      | kPi       | kPi         | variable | const double       | globals_constexpr.cpp | 3    | 1   | 3        | 27      | true          | false            | false     | false   | false   |
      | c:globals_constexpr.cpp@kMaxSize | kMaxSize  | kMaxSize    | variable | const int          | globals_constexpr.cpp | 4    | 1   | 4        | 31      | true          | false            | false     | false   | false   |
      | c:globals_constexpr.cpp@kMask    | kMask     | kMask       | variable | const unsigned int | globals_constexpr.cpp | 5    | 1   | 5        | 47      | true          | false            | false     | false   | false   |
      | c:@kShifted                      | kShifted  | kShifted    | variable | const long         | globals_constexpr.cpp | 6    | 1   | 6        | 42      | true          | false            | false     | false   | false   |
      | c:@g_startup                     | g_startup | g_startup   | variable | int                | globals_constexpr.cpp | 7    | 1   | 7        | 39      | true          | false            | false     | false   | false   |
      | c:globals_constexpr.cpp@kCount   | kCount    | kCount      | variable | const __size_t     | globals_constexpr.cpp | 8    | 1   | 8        | 29      | true          | false            | false     | false   | false   |
      | c:@F@square#I#                   | square    | square(int) | function | int (int)          | globals_constexpr.cpp | 10   | 1   | 10       | 46      | true          | false            | false     | false   | false   |
      | c:@F@cube#I#                     | cube      | cube(int)   | function | int (int)          | globals_constexpr.cpp | 12   | 1   | 22       | 2       | true          | false            | false     | false   | false   |
      | c:globals_constexpr.cpp@kSquare  | kSquare   | kSquare     | variable | const int          | globals_constexpr.cpp | 24   | 1   | 24       | 34      | true          | false            | false     | false   | false   |
      | c:globals_constexpr.cpp@kCube    | kCube     | kCube       | variable | const int          | globals_constexpr.cpp | 25   | 1   | 25       | 30      | true          | false            | false     | false   | false   |
      | c:globals_constexpr.cpp@kDecay   | kDecay    | kDecay      | variable | const int          | globals_constexpr.cpp | 26   | 1   | 26       | 39      | true          | false            | false     | false   | false   |
      | c:globals_constexpr.cpp@kPicked  | kPicked   | kPicked     | variable | const int          | globals_constexpr.cpp | 27   | 1   | 27       | 57      | true          | false            | false     | false   | false   |

  Scenario: Linkage decides the USR shape of a namespace-scope variable
    # External-linkage globals (plain, constinit, inline constexpr) get a
    # translation-unit-independent USR; internal-linkage ones (static, const,
    # constexpr) are qualified by the file that owns them.
    Then the index holds the symbols:
      | usr                              | spelling  |
      | c:@g_counter                     | g_counter |
      | c:@g_startup                     | g_startup |
      | c:@kShifted                      | kShifted  |
      | c:globals_constexpr.cpp@s_hidden | s_hidden  |
      | c:globals_constexpr.cpp@kPi      | kPi       |
      | c:globals_constexpr.cpp@kMaxSize | kMaxSize  |

  Scenario: Every constant-evaluatable initializer's value is captured
    # Clang's constant evaluator computes the values at index time -- through
    # the consteval call, the `if consteval` constexpr call, the decay-copy
    # and the conditional operator -- and the symbol row records the printed
    # result. The constexpr callables themselves hold no value, and no
    # variable here needs runtime initialization.
    Then the index holds the symbols:
      | spelling  | const_value  |
      | g_counter | 0            |
      | s_hidden  | 1            |
      | kPi       | 3.141590e+00 |
      | kMaxSize  | 1024         |
      | kMask     | 90           |
      | kShifted  | 1048576      |
      | g_startup | 256          |
      | kCount    | 42           |
      | square    | -            |
      | cube      | -            |
      | kSquare   | 25           |
      | kCube     | 27           |
      | kDecay    | 1024         |
      | kPicked   | 25           |

  Scenario: The signature tier records each variable's declared type
    Then symbol "g_counter" has type "int"
    And symbol "s_hidden" has type "int"
    And symbol "kPi" has type "const double"
    And symbol "kMaxSize" has type "const int"
    And symbol "kMask" has type "const unsigned int"
    And symbol "kShifted" has type "const long"
    And symbol "g_startup" has type "int"
    And symbol "kCount" has type "const unsigned long"

  Scenario: constexpr initializer expressions collapse to the variable's declared type
    # square(5), cube(3), auto(kMaxSize) decay-copy and the conditional
    # operator all initialize plain `const int` constants; `auto` deduction
    # from 42uz lands on the platform's size type.
    Then symbol "kSquare" has type "const int"
    And symbol "kCube" has type "const int"
    And symbol "kDecay" has type "const int"
    And symbol "kPicked" has type "const int"
    And symbol "kCount" has type "const unsigned long"

  Scenario: The consteval function is an ordinary callable in the signature tier
    Then symbol "square" returns "int"
    And symbol "square" takes the parameters:
      | position | name | type |
      | 0        | n    | int  |
    And symbol "square" spans lines 10 to 10
    And symbol "square" has the definitions:
      | file                  | line | end_line | component |
      | globals_constexpr.cpp | 10   | 10       | fixture   |

  Scenario: The constexpr function with `if consteval` spans both branches
    Then symbol "cube" returns "int"
    And symbol "cube" takes the parameters:
      | position | name | type |
      | 0        | n    | int  |
    And symbol "cube" spans lines 12 to 22
    And symbol "cube" has the definitions:
      | file                  | line | end_line | component |
      | globals_constexpr.cpp | 12   | 22       | fixture   |

  Scenario: Namespace-scope initializers create no semantic edges
    # A call edge needs an enclosing callable, so square(5) and cube(3) in
    # variable initializers -- evaluated at compile time -- record no edges.
    Then the index holds no edges
    And symbol "square" is called by nothing
    And symbol "cube" is called by nothing

  Scenario: Function-local variables in the constexpr body are not promoted
    # `result` and `i` inside cube()'s runtime branch stay local.
    Then the index holds exactly 14 symbols
