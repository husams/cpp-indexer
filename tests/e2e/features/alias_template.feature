@e2e
Feature: Indexing aliases whose target is a class-template instantiation
  I index a class template and two aliases that name concrete instantiations
  of it. Naming `Box<int>` in an alias is what makes the concrete record
  exist in the index: the alias mints the instance and links to it with an
  `alias_of` edge, and the instance links to the pattern with `instantiates`.
  Starting from the alias alone -- by qualified name, without touching a
  USR -- a client must reach the concrete instance, its bound template
  argument, its substituted members, the pattern, and the calls that land on
  the instance: IntBox -> Box<int> -> Box<T>.

  The two aliases differ on purpose. `readBox` calls a member through
  IntBox, which forces the real implicit instantiation of Box<int>
  (is_instantiation = true, substituted members minted). RealBox only names
  Box<double> and nothing ever requires the complete type, so Box<double>
  stays a minted stub record (is_instantiation = false, no members) whose
  `instantiates` edge still ties it to the pattern.

  Fixture: fixtures/alias_template.cpp

       8  template <class T> class Box { ... };   pattern (lines 8-15)
      17  using IntBox = Box<int>;
      19  typedef Box<double> RealBox;
      21  int readBox(const IntBox &box) { return box.get(); }

  Background:
    Given a clean index workspace for fixture "alias_template.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the CLI output contains "index: 1 indexed, 0 failed"

  Scenario: The pattern, both aliases, and both minted instances are captured
    # Box<int> was really instantiated (readBox needs the complete type to
    # call get), so it carries is_instantiation and its substituted members
    # item/get exist. Box<double> is only NAMED by the typedef: it is a stub
    # (is_instantiation false, located at the pattern's declaration) with no
    # members. readBox's USR
    # (`...#&1$@S@Box>#I#`) canonicalizes IntBox away -- the alias survives
    # only in type_info, the signature slots, and the `uses` edge.
    Then the index holds exactly these symbols:
      | usr                            | spelling | qual_name               | kind           | type_info            | line | col | end_line | end_col | access  | is_definition | is_instantiation |
      | c:@ST>1#T@Box                  | Box      | Box<T>                  | class-template | -                    | 8    | 1   | 15       | 2       | -       | true          | false            |
      | c:@ST>1#T@Box@FI@item          | item     | Box<T>::item            | member         | T                    | 10   | 3   | 10       | 9       | private | true          | false            |
      | c:@ST>1#T@Box@F@Box#t0.0#      | Box<T>   | Box<T>::Box(T)          | constructor    | void (T)             | 13   | 3   | 13       | 41      | public  | true          | false            |
      | c:@ST>1#T@Box@F@get#1          | get      | Box<T>::get() const     | method         | T () const           | 14   | 3   | 14       | 33      | public  | true          | false            |
      | c:@IntBox                      | IntBox   | IntBox                  | type-alias     | Box<int>             | 17   | 1   | 17       | 24      | -       | true          | false            |
      | c:alias_template.cpp@T@RealBox | RealBox  | RealBox                 | typedef        | Box<double>          | 19   | 1   | 19       | 28      | -       | true          | false            |
      | c:@F@readBox#&1$@S@Box>#I#     | readBox  | readBox(const IntBox &) | function       | int (const IntBox &) | 21   | 1   | 21       | 53      | -       | true          | false            |
      | c:@S@Box>#I                    | Box      | Box<int>                | class          | Box<int>             | 9    | 7   | -        | -       | -       | false         | true             |
      | c:@S@Box>#I@FI@item            | item     | Box<int>::item          | member         | int                  | 10   | 5   | -        | -       | -       | false         | false            |
      | c:@S@Box>#d                    | Box      | Box<double>             | class          | Box<double>          | 9    | 7   | -        | -       | -       | false         | false            |
      | c:@S@Box>#I@F@get#1            | get      | Box<int>::get() const   | method         | int () const         | 14   | 5   | -        | -       | -       | false         | false            |

  Scenario: Each alias records its concrete instantiation as the underlying type
    Then symbol "IntBox" has underlying type "Box<int>"
    And the underlying type of "IntBox" is declared by "Box<int>"
    And symbol "RealBox" has underlying type "Box<double>"
    And the underlying type of "RealBox" is declared by "Box<double>"
    And the type "const IntBox" canonicalizes to "const Box<int>"

  Scenario: The instance reached from the alias leads to the pattern and its arguments
    # Two hops by qualified name: IntBox's underlying type is declared by
    # Box<int>; Box<int> instantiates Box<T> and binds int. The stub
    # Box<double> still binds double and still points at the pattern through
    # its `instantiates` edge, but it does not count as a real instantiation,
    # so the pattern reports exactly one.
    Then symbol "Box<T>" declares the template parameters:
      | position | name | kind |
      | 0        | T    | type |
    And symbol "Box<int>" is an instantiation of "Box<T>"
    And symbol "Box<int>" binds the template arguments:
      | position | kind | value |
      | 0        | type | int   |
    And symbol "Box<double>" binds the template arguments:
      | position | kind | value  |
      | 0        | type | double |
    And symbol "Box<T>" has 1 instantiation
    And the template relationships are:
      | symbol      | relationship | target |
      | Box<int>    | instantiates | Box<T> |
      | Box<double> | instantiates | Box<T> |

  Scenario: A call made through the alias lands on the concrete instance member
    # readBox's parameter is spelled with the alias, and its slot names
    # IntBox; the call through it resolves to Box<int>::get() const -- the
    # substituted member of the instance the alias points at, not the
    # dependent pattern member Box<T>::get() const.
    Then callable "readBox(const IntBox &)" has signature slots:
      | role      | position | name | declared_type  | adjusted_type  | mode             | value_kind | named_decl | reference_semantics | default |
      | return    | -        | -    | int            | int            | value            | builtin    | -          | -                   | -       |
      | parameter | 0        | box  | const IntBox & | const IntBox & | lvalue-reference | alias      | IntBox     | ordinary            | -       |
    And symbol "readBox(const IntBox &)" calls:
      | qual_name             | kind   | line |
      | Box<int>::get() const | method | 14   |
    And symbol "Box<int>::get() const" is called by:
      | qual_name               | kind     | line |
      | readBox(const IntBox &) | function | 21   |

  Scenario: Every relationship in the file is accounted for
    # alias -> instance is `alias_of`; instance -> pattern is `instantiates`;
    # both hops carry qualified names on each end. `uses` remains only for
    # references (readBox's parameter naming IntBox, get() reading item).
    Then the edge kind totals are:
      | kind         | total |
      | field_of     | 2     |
      | method_of    | 3     |
      | instantiates | 2     |
      | alias_of     | 2     |
      | uses         | 2     |
      | calls        | 1     |
    And the index holds exactly these edges:
      | src                     | kind         | dst                   | count | sites |
      | Box<T>::item            | field_of     | Box<T>                | 1     | -     |
      | Box<T>::Box(T)          | method_of    | Box<T>                | 1     | -     |
      | Box<T>::get() const     | method_of    | Box<T>                | 1     | -     |
      | Box<T>::get() const     | uses         | Box<T>::item          | 1     | 14:26 |
      | Box<int>                | instantiates | Box<T>                | 1     | -     |
      | Box<int>::item          | field_of     | Box<int>              | 1     | -     |
      | Box<int>::get() const   | method_of    | Box<int>              | 1     | -     |
      | IntBox                  | alias_of     | Box<int>              | 1     | 17:7  |
      | Box<double>             | instantiates | Box<T>                | 1     | -     |
      | RealBox                 | alias_of     | Box<double>           | 1     | 19:21 |
      | readBox(const IntBox &) | uses         | IntBox                | 1     | 21:27 |
      | readBox(const IntBox &) | calls        | Box<int>::get() const | 1     | 21:41 |
