@e2e
Feature: Indexing a class template with an explicit and an implicit instantiation
  I index a class template that is explicitly instantiated for `int` and
  implicitly instantiated for `double` by a user function. I expect three
  a pattern field and callable members, plus callable-member families for the
  explicit `MyClass<int>` record and the implicit `MyClass<double>` record.
  Each instantiated node links back to its pattern with an `instantiates` edge,
  and calls in testMyClass() land on the `double` family.

  Fixture: fixtures/basic_class_template.cpp

       1  template<class T> class MyClass { ... };   pattern (lines 1-9)
      12  template class MyClass<int>;               explicit instantiation
      15  void testMyClass() { MyClass<double> ... } implicit instantiation

  Background:
    Given a clean index workspace for fixture "basic_class_template.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the index holds exactly 16 symbols
    And the index holds exactly 20 edges

  Scenario: The pattern, both instantiated records and their callable members are indexed
    Then the index holds exactly these symbols:
      | usr                                | spelling    | qual_name                        | kind           | type_info        | line | col | end_line | end_col | access  | is_definition | is_instantiation |
      | c:@ST>1#T@MyClass                  | MyClass     | MyClass                          | class-template | -                | 1    | 1   | 9        | 2       | -       | true          | false            |
      | c:@ST>1#T@MyClass@FI@value         | value       | MyClass::value                   | member         | T                | 4    | 5   | 4        | 12      | private | true          | false            |
      | c:@ST>1#T@MyClass@F@MyClass#t0.0#  | MyClass<T>  | MyClass::MyClass<T>(T)           | constructor    | void (T)         | 6    | 5   | 6        | 39      | public  | true          | false            |
      | c:@ST>1#T@MyClass@F@getValue#1     | getValue    | MyClass::getValue() const        | method         | T () const       | 7    | 5   | 7        | 41      | public  | true          | false            |
      | c:@ST>1#T@MyClass@F@setValue#t0.0# | setValue    | MyClass::setValue(T)             | method         | void (T)         | 8    | 5   | 8        | 52      | public  | true          | false            |
      | c:@S@MyClass>#I                    | MyClass     | MyClass<int>                     | class          | MyClass<int>     | 12   | 1   | 12       | 28      | -       | true          | true             |
      | c:@S@MyClass>#I@F@MyClass#I#       | MyClass     | MyClass<int>::MyClass(int)       | constructor    | void (int)       | 12   | 16  | 12       | 16      | public  | true          | false            |
      | c:@S@MyClass>#I@F@getValue#1       | getValue    | MyClass<int>::getValue() const   | method         | int () const     | 12   | 16  | 12       | 16      | public  | true          | false            |
      | c:@S@MyClass>#I@F@setValue#I#      | setValue    | MyClass<int>::setValue(int)      | method         | void (int)       | 12   | 16  | 12       | 16      | public  | true          | false            |
      | c:@S@MyClass>#I@FI@value           | value       | MyClass<int>::value              | member         | int              | 4    | 7   | -        | -       | -       | false         | false            |
      | c:@F@testMyClass#                  | testMyClass | testMyClass()                    | function       | void ()          | 15   | 1   | 19       | 2       | -       | true          | false            |
      | c:@S@MyClass>#d                    | MyClass     | MyClass<double>                  | class          | MyClass<double>  | 2    | 7   | -        | -       | -       | false         | true             |
      | c:@S@MyClass>#d@F@MyClass#d#       | MyClass     | MyClass<double>::MyClass(double) | constructor    | void (double)    | 6    | 5   | -        | -       | -       | false         | false            |
      | c:@S@MyClass>#d@F@getValue#1       | getValue    | MyClass<double>::getValue() const | method        | double () const  | 7    | 7   | -        | -       | -       | false         | false            |
      | c:@S@MyClass>#d@F@setValue#d#      | setValue    | MyClass<double>::setValue(double) | method        | void (double)    | 8    | 10  | -        | -       | -       | false         | false            |
      | c:@S@MyClass>#d@FI@value           | value       | MyClass<double>::value           | member         | double           | 4    | 7   | -        | -       | -       | false         | false            |

  Scenario: The pattern declares one type parameter and is instantiated twice
    Then symbol "usr:c:@ST>1#T@MyClass" declares the template parameters:
      | position | name | kind |
      | 0        | T    | type |
    And symbol "usr:c:@ST>1#T@MyClass" binds no template arguments
    And symbol "usr:c:@ST>1#T@MyClass" has 2 instantiations

  Scenario: Pattern members keep their dependent types
    Then symbol "usr:c:@ST>1#T@MyClass@FI@value" has type "type-parameter-0-0"
    And symbol "usr:c:@ST>1#T@MyClass@F@getValue#1" returns "type-parameter-0-0"
    And symbol "usr:c:@ST>1#T@MyClass@F@setValue#t0.0#" returns "void"
    And symbol "usr:c:@ST>1#T@MyClass@F@setValue#t0.0#" takes the parameters:
      | position | name  | type               |
      | 0        | value | type-parameter-0-0 |

  Scenario: Each instantiated record binds its own template argument
    Then symbol "usr:c:@S@MyClass>#I" binds the template arguments:
      | position | kind | value |
      | 0        | type | int   |
    And symbol "usr:c:@S@MyClass>#I" is an instantiation of "usr:c:@ST>1#T@MyClass"
    And symbol "usr:c:@S@MyClass>#d" binds the template arguments:
      | position | kind | value  |
      | 0        | type | double |
    And symbol "usr:c:@S@MyClass>#d" is an instantiation of "usr:c:@ST>1#T@MyClass"

  Scenario: testMyClass() records which template argument it instantiated
    Then symbol "testMyClass" returns "void"
    And symbol "testMyClass" takes no parameters
    And symbol "testMyClass" spans lines 15 to 19
    And symbol "testMyClass" binds no template arguments

  Scenario: Every relationship in the file is accounted for
    Then the edge kind totals are:
      | kind            | total |
      | method_of       | 9     |
      | field_of        | 3     |
      | instantiates    | 2     |
      | uses            | 2     |
      | calls           | 3     |
      | construct-value | 1     |
    And the index holds exactly these edges:
      | src                                    | kind            | dst                                    | count | sites |
      | usr:c:@ST>1#T@MyClass@FI@value         | field_of        | usr:c:@ST>1#T@MyClass                  | 1     | -     |
      | usr:c:@ST>1#T@MyClass@F@MyClass#t0.0#  | method_of       | usr:c:@ST>1#T@MyClass                  | 1     | -     |
      | usr:c:@ST>1#T@MyClass@F@getValue#1     | method_of       | usr:c:@ST>1#T@MyClass                  | 1     | -     |
      | usr:c:@ST>1#T@MyClass@F@setValue#t0.0# | method_of       | usr:c:@ST>1#T@MyClass                  | 1     | -     |
      | usr:c:@ST>1#T@MyClass@F@getValue#1     | uses            | usr:c:@ST>1#T@MyClass@FI@value         | 1     | 7:33  |
      | usr:c:@ST>1#T@MyClass@F@setValue#t0.0# | uses            | usr:c:@ST>1#T@MyClass@FI@value         | 1     | 8:36  |
      | usr:c:@S@MyClass>#I                    | instantiates    | usr:c:@ST>1#T@MyClass                  | 1     | -     |
      | usr:c:@S@MyClass>#I@F@MyClass#I#       | method_of       | usr:c:@S@MyClass>#I                    | 1     | -     |
      | usr:c:@S@MyClass>#I@F@getValue#1       | method_of       | usr:c:@S@MyClass>#I                    | 1     | -     |
      | usr:c:@S@MyClass>#I@F@setValue#I#      | method_of       | usr:c:@S@MyClass>#I                    | 1     | -     |
      | usr:c:@S@MyClass>#I@FI@value           | field_of        | usr:c:@S@MyClass>#I                    | 1     | -     |
      | usr:c:@S@MyClass>#d                    | instantiates    | usr:c:@ST>1#T@MyClass                  | 1     | -     |
      | usr:c:@S@MyClass>#d@F@MyClass#d#       | method_of       | usr:c:@S@MyClass>#d                    | 1     | -     |
      | usr:c:@S@MyClass>#d@F@getValue#1       | method_of       | usr:c:@S@MyClass>#d                    | 1     | -     |
      | usr:c:@S@MyClass>#d@F@setValue#d#      | method_of       | usr:c:@S@MyClass>#d                    | 1     | -     |
      | usr:c:@S@MyClass>#d@FI@value           | field_of        | usr:c:@S@MyClass>#d                    | 1     | -     |
      | testMyClass                            | calls           | usr:c:@S@MyClass>#d@F@MyClass#d#       | 1     | 16:21 |
      | testMyClass                            | calls           | usr:c:@S@MyClass>#d@F@getValue#1       | 1     | 17:20 |
      | testMyClass                            | calls           | usr:c:@S@MyClass>#d@F@setValue#d#      | 1     | 18:5  |
      | testMyClass                            | construct-value | usr:c:@S@MyClass>#d                    | 1     | -     |

  Scenario: Every `uses` site is anchored at an exact source line and column
    # Sites only, and only `uses` -- no relationship totals, no call sites. A
    # site is WHERE the reference is written, never the declaration line of
    # either endpoint: both rows read the field `value` inside a member body of
    # the pattern, while the field itself is declared on line 4. `token` is read
    # back from the fixture on disk, so line and col are proven against real
    # source text instead of being echoed from the database.
    #
    #   7  |     T getValue() const { return value; }
    #      |                                 ^ col 33
    #   8  |     void setValue(T value) { this->value = value; }
    #      |                                    ^ col 36  (the member, not the parameter)
    Then the "uses" edge sites are:
      | src                                    | dst                            | file                     | line | col | token |
      | usr:c:@ST>1#T@MyClass@F@getValue#1     | usr:c:@ST>1#T@MyClass@FI@value | basic_class_template.cpp | 7    | 33  | value |
      | usr:c:@ST>1#T@MyClass@F@setValue#t0.0# | usr:c:@ST>1#T@MyClass@FI@value | basic_class_template.cpp | 8    | 36  | value |

  Scenario: The fixture is fully resolved -- nothing is left as a minted stub
    # Every USR an edge anchors is declared or defined in the indexed file, so
    # the indexer mints no placeholder for an unknown target.
    Then the index holds exactly 0 unresolved symbols
    And no edge points at an unresolved symbol

  Scenario: The calls land on the double family, never on the int family or the pattern
    Then symbol "testMyClass" calls:
      | qual_name                         | kind        | line |
      | MyClass<double>::MyClass(double)  | constructor | 6    |
      | MyClass<double>::getValue() const | method      | 7    |
      | MyClass<double>::setValue(double) | method      | 8    |
    And symbol "usr:c:@S@MyClass>#d@F@getValue#1" is called by:
      | qual_name     | kind     | line |
      | testMyClass() | function | 15   |
    And symbol "usr:c:@S@MyClass>#I@F@getValue#1" is called by nothing
    And symbol "usr:c:@ST>1#T@MyClass@F@getValue#1" is called by nothing
