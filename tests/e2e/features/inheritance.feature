@e2e
Feature: Indexing class hierarchies -- inheritance, overrides and dynamic dispatch
  I index a translation unit holding a pure-virtual base with a two-level
  override chain (one link through virtual inheritance), a CRTP singleton
  class template with two instantiations, and one function that exercises
  every class. I expect the inheritance edges, the override chain, the
  dynamic-dispatch target sets and the template instantiation links to be
  recorded exactly.

  Fixture: fixtures/Inheritance.cpp

       1  struct AbstractBase { int x; int y; virtual int calc() = 0; }
       8  struct Derived : public AbstractBase          (overrides calc)
      14  class Derived2 : virtual public Derived       (overrides calc again)
      21  template <typename T> struct Singleton        (CRTP base)
      29  struct Cache : public Singleton<Cache>
      36  template <typename T> struct Cache2 : public Singleton<Cache2<T>>
      43  int useClasses()                              (uses all of the above)

  Background:
    Given a clean index workspace for fixture "Inheritance.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the CLI output contains "index: 1 indexed, 0 failed"
    And the index holds exactly 0 unresolved symbols

  Scenario: Every symbol in the file is accounted for with its key facts
    Then the index holds exactly 24 symbols
    And the index holds exactly these symbols:
      | usr                                          | spelling     | qual_name                             | kind           | type_info        | file            | line | col | end_line | end_col | access | is_definition | is_instantiation | is_static | is_pure | is_stub |
      | c:@S@AbstractBase                            | AbstractBase | AbstractBase                          | struct         | AbstractBase     | Inheritance.cpp | 1    | 1   | 6        | 2       | -      | true          | false            | false     | false   | false   |
      | c:@S@AbstractBase@FI@x                       | x            | AbstractBase::x                       | member         | int              | Inheritance.cpp | 2    | 3   | 2        | 13      | public | true          | false            | false     | false   | false   |
      | c:@S@AbstractBase@FI@y                       | y            | AbstractBase::y                       | member         | int              | Inheritance.cpp | 3    | 3   | 3        | 13      | public | true          | false            | false     | false   | false   |
      | c:@S@AbstractBase@F@calc#                    | calc         | AbstractBase::calc()                  | method         | int ()           | Inheritance.cpp | 5    | 3   | 5        | 25      | public | false         | false            | false     | true    | false   |
      | c:@S@Derived                                 | Derived      | Derived                               | struct         | Derived          | Inheritance.cpp | 8    | 1   | 12       | 2       | -      | true          | false            | false     | false   | false   |
      | c:@S@Derived@F@calc#                         | calc         | Derived::calc()                       | method         | int ()           | Inheritance.cpp | 9    | 3   | 11       | 4       | public | true          | false            | false     | false   | false   |
      | c:@S@Derived2                                | Derived2     | Derived2                              | class          | Derived2         | Inheritance.cpp | 14   | 1   | 19       | 2       | -      | true          | false            | false     | false   | false   |
      | c:@S@Derived2@F@calc#                        | calc         | Derived2::calc()                      | method         | int ()           | Inheritance.cpp | 16   | 3   | 18       | 4       | public | true          | false            | false     | false   | false   |
      | c:@ST>1#T@Singleton                          | Singleton    | Singleton<T>                          | class-template | -                | Inheritance.cpp | 21   | 1   | 27       | 2       | -      | true          | false            | false     | false   | false   |
      | c:@ST>1#T@Singleton@F@getInstance#S          | getInstance  | Singleton<T>::getInstance()           | method         | T &()            | Inheritance.cpp | 23   | 3   | 26       | 4       | public | true          | false            | true      | false   | false   |
      | c:@S@Cache                                   | Cache        | Cache                                 | struct         | Cache            | Inheritance.cpp | 29   | 1   | 34       | 2       | -      | true          | false            | false     | false   | false   |
      | c:@S@Cache@FI@value                          | value        | Cache::value                          | member         | int              | Inheritance.cpp | 30   | 3   | 30       | 16      | public | true          | false            | false     | false   | false   |
      | c:@S@Cache@F@getValue#                       | getValue     | Cache::getValue()                     | method         | int ()           | Inheritance.cpp | 31   | 3   | 33       | 4       | public | true          | false            | false     | false   | false   |
      | c:@ST>1#T@Cache2                             | Cache2       | Cache2<T>                             | class-template | -                | Inheritance.cpp | 36   | 1   | 41       | 2       | -      | true          | false            | false     | false   | false   |
      | c:@ST>1#T@Cache2@F@calc#                     | calc         | Cache2<T>::calc()                     | method         | T ()             | Inheritance.cpp | 38   | 3   | 40       | 4       | public | true          | false            | false     | false   | false   |
      | c:@F@useClasses#                             | useClasses   | useClasses()                          | function       | int ()           | Inheritance.cpp | 43   | 1   | 53       | 2       | -      | true          | false            | false     | false   | false   |
      | c:@S@Singleton>#$@S@Cache                    | Singleton    | Singleton<Cache>                      | struct         | Singleton<Cache> | Inheritance.cpp | 22   | 8   | -        | -       | -      | false         | true             | false     | false   | false   |
      | c:@S@Derived@F@Derived#                      | Derived      | Derived::Derived()                    | constructor    | void () noexcept | Inheritance.cpp | 8    | 8   | -        | -       | -      | false         | false            | false     | false   | false   |
      | c:@S@Derived2@F@Derived2#                    | Derived2     | Derived2::Derived2()                  | constructor    | void () noexcept | Inheritance.cpp | 14   | 7   | -        | -       | -      | false         | false            | false     | false   | false   |
      | c:@S@Singleton>#$@S@Cache@F@getInstance#S    | getInstance  | Singleton<Cache>::getInstance()       | method         | Cache &()        | Inheritance.cpp | 23   | 13  | -        | -       | -      | false         | false            | false     | false   | false   |
      | c:@S@Cache2>#I@F@calc#                       | calc         | Cache2<int>::calc()                   | method         | int ()           | Inheritance.cpp | 38   | 5   | -        | -       | -      | false         | false            | false     | false   | false   |
      | c:@S@Cache2>#I                               | Cache2       | Cache2<int>                           | struct         | Cache2<int>      | Inheritance.cpp | 37   | 8   | -        | -       | -      | false         | true             | false     | false   | false   |
      | c:@S@Singleton>#$@S@Cache2>#I@F@getInstance#S | getInstance | Singleton<Cache2<int>>::getInstance() | method         | Cache2<int> &()  | Inheritance.cpp | 23   | 13  | -        | -       | -      | false         | false            | false     | false   | false   |
      | c:@S@Singleton>#$@S@Cache2>#I                | Singleton    | Singleton<Cache2<int>>                | struct         | Singleton<Cache2<int>> | Inheritance.cpp | 22 | 8 | - | -     | -      | false         | true             | false     | false   | false   |

  Scenario: The class hierarchy is recorded edge by edge
    Then record "AbstractBase" has no bases
    And record "AbstractBase" has the direct subclasses:
      | qual_name | kind   | line |
      | Derived   | struct | 8    |
    And record "Derived" has the direct bases:
      | qual_name    | kind   | line |
      | AbstractBase | struct | 1    |
    And record "Derived2" has the direct bases:
      | qual_name | kind   | line |
      | Derived   | struct | 8    |
    And record "Derived2" has no subclasses
    And the full base hierarchy of "Derived2" is:
      | qual_name    | kind   | line |
      | Derived      | struct | 8    |
      | AbstractBase | struct | 1    |
    And the full derived subtree of "AbstractBase" is:
      | qual_name | kind   | line |
      | Derived   | struct | 8    |
      | Derived2  | class  | 14   |

  Scenario: The CRTP base is an instantiated class template
    Then record "Cache" has the direct bases:
      | qual_name        | kind   | line |
      | Singleton<Cache> | struct | 22   |
    And symbol "Singleton<Cache>" is an instantiation of "Singleton<T>"
    And symbol "Cache2<int>" is an instantiation of "Cache2<T>"
    And symbol "Singleton<T>" has 2 instantiations
    And symbol "Singleton<T>" declares the template parameters:
      | position | name | kind |
      | 0        | T    | type |
    And symbol "Singleton<Cache>" binds the template arguments:
      | position | kind | value |
      | 0        | type | Cache |
    And symbol "Cache2<int>" binds the template arguments:
      | position | kind | value |
      | 0        | type | int   |

  Scenario: The override chain links each calc() to the one it overrides
    Then method "Derived::calc()" overrides:
      | qual_name            | kind   | line |
      | AbstractBase::calc() | method | 5    |
    And method "Derived2::calc()" overrides:
      | qual_name       | kind   | line |
      | Derived::calc() | method | 9    |
    And method "AbstractBase::calc()" is overridden by:
      | qual_name       | kind   | line |
      | Derived::calc() | method | 9    |
    And method "Derived::calc()" is overridden by:
      | qual_name        | kind   | line |
      | Derived2::calc() | method | 16   |
    And method "AbstractBase::calc()" is a virtual method
    And method "Derived2::calc()" is a virtual method
    And method "Cache::getValue()" is not a virtual method

  Scenario: Dynamic dispatch resolves to every concrete override, never the pure root
    Then a virtual call to "AbstractBase::calc()" can land on:
      | qual_name        | kind   | line |
      | Derived::calc()  | method | 9    |
      | Derived2::calc() | method | 16   |
    And a virtual call to "Derived::calc()" can land on:
      | qual_name        | kind   | line |
      | Derived::calc()  | method | 9    |
      | Derived2::calc() | method | 16   |
    And the virtual dispatch points of "useClasses()" are:
      | qual_name            | kind   | line |
      | AbstractBase::calc() | method | 5    |
      | Derived2::calc()     | method | 16   |

  Scenario: The caller reaches every class, and dispatch expands its virtual calls
    Then symbol "useClasses()" calls:
      | qual_name                             | kind        | line |
      | Derived::Derived()                    | constructor | 8    |
      | Derived2::Derived2()                  | constructor | 14   |
      | AbstractBase::calc()                  | method      | 5    |
      | Derived::calc()                       | method      | 9    |
      | Derived2::calc()                      | method      | 16   |
      | Cache::getValue()                     | method      | 31   |
      | Singleton<Cache>::getInstance()       | method      | 23   |
      | Cache2<int>::calc()                   | method      | 38   |
      | Singleton<Cache2<int>>::getInstance() | method      | 23   |
    And symbol "AbstractBase::calc()" is called by:
      | qual_name    | kind     | line |
      | useClasses() | function | 43   |
    And symbol "Derived::calc()" is called by:
      | qual_name    | kind     | line |
      | useClasses() | function | 43   |
    And symbol "Derived2::calc()" is called by:
      | qual_name    | kind     | line |
      | useClasses() | function | 43   |
    And symbol "useClasses()" is called by nothing

  Scenario: Each call is written where the source says it is
    Then the "calls" edge sites are:
      | src          | dst                                   | file            | line | col | token                      |
      | useClasses() | Derived::Derived()                    | Inheritance.cpp | 44   | 11  | d                          |
      | useClasses() | Derived2::Derived2()                  | Inheritance.cpp | 45   | 12  | d2                         |
      | useClasses() | AbstractBase::calc()                  | Inheritance.cpp | 48   | 15  | base.calc()                |
      | useClasses() | Derived2::calc()                      | Inheritance.cpp | 49   | 12  | d2.calc()                  |
      | useClasses() | Cache::getValue()                     | Inheritance.cpp | 50   | 12  | Cache::getInstance()       |
      | useClasses() | Singleton<Cache>::getInstance()       | Inheritance.cpp | 50   | 12  | Cache::getInstance()       |
      | useClasses() | Cache2<int>::calc()                   | Inheritance.cpp | 51   | 12  | Cache2<int>::getInstance() |
      | useClasses() | Singleton<Cache2<int>>::getInstance() | Inheritance.cpp | 51   | 12  | Cache2<int>::getInstance() |

  Scenario: Every relationship in the file is accounted for
    Then the index holds exactly 42 edges
    And the edge kind totals are:
      | kind            | total |
      | calls           | 8     |
      | inherits        | 3     |
      | instantiates    | 3     |
      | overrides       | 2     |
      | uses            | 10    |
      | field_of        | 3     |
      | method_of       | 9     |
      | construct-value | 2     |
      | dispatch_calls  | 2     |
    And the index holds the edges:
      | src                    | kind           | dst                    |
      | Derived                | inherits       | AbstractBase           |
      | Derived2               | inherits       | Derived                |
      | Cache                  | inherits       | Singleton<Cache>       |
      | Singleton<Cache>       | instantiates   | Singleton<T>           |
      | Singleton<Cache2<int>> | instantiates   | Singleton<T>           |
      | Cache2<int>            | instantiates   | Cache2<T>              |
      | Derived::calc()        | overrides      | AbstractBase::calc()   |
      | Derived2::calc()       | overrides      | Derived::calc()        |
      | useClasses()           | dispatch_calls | Derived::calc()        |
      | useClasses()           | dispatch_calls | Derived2::calc()       |
      | useClasses()           | construct-value | Derived               |
      | useClasses()           | construct-value | Derived2              |
    And no edge points at an unresolved symbol

  Scenario: Method bodies read the base-class fields they inherit
    Then the index holds the edges:
      | src              | kind | dst             | sites |
      | Derived::calc()  | uses | AbstractBase::x | 10:12 |
      | Derived::calc()  | uses | AbstractBase::y | 10:16 |
      | Derived2::calc() | uses | AbstractBase::x | 17:12 |
      | Derived2::calc() | uses | AbstractBase::y | 17:16 |
      | Cache::getValue() | uses | Cache::value   | 32:12 |

  Scenario: In-class field initializers are defaults, not constant values
    # `x = 10`, `y = 20` and `value = 0` initialize each new object, so the
    # field itself has no single compile-time value: const_value (v33) stays
    # unset. Only variables and enumerators record one.
    Then the index holds the symbols:
      | qual_name       | kind   | const_value |
      | AbstractBase::x | member | -           |
      | AbstractBase::y | member | -           |
      | Cache::value    | member | -           |
