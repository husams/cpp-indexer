@e2e
Feature: Indexing global variables declared through type aliases
  I index three global variables whose types are written as aliases: a
  builtin alias (Meters = double), a record alias (Location = Point), and a
  class-template-instantiation alias (PointPair = Pair<Point>). Each
  variable must keep the WRITTEN alias spelling as its declared type and
  hold an `of_type` edge to the alias declaration -- never a shortcut to the
  canonical type -- while the alias itself hands the next hop onward through
  its own `alias_of` edge. Both chains stay walkable with qualified names
  alone:
  distance -> Meters -> double, and
  segment -> PointPair -> Pair<Point> -> Pair<T>.

  Defining `segment` requires the complete type, so the variable definition
  is what forces the implicit instantiation of Pair<Point> with its
  substituted members.

  Fixture: fixtures/alias_variables.cpp

       8  struct Point { double x; double y; };       (lines 8-11)
      13  template <class T> struct Pair { ... };     pattern (lines 13-17)
      19  using Meters = double;
      21  using Location = Point;
      23  using PointPair = Pair<Point>;
      25  Meters distance = 12.5;
      27  Location origin{0.0, 0.0};
      29  PointPair segment{};

  Background:
    Given a clean index workspace for fixture "alias_variables.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the CLI output contains "index: 1 indexed, 0 failed"

  Scenario: The variables, the aliases, and the forced instantiation are captured
    # Every variable's type_info is the alias spelling as written, not the
    # canonical type. Pair<Point> exists because segment's definition needs
    # the complete type; its members carry the substituted type Point.
    Then the index holds exactly these symbols:
      | usr                           | spelling  | qual_name           | kind           | type_info   | line | col | end_line | end_col | access | is_definition | is_instantiation |
      | c:@S@Point                    | Point     | Point               | struct         | Point       | 8    | 1   | 11       | 2       | -      | true          | false            |
      | c:@S@Point@FI@x               | x         | Point::x            | member         | double      | 9    | 3   | 9        | 11      | public | true          | false            |
      | c:@S@Point@FI@y               | y         | Point::y            | member         | double      | 10   | 3   | 10       | 11      | public | true          | false            |
      | c:@ST>1#T@Pair                | Pair      | Pair<T>             | class-template | -           | 13   | 1   | 17       | 2       | -      | true          | false            |
      | c:@ST>1#T@Pair@FI@first       | first     | Pair<T>::first      | member         | T           | 15   | 3   | 15       | 10      | public | true          | false            |
      | c:@ST>1#T@Pair@FI@second      | second    | Pair<T>::second     | member         | T           | 16   | 3   | 16       | 11      | public | true          | false            |
      | c:@Meters                     | Meters    | Meters              | type-alias     | double      | 19   | 1   | 19       | 22      | -      | true          | false            |
      | c:@Location                   | Location  | Location            | type-alias     | Point       | 21   | 1   | 21       | 23      | -      | true          | false            |
      | c:@PointPair                  | PointPair | PointPair           | type-alias     | Pair<Point> | 23   | 1   | 23       | 30      | -      | true          | false            |
      | c:@distance                   | distance  | distance            | variable       | Meters      | 25   | 1   | 25       | 23      | -      | true          | false            |
      | c:@origin                     | origin    | origin              | variable       | Location    | 27   | 1   | 27       | 26      | -      | true          | false            |
      | c:@segment                    | segment   | segment             | variable       | PointPair   | 29   | 1   | 29       | 20      | -      | true          | false            |
      | c:@S@Pair>#$@S@Point          | Pair      | Pair<Point>         | struct         | Pair<Point> | 14   | 8   | -        | -       | -      | false         | true             |
      | c:@S@Pair>#$@S@Point@FI@first | first     | Pair<Point>::first  | member         | Point       | 15   | 5   | -        | -       | -      | false         | false            |
      | c:@S@Pair>#$@S@Point@FI@second| second    | Pair<Point>::second | member         | Point       | 16   | 5   | -        | -       | -      | false         | false            |

  Scenario: Each variable keeps the alias spelling as its declared type
    Then symbol "distance" has type "Meters"
    And symbol "origin" has type "Location"
    And symbol "segment" has type "PointPair"

  Scenario: Each alias still leads to the canonical type
    # Meters bottoms out on the builtin double, which has no declaring
    # symbol; Location and PointPair land on named declarations.
    Then symbol "Meters" has underlying type "double"
    And the type "Meters" canonicalizes to "double"
    And the builtin types "double" have no declaring symbol
    And symbol "Location" has underlying type "Point"
    And the underlying type of "Location" is declared by "Point"
    And the type "Location" canonicalizes to "Point"
    And symbol "PointPair" has underlying type "Pair<Point>"
    And the underlying type of "PointPair" is declared by "Pair<Point>"
    And the type "PointPair" canonicalizes to "Pair<Point>"

  Scenario: The template is reachable from the variable's alias
    # segment -> PointPair (of_type) -> Pair<Point> (underlying) ->
    # Pair<T> (instantiates), every hop named.
    Then symbol "Pair<Point>" is an instantiation of "Pair<T>"
    And symbol "Pair<Point>" binds the template arguments:
      | position | kind | value |
      | 0        | type | Point |
    And symbol "Pair<T>" declares the template parameters:
      | position | name | kind |
      | 0        | T    | type |
    And symbol "Pair<T>" has 1 instantiation

  Scenario: Variables are of_type their alias, and each alias is alias_of its target
    # Typed relations, no overloaded "uses" anywhere in this file:
    # distance/origin/segment -> of_type -> their alias (a variable is OF a
    # type; it points at the alias as written, never shortcuts to the
    # canonical type), and Location/PointPair -> alias_of -> the type they
    # name. Meters has no alias_of edge: its target is the builtin double,
    # which has no symbol to point at.
    Then the edge kind totals are:
      | kind         | total |
      | field_of     | 6     |
      | of_type      | 3     |
      | alias_of     | 2     |
      | instantiates | 1     |
    And the index holds exactly these edges:
      | src                 | kind         | dst         | count | sites |
      | Point::x            | field_of     | Point       | 1     | -     |
      | Point::y            | field_of     | Point       | 1     | -     |
      | Pair<T>::first      | field_of     | Pair<T>     | 1     | -     |
      | Pair<T>::second     | field_of     | Pair<T>     | 1     | -     |
      | Pair<Point>::first  | field_of     | Pair<Point> | 1     | -     |
      | Pair<Point>::second | field_of     | Pair<Point> | 1     | -     |
      | Location            | alias_of     | Point       | 1     | 21:7  |
      | PointPair           | alias_of     | Pair<Point> | 1     | 23:7  |
      | Pair<Point>         | instantiates | Pair<T>     | 1     | -     |
      | distance            | of_type      | Meters      | 1     | 25:8  |
      | origin              | of_type      | Location    | 1     | 27:10 |
      | segment             | of_type      | PointPair   | 1     | 29:11 |
