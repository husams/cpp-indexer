@e2e @spec
Feature: Callable signature slots preserve types, declarations, qualifiers, and defaults
  Code-navigation clients must not parse rendered C++ signatures. Every free
  function, method, constructor, and overload exposes one ordered signature made
  from typed slots.

  A signature slot has these observable facts:

    * `role` is `return` or `parameter`;
    * return slots have no position, name, default, or forwarding semantics;
    * parameter positions are zero-based and remain present for unnamed params;
    * `declared_type` preserves the source-level type and aliases;
    * `adjusted_type` is the callable type after array/function parameter decay
      and removal of top-level cv-qualification from by-value parameters;
    * `mode` is `value`, `lvalue-reference`, or `rvalue-reference`;
    * `value_kind` describes the value after removing a reference layer, so a
      pointer passed by value is `mode=value, value_kind=pointer`, while a
      reference to a pointer is `mode=lvalue-reference, value_kind=pointer`;
    * `named_decl` is the nearest named class, struct, union, enum, alias, or
      typedef reached through compound layers;
    * `reference_semantics` distinguishes an ordinary reference from template
      forwarding behavior; ordinary callables only use `ordinary` or `-`;
    * `default` is stable source-token text without the `=`.

  A single `const` flag on a parameter would be incorrect. Qualifiers belong to
  individual nodes in the recursive type shape. Each layer separately records
  `const`, `volatile`, and `restrict`, allowing the index to distinguish
  `const T *`, `T * const`, and `const T * const &`.

  Return values use the same signature-slot view as parameters, but they are not
  parameters. Constructors and destructors have no return slot. A return slot
  may itself be by value, lvalue reference, or rvalue reference.

  Default arguments preserve identifiers and macro names. Comments and
  insignificant whitespace are not data. Defaults are not evaluated, folded,
  or rewritten. Effective defaults are merged across visible redeclarations,
  and each default identifies the declaration that introduced it.

  This feature covers built-ins, enums, structs, classes, unions, aliases,
  function types, multi-level and member pointers, references to pointers,
  array/function references, lvalue/rvalue references, recursive qualifiers,
  array/function adjustment, unnamed parameters, overloads, constructors,
  static/const methods, defaults, and redeclarations. Template-only behavior
  is specified in `template_type_links.feature`.

  Fixture: fixtures/param_type_links.cpp (namespace `geo`)

    enum class Unit { Meters, Feet };
    struct Point { int x; int y; };
    class Ruler { ... };
    union Payload { int count; double distance; };
    using Distance = double;
    typedef Point Location;
    using Mapper = Point(const Point &, Unit);
    using MemberMapper = Distance (Ruler::*)(const Point &) const;

    void pointer_forms(
      Point value, const Point const_value,
      Point *pointer, Point **pointer_to_pointer,
      const Point *pointer_to_const, Point *const const_pointer,
      const Point *const const_pointer_to_const,
      Point *&pointer_ref, const Point *&pointer_to_const_ref,
      Point *const &const_pointer_ref,
      const Point *const &const_pointer_to_const_ref,
      Point *&&pointer_rvalue_ref);

    void compound_forms(
      Point (&array_ref)[4], const Point (&const_array_ref)[4],
      Mapper &function_ref, Mapper &&function_rvalue_ref,
      MemberMapper member_function, int Point::*member_data);

    void free_reset(Point *&slot);
    void qualified(const volatile Point *const &input,
                   Point *__restrict output);

    class Bindings {
      Bindings(Point *const &initial);
      void reset(Point *&slot);
      void observe(const Point *&slot) const;
      static void consume(Point *&&slot);
    };

  This is signature/type metadata. It must not create synthetic symbols or
  semantic graph edges. Call-site default selection, expression evaluation,
  overload resolution, attributes, and exception behavior are unchanged.

  Background:
    Given a clean index workspace for fixture "param_type_links.cpp"
    When I build the index with the cidx CLI

  Scenario: The fixture produces a resolved index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file

  Scenario: A free function exposes return and parameter slots together
    # The array parameter decays to a pointer. The alias used for the return
    # type remains visible and links to its alias declaration.
    Then callable "geo::span(const Point *, int, bool)" has signature slots:
      | role      | position | name     | declared_type      | adjusted_type      | mode  | value_kind | named_decl    | reference_semantics | default |
      | return    | -        | -        | Distance           | Distance           | value | alias      | geo::Distance | -                   | -       |
      | parameter | 0        | points   | const geo::Point [] | const geo::Point * | value | pointer    | geo::Point    | -                   | -       |
      | parameter | 1        | count    | int                | int                | value | builtin    | -             | -                   | 1       |
      | parameter | 2        | absolute | bool               | bool               | value | builtin    | -             | -                   | false   |

  Scenario: Unnamed parameters keep their slots and defaults
    Then callable "geo::unnamed(int, double)" has signature slots:
      | role      | position | name | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default |
      | return    | -        | -    | int           | int           | value | builtin    | -          | -                   | -       |
      | parameter | 0        | -    | int           | int           | value | builtin    | -          | -                   | 0       |
      | parameter | 1        | -    | double        | double        | value | builtin    | -          | -                   | 1.0     |

  Scenario: A function without parameters has only a return slot
    Then callable "geo::nothing()" has signature slots:
      | role   | position | name | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default |
      | return | -        | -    | void          | void          | value | builtin    | -          | -                   | -       |
    And callable "geo::nothing()" has no parameter slots

  Scenario: Return slots preserve value and reference forms
    Then callable "geo::origin()" has signature slots:
      | role   | position | name | declared_type     | adjusted_type     | mode             | value_kind | named_decl | reference_semantics | default |
      | return | -        | -    | const geo::Point & | const geo::Point & | lvalue-reference | record     | geo::Point | -                   | -       |
    And callable "geo::find(Unit)" has signature slots:
      | role      | position | name | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default |
      | return    | -        | -    | geo::Point *  | geo::Point *  | value | pointer    | geo::Point | -                   | -       |
      | parameter | 0        | unit | geo::Unit     | geo::Unit     | value | enum       | geo::Unit  | -                   | -       |

  Scenario: A const method uses the same signature-slot model
    Then callable "geo::Ruler::measure(const Point &) const" has signature slots:
      | role      | position | name | declared_type     | adjusted_type     | mode             | value_kind | named_decl    | reference_semantics | default |
      | return    | -        | -    | Distance          | Distance          | value            | alias      | geo::Distance | -                   | -       |
      | parameter | 0        | to   | const geo::Point & | const geo::Point & | lvalue-reference | record     | geo::Point    | ordinary            | -       |

  Scenario: Overloads own independent signature slots
    Then callable "geo::Ruler::shift(Point *, int)" has signature slots:
      | role      | position | name   | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default |
      | return    | -        | -      | void          | void          | value | builtin    | -          | -                   | -       |
      | parameter | 0        | target | geo::Point *  | geo::Point *  | value | pointer    | geo::Point | -                   | nullptr |
      | parameter | 1        | count  | int           | int           | value | builtin    | -          | -                   | 1       |
    And callable "geo::Ruler::shift(Point &)" has signature slots:
      | role      | position | name   | declared_type | adjusted_type | mode             | value_kind | named_decl | reference_semantics | default |
      | return    | -        | -      | void          | void          | value            | builtin    | -          | -                   | -       |
      | parameter | 0        | target | geo::Point &  | geo::Point &  | lvalue-reference | record     | geo::Point | ordinary            | -       |

  Scenario: A constructor has parameter slots and no return slot
    Then callable "geo::Ruler::Ruler(const Point &, Unit)" has no return slot
    And callable "geo::Ruler::Ruler(const Point &, Unit)" has parameter slots:
      | role      | position | name   | declared_type     | adjusted_type     | mode             | value_kind | named_decl | reference_semantics | default      |
      | parameter | 0        | origin | const geo::Point & | const geo::Point & | lvalue-reference | record     | geo::Point | ordinary            | -            |
      | parameter | 1        | unit   | geo::Unit         | geo::Unit         | value            | enum       | geo::Unit  | -                   | Unit::Meters |

  Scenario: A static method preserves aliases and declarations
    Then callable "geo::Ruler::scale(Distance, Unit)" has signature slots:
      | role      | position | name  | declared_type | adjusted_type | mode  | value_kind | named_decl    | reference_semantics | default      |
      | return    | -        | -     | Distance      | Distance      | value | alias      | geo::Distance | -                   | -            |
      | parameter | 0        | value | Distance      | Distance      | value | alias      | geo::Distance | -                   | -            |
      | parameter | 1        | unit  | geo::Unit     | geo::Unit     | value | enum       | geo::Unit     | -                   | Unit::Meters |

  Scenario: Declared and adjusted types are both retained
    # Array and function parameters decay to pointers. Top-level const on a
    # by-value scalar or pointer is removed only from the adjusted type.
    Then callable "geo::adjust(const Point *, Mapper *, int, Point *)" has signature slots:
      | role      | position | name     | declared_type      | adjusted_type      | mode  | value_kind | named_decl | reference_semantics | default |
      | return    | -        | -        | void               | void               | value | builtin    | -          | -                   | -       |
      | parameter | 0        | points   | const geo::Point [] | const geo::Point * | value | pointer    | geo::Point  | -                   | -       |
      | parameter | 1        | mapper   | Mapper              | Mapper *           | value | pointer    | geo::Mapper | -                   | -       |
      | parameter | 2        | limit    | const int          | int                | value | builtin    | -          | -                   | -       |
      | parameter | 3        | cursor   | geo::Point * const | geo::Point *       | value | pointer    | geo::Point | -                   | -       |

  Scenario: Parameters navigate through structs, classes, unions, and aliases
    Then callable "geo::configure(Location, Ruler *, Payload, Mapper &)" has signature slots:
      | role      | position | name    | declared_type | adjusted_type | mode             | value_kind | named_decl     | reference_semantics | default        |
      | return    | -        | -       | void          | void          | value            | builtin    | -              | -                   | -              |
      | parameter | 0        | origin  | Location      | Location      | value            | alias      | geo::Location  | -                   | Location{}     |
      | parameter | 1        | owner   | geo::Ruler *  | geo::Ruler *  | value            | pointer    | geo::Ruler     | -                   | nullptr        |
      | parameter | 2        | payload | geo::Payload  | geo::Payload  | value            | record     | geo::Payload   | -                   | Payload{0}     |
      | parameter | 3        | mapper  | geo::Mapper & | geo::Mapper & | lvalue-reference | alias      | geo::Mapper    | ordinary            | default_mapper |

  Scenario: An ordinary rvalue reference is not a forwarding reference
    Then callable "geo::consume(Payload &&)" has signature slots:
      | role      | position | name    | declared_type  | adjusted_type  | mode             | value_kind | named_decl   | reference_semantics | default |
      | return    | -        | -       | void           | void           | value            | builtin    | -            | -                   | -       |
      | parameter | 0        | payload | geo::Payload && | geo::Payload && | rvalue-reference | record     | geo::Payload | ordinary            | -       |

  Scenario: A free function distinguishes every value and pointer-reference form
    Then callable "geo::pointer_forms" has parameter slots:
      | position | name                       | declared_type              | adjusted_type              | mode             | value_kind | named_decl | reference_semantics |
      | 0        | value                      | geo::Point                 | geo::Point                 | value            | record     | geo::Point | -                   |
      | 1        | const_value                | const geo::Point           | geo::Point                 | value            | record     | geo::Point | -                   |
      | 2        | pointer                    | geo::Point *               | geo::Point *               | value            | pointer    | geo::Point | -                   |
      | 3        | pointer_to_pointer         | geo::Point **              | geo::Point **              | value            | pointer    | geo::Point | -                   |
      | 4        | pointer_to_const           | const geo::Point *         | const geo::Point *         | value            | pointer    | geo::Point | -                   |
      | 5        | const_pointer              | geo::Point * const         | geo::Point *               | value            | pointer    | geo::Point | -                   |
      | 6        | const_pointer_to_const     | const geo::Point * const   | const geo::Point *         | value            | pointer    | geo::Point | -                   |
      | 7        | pointer_ref                | geo::Point *&              | geo::Point *&              | lvalue-reference | pointer    | geo::Point | ordinary            |
      | 8        | pointer_to_const_ref       | const geo::Point *&        | const geo::Point *&        | lvalue-reference | pointer    | geo::Point | ordinary            |
      | 9        | const_pointer_ref          | geo::Point * const &       | geo::Point * const &       | lvalue-reference | pointer    | geo::Point | ordinary            |
      | 10       | const_pointer_to_const_ref | const geo::Point * const & | const geo::Point * const & | lvalue-reference | pointer    | geo::Point | ordinary            |
      | 11       | pointer_rvalue_ref         | geo::Point *&&             | geo::Point *&&             | rvalue-reference | pointer    | geo::Point | ordinary            |

  Scenario: Pointer-reference qualifiers remain attached to the correct layer
    Then parameter 8 of callable "geo::pointer_forms" has type layers:
      | path                  | kind             | const | declaration |
      | root                  | lvalue-reference | false | -           |
      | root.referent         | pointer           | false | -           |
      | root.referent.pointee | record            | true  | geo::Point  |
    And parameter 9 of callable "geo::pointer_forms" has type layers:
      | path                  | kind             | const | declaration |
      | root                  | lvalue-reference | false | -           |
      | root.referent         | pointer           | true  | -           |
      | root.referent.pointee | record            | false | geo::Point  |
    And parameter 10 of callable "geo::pointer_forms" has type layers:
      | path                  | kind             | const | declaration |
      | root                  | lvalue-reference | false | -           |
      | root.referent         | pointer           | true  | -           |
      | root.referent.pointee | record            | true  | geo::Point  |

  Scenario: Array references do not decay and retain their bounds
    Then callable "geo::compound_forms" has these array-reference slots:
      | position | name            | declared_type          | adjusted_type          | mode             | value_kind | extent | element_decl |
      | 0        | array_ref       | geo::Point (&)[4]      | geo::Point (&)[4]      | lvalue-reference | array      | 4      | geo::Point   |
      | 1        | const_array_ref | const geo::Point (&)[4] | const geo::Point (&)[4] | lvalue-reference | array      | 4      | geo::Point   |
    And the element type of parameter 1 is const-qualified

  Scenario: Function references retain reference mode and function shape
    Then callable "geo::compound_forms" has these function-reference slots:
      | position | name                | declared_type | adjusted_type | mode             | value_kind | named_decl  | reference_semantics |
      | 2        | function_ref        | Mapper &      | Mapper &      | lvalue-reference | alias      | geo::Mapper | ordinary            |
      | 3        | function_rvalue_ref | Mapper &&     | Mapper &&     | rvalue-reference | alias      | geo::Mapper | ordinary            |
    And after removing each reference layer, both canonical referent types are "geo::Point (const geo::Point &, geo::Unit)"

  Scenario: Member pointers expose their owner and component types
    Then callable "geo::compound_forms" has these member-pointer slots:
      | position | name            | declared_type     | mode  | value_kind        | canonical_kind          | owner_decl |
      | 4        | member_function | MemberMapper      | value | alias             | member-function-pointer | geo::Ruler |
      | 5        | member_data     | int geo::Point::* | value | member-data-pointer | member-data-pointer     | geo::Point |
    And member-function pointer parameter 4 has component slots:
      | role      | position | type               | named_decl    |
      | return    | -        | Distance           | geo::Distance |
      | parameter | 0        | const geo::Point & | geo::Point    |
    And the member-function type of parameter 4 has cv-qualifier "const"
    And member-data pointer parameter 5 has value type "int"

  Scenario: Complex pointer forms use the same slots across callable kinds
    Then the following callable parameters are recorded identically:
      | callable                         | callable_kind | position | declared_type              | mode             | value_kind | named_decl | reference_semantics |
      | geo::free_reset(Point *&)        | free-function | 0        | geo::Point *&              | lvalue-reference | pointer    | geo::Point | ordinary            |
      | geo::Bindings::Bindings(Point * const &) | constructor   | 0        | geo::Point * const &       | lvalue-reference | pointer    | geo::Point | ordinary            |
      | geo::Bindings::reset(Point *&)   | method        | 0        | geo::Point *&              | lvalue-reference | pointer    | geo::Point | ordinary            |
      | geo::Bindings::observe(const Point *&) const | const-method  | 0        | const geo::Point *&        | lvalue-reference | pointer    | geo::Point | ordinary            |
      | geo::Bindings::consume(Point *&&) | static-method | 0        | geo::Point *&&             | rvalue-reference | pointer    | geo::Point | ordinary            |
    And constructor "geo::Bindings::Bindings(Point * const &)" has no return slot

  Scenario: Every supported named type reaches its declaration
    Then the following named types link to their declaring symbols:
      | type         | type_kind | declaration   | declaration_kind |
      | geo::Point   | record    | geo::Point    | struct           |
      | geo::Ruler   | record    | geo::Ruler    | class            |
      | geo::Payload | record    | geo::Payload  | union             |
      | geo::Unit    | enum      | geo::Unit     | enum-class        |
      | Distance     | alias     | geo::Distance | type-alias        |
      | Location     | alias     | geo::Location | typedef           |
      | Mapper       | alias     | geo::Mapper   | type-alias        |
      | MemberMapper | alias     | geo::MemberMapper | type-alias     |
    And the builtin types "void", "bool", "int", and "double" have no declaring symbol

  Scenario: Cv and restrict qualifiers remain attached to callable type layers
    Then callable "geo::qualified(const volatile Point * const &, Point * __restrict)" has parameter slots:
      | position | name   | declared_type                       | adjusted_type                       | mode             | value_kind | named_decl |
      | 0        | input  | const volatile geo::Point * const & | const volatile geo::Point * const & | lvalue-reference | pointer    | geo::Point |
      | 1        | output | geo::Point * __restrict             | geo::Point * __restrict             | value            | pointer    | geo::Point |
    And parameter 0 has type layers:
      | path              | kind             | const | volatile | restrict | declaration |
      | root              | lvalue-reference | false | false    | false    | -           |
      | root.referent     | pointer           | true  | false    | false    | -           |
      | root.referent.pointee | record        | true  | true     | false    | geo::Point  |
    And parameter 1 has type layers:
      | path         | kind    | const | volatile | restrict | declaration |
      | root         | pointer | false | false    | true     | -           |
      | root.pointee | record  | false | false    | false    | geo::Point  |

  Scenario: Aliases preserve identity and expose their canonical targets
    Then the type "Distance" canonicalizes to "double"
    And symbol "geo::Distance" has underlying type "double"
    And the type "Location" canonicalizes to "geo::Point"
    And symbol "geo::Location" has underlying type "geo::Point"
    And the underlying type of "geo::Location" is declared by "geo::Point"
    And the type "Mapper" canonicalizes to "geo::Point (const geo::Point &, geo::Unit)"
    And symbol "geo::Mapper" has underlying type "geo::Point (const geo::Point &, geo::Unit)"
    And symbol "geo::MemberMapper" has underlying type "Distance (geo::Ruler::*)(const geo::Point &) const"
    And the type "MemberMapper" canonicalizes to "double (geo::Ruler::*)(const geo::Point &) const"

  Scenario: A raw function-pointer slot exposes the complete function type
    Then callable "geo::apply(Point (*)(const Point &, Unit))" has signature slots:
      | role      | position | name   | declared_type                              | adjusted_type                              | mode  | value_kind | named_decl | reference_semantics | default |
      | return    | -        | -      | void                                       | void                                       | value | builtin    | -          | -                   | -       |
      | parameter | 0        | mapper | geo::Point (*)(const geo::Point &, geo::Unit) | geo::Point (*)(const geo::Point &, geo::Unit) | value | pointer    | -          | -                   | nullptr |
    And its function type has component slots:
      | role      | position | type               | mode             | value_kind | named_decl |
      | return    | -        | geo::Point         | value            | record     | geo::Point |
      | parameter | 0        | const geo::Point & | lvalue-reference | record     | geo::Point |
      | parameter | 1        | geo::Unit          | value            | enum       | geo::Unit  |

  Scenario: Defaults from redeclarations form one effective signature
    # The first declaration defaults `unit`; a later declaration defaults
    # `limit`; the definition repeats neither default.
    Then callable "geo::sample(int, Unit)" has 3 declarations
    And its effective parameter defaults are:
      | position | name  | default      | introduced_by |
      | 0        | limit | 10           | declaration 2 |
      | 1        | unit  | Unit::Meters | declaration 1 |
    And declaration 3 does not erase either default

  Scenario: Reverse lookup follows return, alias, pointer, reference, and function layers
    Then the type "geo::Point" is used by callable signature slots as:
      | role      | position | callable                                            | through         |
      | return    | -        | geo::origin()                                       | reference       |
      | return    | -        | geo::find(Unit)                                     | pointer         |
      | parameter | 0        | geo::Ruler::Ruler(const Point &, Unit)              | reference       |
      | parameter | 0        | geo::Ruler::measure(const Point &) const            | reference       |
      | parameter | 0        | geo::span(const Point *, int, bool)                 | pointer         |
      | parameter | 0        | geo::configure(Location, Ruler *, Payload, Mapper &) | Location alias  |
      | parameter | 0        | geo::apply(Point (*)(const Point &, Unit))          | function return |
      | parameter | 3        | geo::pointer_forms                                  | pointer-to-pointer |
      | parameter | 10       | geo::pointer_forms                                  | const-pointer-reference |
      | parameter | 0        | geo::qualified                                      | cv-pointer-reference |

  Scenario: Recording signature slots adds no synthetic graph topology
    Then interning a signature slot or recursive type layer creates no symbol
    And interning a signature slot or recursive type layer creates no semantic edge

  Scenario: The CLI exposes the unified signature-slot view
    Then `cidx graph signature --name geo::adjust --first` lists:
      | line                                                                    |
      | return: void [value, builtin]                                           |
      | param 0: points: const geo::Point [] => const geo::Point * [value, pointer] -> geo::Point |
      | param 1: mapper: Mapper => Mapper * [value, pointer] -> geo::Mapper      |
      | param 2: limit: const int => int [value, builtin]                       |
      | param 3: cursor: geo::Point * const => geo::Point * [value, pointer] -> geo::Point |
    And `cidx graph signature --name geo::sample --first` lists:
      | line                                                   |
      | return: int [value, builtin]                           |
      | param 0: limit: int [value, builtin] = 10              |
      | param 1: unit: geo::Unit [value, enum] -> geo::Unit = Unit::Meters |

  Scenario: Enumerators record their evaluated constant values
    # Clang's constant evaluator assigns each enumerator its value and the
    # symbol row records the printed result (v33 const_value). The enum type
    # itself records none.
    Then the index holds the symbols:
      | spelling | qual_name         | kind          | const_value |
      | Meters   | geo::Unit::Meters | enum-constant | 0           |
      | Feet     | geo::Unit::Feet   | enum-constant | 1           |
      | Unit     | geo::Unit         | enum          | -           |
