@e2e
Feature: Capturing standard-library symbols referenced by user code
  I index a single translation unit that uses `std::string` and `std::vector`
  from the C++ standard library. System headers are never indexed themselves:
  only the three project functions become indexed symbols. Every standard-library
  entity the user code touches -- `std::vector<int>`, its constructors and
  `push_back`, `std::string` and its `size()`, and the free `operator+`
  overload -- is minted as an *unresolved stub* symbol keyed by its real USR,
  so the call and construction edges from project code still land on precise,
  fully-typed targets.

  Stub names show the alias users actually write, not the library's internal
  identity: the inline namespace (libc++'s `std::__1`) is transparent,
  template arguments that only restate defaults are dropped
  (`std::vector<int>`, never `std::vector<int, std::allocator<int>>`), and a
  library-declared preferred name wins outright (`std::string`, never
  `std::basic_string<char, std::char_traits<char>, std::allocator<char>>`).
  The USR still records the full canonical identity, so nothing collapses.
  Implicit instantiations demanded by the user code are marked as
  instantiations and bind their complete template arguments -- including the
  defaulted allocator/traits -- even though their patterns live in unindexed
  system headers.

  Stub locations point into the toolchain's own headers (e.g. libc++'s
  <string> and <vector>), so their file/line values track the installed SDK
  and are deliberately not pinned here. Every edge *site* is anchored in the
  fixture's own source text and is pinned exactly.

  Fixture: fixtures/std_library.cpp

       4  std::string greet(const std::string& name)     calls operator+ (line 5)
       8  std::vector<int> makeNumbers()                  default-ctor (9), push_back x2 (10, 11), move-ctor (12)
      15  int totalLength(const std::vector<std::string>&)  calls std::string::size (line 18)

  Background:
    Given a clean index workspace for fixture "std_library.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline indexes only the project file and skips system headers
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the CLI output contains "index: 1 indexed, 0 failed"
    And the CLI output contains "headers: 0 indexed (+0 symbols)"
    And the index holds exactly 10 symbols
    And the index holds exactly 11 edges

  Scenario: The project functions are indexed with std types in their signatures
    Then the index holds the symbols:
      | spelling    | qual_name                                     | kind     | type_info                              | file            | line | col | end_line | end_col | is_definition | is_stub |
      | greet       | greet(const std::string &)                    | function | std::string (const std::string &)      | std_library.cpp | 4    | 1   | 6        | 2       | true          | false   |
      | makeNumbers | makeNumbers()                                 | function | std::vector<int> ()                    | std_library.cpp | 8    | 1   | 13       | 2       | true          | false   |
      | totalLength | totalLength(const std::vector<std::string> &) | function | int (const std::vector<std::string> &) | std_library.cpp | 15   | 1   | 21       | 2       | true          | false   |

  Scenario: Every referenced standard-library entity is captured as an unresolved stub under its user-facing alias
    # The `spelling` keeps the declaration's real name (`basic_string`), while
    # `qual_name` is the alias users write. Sugared member-signature spellings
    # like `value_type` survive as written in the library.
    Then the index holds exactly 7 unresolved symbols
    And the index holds the symbols:
      | spelling     | qual_name                                                  | kind        | type_info                                                                 | is_definition | is_instantiation | is_stub |
      | operator+    | std::operator+<>(const value_type *, const std::string &) | function    | std::string (const value_type *, const std::string &)                    | false         | true             | true    |
      | vector       | std::vector<int>                                           | class       | std::vector<int>                                                          | false         | true             | true    |
      | vector       | std::vector<int>::vector()                                 | constructor | void () noexcept(is_nothrow_default_constructible<allocator_type>::value) | false         | false            | true    |
      | vector       | std::vector<int>::vector(vector<int> &&)                   | constructor | void (vector<int> &&) noexcept                                            | false         | false            | true    |
      | push_back    | std::vector<int>::push_back(value_type &&)                 | method      | void (value_type &&)                                                      | false         | false            | true    |
      | basic_string | std::string                                                | class       | std::string                                                               | false         | true             | true    |
      | size         | std::string::size() const                                  | method      | size_type () const noexcept                                               | false         | false            | true    |

  Scenario: The stub instantiations still bind their complete template arguments
    # The alias hides defaulted arguments from the display name only; the
    # template-argument tier keeps the full concrete binding.
    Then symbol "std::vector<int>" binds the template arguments:
      | position | kind | value               |
      | 0        | type | int                 |
      | 1        | type | std::allocator<int> |
    And symbol "std::string" binds the template arguments:
      | position | kind | value                  |
      | 0        | type | char                   |
      | 1        | type | std::char_traits<char> |
      | 2        | type | std::allocator<char>   |

  Scenario: Every relationship between project code and the library is accounted for
    Then the edge kind totals are:
      | kind            | total |
      | calls           | 5     |
      | method_of       | 4     |
      | construct-value | 1     |
      | construct-move  | 1     |
    And the index holds exactly these edges:
      | src                                           | kind            | dst                                                        | count | sites     |
      | greet(const std::string &)                    | calls           | std::operator+<>(const value_type *, const std::string &) | 1     | 5:12      |
      | makeNumbers()                                 | calls           | std::vector<int>::vector()                                 | 1     | 9:22      |
      | makeNumbers()                                 | construct-value | std::vector<int>                                           | 1     | -         |
      | makeNumbers()                                 | calls           | std::vector<int>::push_back(value_type &&)                 | 2     | 10:5,11:5 |
      | makeNumbers()                                 | calls           | std::vector<int>::vector(vector<int> &&)                   | 1     | 12:12     |
      | makeNumbers()                                 | construct-move  | std::vector<int>                                           | 1     | -         |
      | totalLength(const std::vector<std::string> &) | calls           | std::string::size() const                                  | 1     | 18:35     |
      | std::vector<int>::vector()                    | method_of       | std::vector<int>                                           | 1     | -         |
      | std::vector<int>::vector(vector<int> &&)      | method_of       | std::vector<int>                                           | 1     | -         |
      | std::vector<int>::push_back(value_type &&)    | method_of       | std::vector<int>                                           | 1     | -         |
      | std::string::size() const                     | method_of       | std::string                                                | 1     | -         |

  Scenario: Every call into the library is anchored in the fixture's own source
    # `token` is read back from the fixture on disk, so each site is proven
    # against real source text: default construction anchors at the declared
    # variable, member calls at their receiver, operator+ at its left operand.
    Then the "calls" edge sites are:
      | src                                           | dst                                                        | file            | line | col | token    |
      | greet(const std::string &)                    | std::operator+<>(const value_type *, const std::string &) | std_library.cpp | 5    | 12  | "hello,  |
      | makeNumbers()                                 | std::vector<int>::vector()                                 | std_library.cpp | 9    | 22  | numbers  |
      | makeNumbers()                                 | std::vector<int>::push_back(value_type &&)                 | std_library.cpp | 10   | 5   | numbers  |
      | makeNumbers()                                 | std::vector<int>::push_back(value_type &&)                 | std_library.cpp | 11   | 5   | numbers  |
      | makeNumbers()                                 | std::vector<int>::vector(vector<int> &&)                   | std_library.cpp | 12   | 12  | numbers  |
      | totalLength(const std::vector<std::string> &) | std::string::size() const                                  | std_library.cpp | 18   | 35  | word     |

  Scenario: The call graph crosses the project/library boundary in both directions
    Then symbol "makeNumbers" calls:
      | qual_name                                  | kind        |
      | std::vector<int>::vector()                 | constructor |
      | std::vector<int>::vector(vector<int> &&)   | constructor |
      | std::vector<int>::push_back(value_type &&) | method      |
    And symbol "std::vector<int>::push_back(value_type &&)" is called by:
      | qual_name     | kind     | line |
      | makeNumbers() | function | 8    |
    And symbol "std::string::size() const" is called by:
      | qual_name                                     | kind     | line |
      | totalLength(const std::vector<std::string> &) | function | 15   |
