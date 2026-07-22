@e2e
Feature: Indexing struct, class and union declarations
  I index a translation unit holding three record kinds -- a POD struct, a
  class with private state plus accessors, and a union -- and I expect every
  record, every field and every member function to be indexed with its access
  specifier, its span, and its containment edge back to the owning record.

  Fixture: fixtures/basic_record_decl.cpp

       1  struct Point { int x; int y; };            (lines 1-4)
       7  class PointClass { ... };                  (lines 7-18)
      20  union PointUnion { int x; int y; };        (lines 20-23)

  Background:
    Given a clean index workspace for fixture "basic_record_decl.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the index holds exactly 15 symbols
    And the index holds exactly 16 edges

  Scenario: The three records are indexed with their kind and full span
    Then the index holds the symbols:
      | usr             | qual_name  | kind   | type_info  | line | end_line | is_definition |
      | c:@S@Point      | Point      | struct | Point      | 1    | 4        | true          |
      | c:@S@PointClass | PointClass | class  | PointClass | 7    | 18       | true          |
      | c:@U@PointUnion | PointUnion | union  | PointUnion | 20   | 23       | true          |

  Scenario: Every symbol in the file is accounted for with its key facts
    Then the index holds exactly these symbols:
      | usr                                | spelling   | qual_name              | kind        | type_info       | file                  | line | col | end_line | end_col | access  | is_definition | is_static |
      | c:@S@Point                         | Point      | Point                  | struct      | Point           | basic_record_decl.cpp | 1    | 1   | 4        | 2       | -       | true          | false     |
      | c:@S@Point@FI@x                    | x          | Point::x               | member      | int             | basic_record_decl.cpp | 2    | 5   | 2        | 10      | public  | true          | false     |
      | c:@S@Point@FI@y                    | y          | Point::y               | member      | int             | basic_record_decl.cpp | 3    | 5   | 3        | 10      | public  | true          | false     |
      | c:@S@PointClass                    | PointClass | PointClass             | class       | PointClass      | basic_record_decl.cpp | 7    | 1   | 18       | 2       | -       | true          | false     |
      | c:@S@PointClass@FI@x               | x          | PointClass::x          | member      | int             | basic_record_decl.cpp | 9    | 5   | 9        | 10      | private | true          | false     |
      | c:@S@PointClass@FI@y               | y          | PointClass::y          | member      | int             | basic_record_decl.cpp | 10   | 5   | 10       | 10      | private | true          | false     |
      | c:@S@PointClass@F@PointClass#I#I#  | PointClass | PointClass::PointClass(int, int) | constructor | void (int, int) | basic_record_decl.cpp | 12   | 5   | 12       | 45      | public  | true          | false     |
      | c:@S@PointClass@F@PointClass#      | PointClass | PointClass::PointClass()         | constructor | void ()         | basic_record_decl.cpp | 13   | 5   | 13       | 33      | public  | true          | false     |
      | c:@S@PointClass@F@getX#1           | getX       | PointClass::getX() const         | method      | int () const    | basic_record_decl.cpp | 14   | 5   | 14       | 35      | public  | true          | false     |
      | c:@S@PointClass@F@getY#1           | getY       | PointClass::getY() const         | method      | int () const    | basic_record_decl.cpp | 15   | 5   | 15       | 35      | public  | true          | false     |
      | c:@S@PointClass@F@setX#I#          | setX       | PointClass::setX(int)            | method      | void (int)      | basic_record_decl.cpp | 16   | 5   | 16       | 38      | public  | true          | false     |
      | c:@S@PointClass@F@setY#I#          | setY       | PointClass::setY(int)            | method      | void (int)      | basic_record_decl.cpp | 17   | 5   | 17       | 38      | public  | true          | false     |
      | c:@U@PointUnion                    | PointUnion | PointUnion             | union       | PointUnion      | basic_record_decl.cpp | 20   | 1   | 23       | 2       | -       | true          | false     |
      | c:@U@PointUnion@FI@x               | x          | PointUnion::x          | member      | int             | basic_record_decl.cpp | 21   | 5   | 21       | 10      | public  | true          | false     |
      | c:@U@PointUnion@FI@y               | y          | PointUnion::y          | member      | int             | basic_record_decl.cpp | 22   | 5   | 22       | 10      | public  | true          | false     |

  Scenario: Field types are recorded by the signature tier
    Then symbol "Point::x" has type "int"
    And symbol "PointClass::y" has type "int"
    And symbol "PointUnion::x" has type "int"

  Scenario: The two overloaded constructors are distinguished by their signature
    Then symbol "PointClass::PointClass(int, int)" takes the parameters:
      | position | name | type |
      | 0        | x    | int  |
      | 1        | y    | int  |
    And symbol "PointClass::PointClass()" takes no parameters

  Scenario: Accessors record their return type and parameters
    Then symbol "PointClass::getX() const" returns "int"
    And symbol "PointClass::getX() const" takes no parameters
    And symbol "PointClass::setX(int)" returns "void"
    And symbol "PointClass::setX(int)" takes the parameters:
      | position | name | type |
      | 0        | x    | int  |

  Scenario: Every relationship in the file is accounted for
    Then the edge kind totals are:
      | kind      | total |
      | field_of  | 6     |
      | method_of | 6     |
      | uses      | 4     |
    And the index holds exactly these edges:
      | src                      | kind      | dst           | count | sites |
      | Point::x                 | field_of  | Point         | 1     | -     |
      | Point::y                 | field_of  | Point         | 1     | -     |
      | PointClass::x            | field_of  | PointClass    | 1     | -     |
      | PointClass::y            | field_of  | PointClass    | 1     | -     |
      | PointUnion::x            | field_of  | PointUnion    | 1     | -     |
      | PointUnion::y            | field_of  | PointUnion    | 1     | -     |
      | PointClass::PointClass(int, int) | method_of | PointClass    | 1     | -     |
      | PointClass::PointClass()         | method_of | PointClass    | 1     | -     |
      | PointClass::getX() const         | method_of | PointClass    | 1     | -     |
      | PointClass::getY() const         | method_of | PointClass    | 1     | -     |
      | PointClass::setX(int)            | method_of | PointClass    | 1     | -     |
      | PointClass::setY(int)            | method_of | PointClass    | 1     | -     |
      | PointClass::getX() const         | uses      | PointClass::x | 1     | 14:31 |
      | PointClass::getY() const         | uses      | PointClass::y | 1     | 15:31 |
      | PointClass::setX(int)            | uses      | PointClass::x | 1     | 16:30 |
      | PointClass::setY(int)            | uses      | PointClass::y | 1     | 17:30 |

  Scenario: Member bodies that only access fields produce no call graph
    Then the index holds no calls edges
    And symbol "PointClass::getX() const" is called by nothing
