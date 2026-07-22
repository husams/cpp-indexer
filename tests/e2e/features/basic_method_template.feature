@e2e
Feature: Indexing a member function template inside a struct
  I index a struct holding a member function template, an out-of-line explicit
  specialization of that member for `int`, and a free function that forces an
  implicit `double` instantiation through a member call. I expect the member
  template to be contained by its struct, the specialization to point at the
  member template, and the member call to reach the instantiation node while
  the implicit default constructor is recorded as a declaration-only symbol.

  Fixture: fixtures/basic_method_template.cpp

       1  struct Calculator {                            (lines 1-6)
       2      template<typename T> T add(T a, T b) { ... } member template
       9  template<> int Calculator::add<int>(...)       explicit specialization
      14  double double_number(double a) { Calculator calc; return calc.add(a, a); }

  Background:
    Given a clean index workspace for fixture "basic_method_template.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the index holds exactly 6 symbols
    And the index holds exactly 10 edges

  Scenario: Struct, member template, specialization, caller, implicit ctor and instantiation are all indexed
    Then the index holds exactly these symbols:
      | usr                                          | spelling      | qual_name              | kind              | type_info      | line | col | end_line | end_col | is_definition | is_instantiation |
      | c:@S@Calculator                              | Calculator    | Calculator             | struct            | Calculator     | 1    | 1   | 6        | 2       | true          | false            |
      | c:@S@Calculator@FT@>1#Tadd#t0.0#S0_#S0_#     | add           | Calculator::add(T, T)             | function-template | T (T, T)       | 2    | 5   | 5        | 6       | true          | false            |
      | c:@S@Calculator@F@add<#I>#I#I#               | add           | Calculator::add<int>(int, int)    | method            | int (int, int) | 9    | 1   | 12       | 2       | true          | false            |
      | c:@F@double_number#d#                        | double_number | double_number(double)             | function          | double (double)| 14   | 1   | 17       | 2       | true          | false            |
      | c:@S@Calculator@F@Calculator#                | Calculator    | Calculator::Calculator()          | constructor       | void () noexcept      | 1    | 8   | -        | -       | false         | false            |
      | c:@S@Calculator@F@add<#d>#d#d#               | add           | Calculator::add<>(double, double) | method            | double (double, double)| 3    | 7   | -        | -       | false         | true             |

  Scenario: The member template declares one type parameter and one instantiation
    Then symbol "usr:c:@S@Calculator@FT@>1#Tadd#t0.0#S0_#S0_#" declares the template parameters:
      | position | name | kind |
      | 0        | T    | type |
    And symbol "usr:c:@S@Calculator@FT@>1#Tadd#t0.0#S0_#S0_#" binds no template arguments
    And symbol "usr:c:@S@Calculator@FT@>1#Tadd#t0.0#S0_#S0_#" has 1 instantiation

  Scenario: The out-of-line int specialization binds int and carries a concrete signature
    Then symbol "usr:c:@S@Calculator@F@add<#I>#I#I#" binds the template arguments:
      | position | kind | value |
      | 0        | type | int   |
    And symbol "usr:c:@S@Calculator@F@add<#I>#I#I#" returns "int"
    And symbol "usr:c:@S@Calculator@F@add<#I>#I#I#" takes the parameters:
      | position | name | type |
      | 0        | a    | int  |
      | 1        | b    | int  |
    And symbol "usr:c:@S@Calculator@F@add<#I>#I#I#" spans lines 9 to 12

  Scenario: The implicit double instantiation binds double and points at the member template
    Then symbol "usr:c:@S@Calculator@F@add<#d>#d#d#" binds the template arguments:
      | position | kind | value  |
      | 0        | type | double |
    And symbol "usr:c:@S@Calculator@F@add<#d>#d#d#" is an instantiation of "usr:c:@S@Calculator@FT@>1#Tadd#t0.0#S0_#S0_#"

  Scenario: Every relationship in the file is accounted for
    Then the edge kind totals are:
      | kind            | total |
      | contains        | 1     |
      | method_of       | 3     |
      | specializes     | 1     |
      | instantiates    | 1     |
      | calls           | 2     |
      | uses            | 1     |
      | construct-value | 1     |
    And the index holds exactly these edges:
      | src                                        | kind            | dst                                        | count | sites |
      | Calculator                                 | contains        | usr:c:@S@Calculator@FT@>1#Tadd#t0.0#S0_#S0_# | 1   | -     |
      | usr:c:@S@Calculator@FT@>1#Tadd#t0.0#S0_#S0_# | method_of     | Calculator                                 | 1     | -     |
      | usr:c:@S@Calculator@F@add<#I>#I#I#         | method_of       | Calculator                                 | 1     | -     |
      | usr:c:@S@Calculator@F@add<#d>#d#d#         | method_of       | Calculator                                 | 1     | -     |
      | usr:c:@S@Calculator@F@add<#I>#I#I#         | specializes     | usr:c:@S@Calculator@FT@>1#Tadd#t0.0#S0_#S0_# | 1   | -     |
      | usr:c:@S@Calculator@F@add<#d>#d#d#         | instantiates    | usr:c:@S@Calculator@FT@>1#Tadd#t0.0#S0_#S0_# | 1   | -     |
      | double_number                              | calls           | Calculator::Calculator()                   | 1     | 15:16 |
      | double_number                              | calls           | usr:c:@S@Calculator@F@add<#d>#d#d#         | 1     | 16:12 |
      | double_number                              | uses            | Calculator                                 | 1     | 15:16 |
      | double_number                              | construct-value | Calculator                                 | 1     | -     |

  Scenario: The member call resolves to the double instantiation, not to the int specialization
    Then symbol "double_number" calls:
      | qual_name                         | kind        | line |
      | Calculator::Calculator()          | constructor | 1    |
      | Calculator::add<>(double, double) | method      | 3    |
    And symbol "usr:c:@S@Calculator@F@add<#d>#d#d#" is called by:
      | qual_name             | kind     | line |
      | double_number(double) | function | 14   |
    And symbol "usr:c:@S@Calculator@F@add<#I>#I#I#" is called by nothing

  Scenario: The caller and explicit specialization expose their definition rows
    Then symbol "double_number" has the definitions:
      | file                       | line | end_line | component |
      | basic_method_template.cpp  | 14   | 17       | fixture   |
    And symbol "usr:c:@S@Calculator@F@add<#I>#I#I#" has the definitions:
      | file                       | line | end_line | component |
      | basic_method_template.cpp  | 9    | 12       | fixture   |
