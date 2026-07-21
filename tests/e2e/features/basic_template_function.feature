@e2e
Feature: Indexing a function template, its explicit specialization and its implicit instantiation
  I index a translation unit containing a primary function template, a full
  explicit specialization for `int`, and a call site that forces an implicit
  `double` instantiation. I expect three distinct node kinds -- primary,
  specialization, instantiation -- linked by `specializes` and `instantiates`
  edges, and I expect the call to land on the instantiation node, not on the
  primary template.

  Fixture: fixtures/basic_template_function.cpp

       1  template<class T> T add(T a, T b) { return a + b; }   primary
       6  template<> int add<int>(int a, int b) { ... }         explicit specialization
      11  float call(double a) { auto result = add(a, a); ... } forces add<double>

  Background:
    Given a clean index workspace for fixture "basic_template_function.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the index holds exactly 4 symbols
    And the index holds exactly 3 edges

  Scenario: Primary, specialization, instantiation and caller are all indexed
    Then the index holds exactly these symbols:
      | usr                           | spelling | qual_name | kind              | type_info       | line | col | end_line | end_col | is_definition | is_instantiation |
      | c:@FT@>1#Tadd#t0.0#S0_#S0_#   | add      | add       | function-template | T (T, T)        | 1    | 1   | 4        | 2       | true          | false            |
      | c:@F@add<#I>#I#I#             | add      | add       | function          | int (int, int)  | 6    | 1   | 8        | 2       | true          | false            |
      | c:@F@call#d#                  | call     | call      | function          | float (double)  | 11   | 1   | 14       | 2       | true          | false            |
      | c:@F@add<#d>#d#d#             | add      | add       | function          | -               | 2    | 3   | -        | -       | false         | true             |

  Scenario: The primary template declares one type parameter and no bound arguments
    Then symbol "usr:c:@FT@>1#Tadd#t0.0#S0_#S0_#" declares the template parameters:
      | position | name | kind |
      | 0        | T    | type |
    And symbol "usr:c:@FT@>1#Tadd#t0.0#S0_#S0_#" binds no template arguments
    And symbol "usr:c:@FT@>1#Tadd#t0.0#S0_#S0_#" has 1 instantiation

  Scenario: The explicit int specialization binds int and has a concrete signature
    Then symbol "usr:c:@F@add<#I>#I#I#" binds the template arguments:
      | position | kind | value |
      | 0        | type | int   |
    And symbol "usr:c:@F@add<#I>#I#I#" returns "int"
    And symbol "usr:c:@F@add<#I>#I#I#" takes the parameters:
      | position | name | type |
      | 0        | a    | int  |
      | 1        | b    | int  |

  Scenario: The implicit double instantiation binds double and points at the primary
    Then symbol "usr:c:@F@add<#d>#d#d#" binds the template arguments:
      | position | kind   | value  |
      | 0        | type   | double |
    And symbol "usr:c:@F@add<#d>#d#d#" is an instantiation of "usr:c:@FT@>1#Tadd#t0.0#S0_#S0_#"

  Scenario: call() has a concrete signature of its own
    Then symbol "call" returns "float"
    And symbol "call" takes the parameters:
      | position | name | type   |
      | 0        | a    | double |
    And symbol "call" spans lines 11 to 14

  Scenario: Every relationship in the file is accounted for
    Then the edge kind totals are:
      | kind         | total |
      | specializes  | 1     |
      | instantiates | 1     |
      | calls        | 1     |
    And the index holds exactly these edges:
      | src                             | kind         | dst                                | count | sites |
      | usr:c:@F@add<#I>#I#I#           | specializes  | usr:c:@FT@>1#Tadd#t0.0#S0_#S0_#    | 1     | -     |
      | usr:c:@F@add<#d>#d#d#           | instantiates | usr:c:@FT@>1#Tadd#t0.0#S0_#S0_#    | 1     | -     |
      | call                            | calls        | usr:c:@F@add<#d>#d#d#              | 1     | 12:19 |

  Scenario: The call resolves to the double instantiation, not to the primary or the int specialization
    Then symbol "call" calls:
      | qual_name | kind     | line |
      | add       | function | 2    |
    And symbol "usr:c:@F@add<#d>#d#d#" is called by:
      | qual_name | kind     | line |
      | call      | function | 11   |
    And symbol "usr:c:@F@add<#I>#I#I#" is called by nothing
