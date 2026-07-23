@e2e
Feature: Indexing typedef and using aliases of a plain struct
  I index one translation unit that aliases a plain struct three ways: a
  C-style `typedef`, a C++ `using` alias-declaration, and a `using` alias
  whose target is itself an alias. Every alias must be a first-class symbol,
  keep the underlying type exactly as written in the source, and hold an
  `alias_of` edge to the declaration it names -- one alias-declaration step
  at a time, so the chain Pigment -> Paint -> Color stays walkable and every
  hop is identifiable by qualified name, not only by USR. A function spelled
  entirely with the aliases must keep those alias spellings in its recorded
  signature.

  Fixture: fixtures/alias_basic.cpp

       7  struct Color { int red; int green; int blue; };   (lines 7-11)
      13  typedef Color Paint;
      15  using Rgb = Color;
      17  using Pigment = Paint;                            alias of an alias
      19  Rgb blend(Paint base, Pigment tint);

  Background:
    Given a clean index workspace for fixture "alias_basic.cpp"
    When I build the index with the cidx CLI

  Scenario: The pipeline produces a resolved single-file index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file
    And the CLI output contains "index: 1 indexed, 0 failed"

  Scenario: Both alias forms are captured as symbols with their written targets
    # The typedef has internal linkage, so its USR is file-qualified; the
    # using-aliases get translation-unit-independent USRs. `type_info` is the
    # underlying type AS WRITTEN: Pigment records "Paint", not "Color". Note
    # how blend's USR (`...#$@S@Color#S0_#`) canonicalizes both parameter
    # aliases away to Color -- the USR alone cannot tell Paint from Pigment,
    # which is exactly why the alias relations below exist as named symbols
    # and edges.
    Then the index holds exactly these symbols:
      | usr                       | spelling | qual_name             | kind       | type_info            | line | col | end_line | end_col | access | is_definition | is_instantiation |
      | c:@S@Color                | Color    | Color                 | struct     | Color                | 7    | 1   | 11       | 2       | -      | true          | false            |
      | c:@S@Color@FI@red         | red      | Color::red            | member     | int                  | 8    | 3   | 8        | 10      | public | true          | false            |
      | c:@S@Color@FI@green       | green    | Color::green          | member     | int                  | 9    | 3   | 9        | 12      | public | true          | false            |
      | c:@S@Color@FI@blue        | blue     | Color::blue           | member     | int                  | 10   | 3   | 10       | 11      | public | true          | false            |
      | c:alias_basic.cpp@T@Paint | Paint    | Paint                 | typedef    | Color                | 13   | 1   | 13       | 20      | -      | true          | false            |
      | c:@Rgb                    | Rgb      | Rgb                   | type-alias | Color                | 15   | 1   | 15       | 18      | -      | true          | false            |
      | c:@Pigment                | Pigment  | Pigment               | type-alias | Paint                | 17   | 1   | 17       | 22      | -      | true          | false            |
      | c:@F@blend#$@S@Color#S0_# | blend    | blend(Paint, Pigment) | function   | Rgb (Paint, Pigment) | 19   | 1   | 19       | 36      | -      | false         | false            |

  Scenario: Each alias records its underlying type one declaration step at a time
    # Paint and Rgb sit directly on the struct. Pigment's underlying type is
    # the ALIAS Paint (declared by the Paint symbol), and only the canonical
    # view collapses the whole chain to Color.
    Then symbol "Paint" has underlying type "Color"
    And the underlying type of "Paint" is declared by "Color"
    And symbol "Rgb" has underlying type "Color"
    And the underlying type of "Rgb" is declared by "Color"
    And symbol "Pigment" has underlying type "Paint"
    And the underlying type of "Pigment" is declared by "Paint"
    And the type "Paint" canonicalizes to "Color"
    And the type "Pigment" canonicalizes to "Color"

  Scenario: Alias type nodes link back to their declaring alias symbols
    Then the following named types link to their declaring symbols:
      | type    | type_kind | declaration | declaration_kind |
      | Paint   | alias     | Paint       | typedef          |
      | Rgb     | alias     | Rgb         | type-alias       |
      | Pigment | alias     | Pigment     | type-alias       |

  Scenario: A signature written with aliases keeps the alias spellings
    # The return and parameter types stay "Rgb", "Paint", "Pigment"; each slot
    # additionally names the alias declaration it goes through, so a client
    # can hop from the signature to the alias symbol by qualified name.
    Then symbol "blend(Paint, Pigment)" returns "Rgb"
    And symbol "blend(Paint, Pigment)" takes the parameters:
      | position | name | type    |
      | 0        | base | Paint   |
      | 1        | tint | Pigment |
    And callable "blend(Paint, Pigment)" has signature slots:
      | role      | position | name | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default |
      | return    | -        | -    | Rgb           | Rgb           | value | alias      | Rgb        | -                   | -       |
      | parameter | 0        | base | Paint         | Paint         | value | alias      | Paint      | -                   | -       |
      | parameter | 1        | tint | Pigment       | Pigment       | value | alias      | Pigment    | -                   | -       |

  Scenario: Every alias relation in the file is a named `alias_of` edge
    # Alias -> target declaration, one step each: Pigment alias_of Paint,
    # never Color directly. `alias_of` is the definitional relation; `uses`
    # stays for references, so blend uses all three aliases at the exact
    # source columns where they are written (return type at the function
    # name, parameter types at the parameter names).
    Then the edge kind totals are:
      | kind     | total |
      | field_of | 3     |
      | alias_of | 3     |
      | uses     | 3     |
    And the index holds exactly these edges:
      | src                   | kind     | dst     | count | sites |
      | Color::red            | field_of | Color   | 1     | -     |
      | Color::green          | field_of | Color   | 1     | -     |
      | Color::blue           | field_of | Color   | 1     | -     |
      | Paint                 | alias_of | Color   | 1     | 13:15 |
      | Rgb                   | alias_of | Color   | 1     | 15:7  |
      | Pigment               | alias_of | Paint   | 1     | 17:7  |
      | blend(Paint, Pigment) | uses     | Rgb     | 1     | 19:5  |
      | blend(Paint, Pigment) | uses     | Paint   | 1     | 19:17 |
      | blend(Paint, Pigment) | uses     | Pigment | 1     | 19:31 |
