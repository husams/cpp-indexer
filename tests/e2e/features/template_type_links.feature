@e2e @spec
Feature: Template callables use the same typed signature slots as ordinary callables
  Template patterns, instantiations, and specializations must expose the same
  signature-slot contract as ordinary free functions, methods, and constructors.
  Template provenance is orthogonal metadata; it must not create a second kind
  of parameter or return representation.

  Every callable signature uses `return` and `parameter` slots with declared and
  adjusted types, value/reference mode, value kind, named declaration, reference
  semantics, and default text. Constructors have no return slot. Dependent types
  use `template-param` nodes; concrete callables use substituted types.

  The formal term `forwarding reference` is used instead of the colloquial
  `universal reference`. A parameter is forwarding only when it is an
  cv-unqualified deduced function-template parameter of the form `T &&` or
  `auto &&`. `const T &&`, a non-deduced `T &&`, and a class-template member that
  uses the class's `T &&` are ordinary rvalue references. After substitution,
  reference collapsing is recorded in the concrete adjusted type while the slot
  retains `from-forwarding` provenance.

  The covered template forms are:

    * primary class and function template patterns;
    * implicit and explicit class-template instantiations;
    * partial and full class-template specializations;
    * implicit and explicit function-template instantiations;
    * explicit function-template specializations;
    * member-function templates and class-template constructors;
    * parameter and argument packs;
    * type, non-type, template-template, and type-pack parameters.

  Class templates and class-template instances are not callables. Their methods
  and constructors have signature slots. A concrete class instance owns its
  `instantiates` relationship. Ordinary members materialized for that class use
  `method_of` only and are not mislabeled as function-template instantiations.
  Real free/member function-template specializations retain their callable
  template relationship.

  Function defaults remain source-backed. An instantiation or explicit function
  specialization inherits default text and its origin from the pattern; the
  index never invents substituted source text.

  Fixture: fixtures/template_type_links.cpp

    struct Widget { int id; };
    enum class Mode { Read, Write };
    template<class T> struct Box { };
    template<template<class> class C = Box, class... Ts> struct Pipeline { };
    template<Mode M = Mode::Read> struct Access { };
    template<class T = Widget, int N = 4> class Buffer {
      Buffer(T seed = T{});
      T get(T fallback = T{});
    };
    template<class T> class Buffer<T *, 1> { ... };       partial specialization
    template<> class Buffer<Widget, 1> { ... };           full specialization
    template class Buffer<Widget, 4>;                     explicit instantiation
    template<class T> T identity(T value = T{});
    template double identity<double>(double value);       explicit instantiation
    template<> Widget identity<Widget>(Widget value);     explicit specialization
    template<class U> U Holder::keep(U item = U{});
    template<class U> U && Holder::relay(U && item);
    template<class U> void Holder::bind(const U *const &item);
    template<class T> T && forward_value(T && value);
    template<class T> const T && exact(const T && value);
    template<class T> struct Sink { void consume(T && value); };
    template<class T> void pointer_template(
      T value, T *pointer, T **pointer_to_pointer,
      const T *pointer_to_const, T *const const_pointer,
      const T *const const_pointer_to_const,
      T *&pointer_ref, const T *&pointer_to_const_ref,
      T *const &const_pointer_ref,
      const T *const &const_pointer_to_const_ref,
      T *&&pointer_rvalue_ref);
    template void pointer_template<int>(
      int, int *, int **, const int *, int *const, const int *const,
      int *&, const int *&, int *const &, const int *const &, int *&&);
    template<> void pointer_template<Widget>(
      Widget, Widget *, Widget **, const Widget *,
      Widget *const, const Widget *const,
      Widget *&, const Widget *&, Widget *const &,
      const Widget *const &, Widget *&&);
    template<class T> struct PointerBox {
      PointerBox(T *const &initial);
      void reset(T *&slot);
      void observe(const T *&slot) const;
    };
    template<> struct PointerBox<Widget> {
      PointerBox(Widget *const &initial);
      void reset(Widget *&slot);
      void observe(const Widget *&slot) const;
    };
    template<class T, int N> void array_template(T (&values)[N]);
    template<class T> void callback_template(T (&callback)(const T &));
    template<class... Ts> void visit(Ts... values);

  The fixture instantiates both lvalue and rvalue forms of `forward_value`, plus
  `Buffer<int, 2>`, `Buffer<Widget *, 1>`, `identity<int>`,
  `identity<double>`,
  `Holder::keep<int>`, `Holder::relay<Widget>`, `Holder::bind<int>`,
  `visit<int, Widget>`,
  `Pipeline<Box, Widget, int>`, and `Access<Mode::Write>`.
  It also implicitly instantiates `pointer_template<double>`,
  `PointerBox<int>`, `array_template<Widget, 4>`, and
  `callback_template<Widget>`.

  Constraint evaluation, deduction guides, overload resolution, and indexing
  declarations from the standard library are outside this feature.

  Background:
    Given a clean index workspace for fixture "template_type_links.cpp"
    When I build the index with the cidx CLI

  Scenario: The fixture produces a resolved index
    Then the index database exists
    And the entity graph is resolved
    And the index holds 1 indexed file

  Scenario: A class-template member pattern has dependent signature slots
    Then callable "Buffer<T, N>::get(T)" has signature slots:
      | role      | position | name     | declared_type     | adjusted_type     | mode  | value_kind    | named_decl | reference_semantics | default |
      | return    | -        | -        | type-parameter-0-0 | type-parameter-0-0 | value | template-param | -          | -                   | -       |
      | parameter | 0        | fallback | type-parameter-0-0 | type-parameter-0-0 | value | template-param | -          | -                   | T{}     |

  Scenario: A class-template constructor pattern has no return slot
    Then callable "Buffer<T, N>::Buffer(T)" has no return slot
    And callable "Buffer<T, N>::Buffer(T)" has parameter slots:
      | role      | position | name | declared_type      | adjusted_type      | mode  | value_kind     | named_decl | reference_semantics | default |
      | parameter | 0        | seed | type-parameter-0-0 | type-parameter-0-0 | value | template-param | -          | -                   | T{}     |

  Scenario: Free and member function-template patterns use the same slots
    Then callable "identity<T>(T)" has signature slots:
      | role      | position | name  | declared_type      | adjusted_type      | mode  | value_kind     | named_decl | reference_semantics | default |
      | return    | -        | -     | type-parameter-0-0 | type-parameter-0-0 | value | template-param | -          | -                   | -       |
      | parameter | 0        | value | type-parameter-0-0 | type-parameter-0-0 | value | template-param | -          | -                   | T{}     |
    And callable "Holder::keep<U>(U)" has signature slots:
      | role      | position | name | declared_type      | adjusted_type      | mode  | value_kind     | named_decl | reference_semantics | default |
      | return    | -        | -    | type-parameter-0-0 | type-parameter-0-0 | value | template-param | -          | -                   | -       |
      | parameter | 0        | item | type-parameter-0-0 | type-parameter-0-0 | value | template-param | -          | -                   | U{}     |

  Scenario: Explicit and implicit class instances have concrete member slots
    Then callable "Buffer<Widget, 4>::get(Widget)" has signature slots:
      | role      | position | name     | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default | default_origin       |
      | return    | -        | -        | Widget        | Widget        | value | record     | Widget     | -                   | -       | -                    |
      | parameter | 0        | fallback | Widget        | Widget        | value | record     | Widget     | -                   | T{}     | Buffer<T, N>::get(T) |
    And callable "Buffer<int, 2>::get(int)" has signature slots:
      | role      | position | name     | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default | default_origin       |
      | return    | -        | -        | int           | int           | value | builtin    | -          | -                   | -       | -                    |
      | parameter | 0        | fallback | int           | int           | value | builtin    | -          | -                   | T{}     | Buffer<T, N>::get(T) |

  Scenario: A concrete class-template constructor has no return slot
    Then callable "Buffer<Widget, 4>::Buffer(Widget)" has no return slot
    And callable "Buffer<Widget, 4>::Buffer(Widget)" has parameter slots:
      | role      | position | name | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default | default_origin          |
      | parameter | 0        | seed | Widget        | Widget        | value | record     | Widget     | -                   | T{}     | Buffer<T, N>::Buffer(T) |

  Scenario: Partial and full class specializations own their signatures
    Then callable "Buffer<T *, 1>::get(T *)" has signature slots:
      | role      | position | name     | declared_type          | adjusted_type          | mode  | value_kind | named_decl | reference_semantics | default |
      | return    | -        | -        | type-parameter-0-0 *   | type-parameter-0-0 *   | value | pointer    | -          | -                   | -       |
      | parameter | 0        | fallback | type-parameter-0-0 *   | type-parameter-0-0 *   | value | pointer    | -          | -                   | nullptr |
    And callable "Buffer<Widget *, 1>::get(Widget *)" has signature slots:
      | role      | position | name     | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default | default_origin           |
      | return    | -        | -        | Widget *      | Widget *      | value | pointer    | Widget     | -                   | -       | -                        |
      | parameter | 0        | fallback | Widget *      | Widget *      | value | pointer    | Widget     | -                   | nullptr | Buffer<T *, 1>::get(T *) |
    And callable "Buffer<Widget, 1>::get(Widget)" has signature slots:
      | role      | position | name     | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default  |
      | return    | -        | -        | Widget        | Widget        | value | record     | Widget     | -                   | -        |
      | parameter | 0        | fallback | Widget        | Widget        | value | record     | Widget     | -                   | Widget{} |

  Scenario: Function-template instantiations and specializations use concrete slots
    Then callable "identity<int>(int)" has signature slots:
      | role      | position | name  | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default | default_origin |
      | return    | -        | -     | int           | int           | value | builtin    | -          | -                   | -       | -              |
      | parameter | 0        | value | int           | int           | value | builtin    | -          | -                   | T{}     | identity<T>(T) |
    And callable "identity<double>(double)" has signature slots:
      | role      | position | name  | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default | default_origin |
      | return    | -        | -     | double        | double        | value | builtin    | -          | -                   | -       | -              |
      | parameter | 0        | value | double        | double        | value | builtin    | -          | -                   | T{}     | identity<T>(T) |
    And callable "identity<Widget>(Widget)" has signature slots:
      | role      | position | name  | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default | default_origin |
      | return    | -        | -     | Widget        | Widget        | value | record     | Widget     | -                   | -       | -              |
      | parameter | 0        | value | Widget        | Widget        | value | record     | Widget     | -                   | T{}     | identity<T>(T) |
    And the explicit specialization does not declare a new default argument

  Scenario: An instantiated member-function template uses concrete slots
    Then callable "Holder::keep<int>(int)" has signature slots:
      | role      | position | name | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default | default_origin     |
      | return    | -        | -    | int           | int           | value | builtin    | -          | -                   | -       | -                  |
      | parameter | 0        | item | int           | int           | value | builtin    | -          | -                   | U{}     | Holder::keep<U>(U) |

  Scenario: A deduced `T &&` parameter is a forwarding reference
    Then callable "forward_value<T>(T &&)" has signature slots:
      | role      | position | name  | declared_type       | adjusted_type       | mode             | value_kind     | named_decl | reference_semantics | default |
      | return    | -        | -     | type-parameter-0-0 && | type-parameter-0-0 && | rvalue-reference | template-param | -          | -                   | -       |
      | parameter | 0        | value | type-parameter-0-0 && | type-parameter-0-0 && | rvalue-reference | template-param | -          | forwarding          | -       |
    And callable "Holder::relay<U>(U &&)" parameter 0 has reference semantics "forwarding"

  Scenario: Cv-qualified `T &&` and class-template `T &&` are ordinary references
    Then callable "exact<T>(const T &&)" parameter 0 has reference semantics "ordinary"
    And callable "Sink<T>::consume(T &&)" parameter 0 has reference semantics "ordinary"
    And callable "Sink<Widget>::consume(Widget &&)" parameter 0 has reference semantics "ordinary"

  Scenario: Forwarding substitution records reference collapsing
    Then callable "forward_value<Widget &>(Widget &)" has signature slots:
      | role      | position | name  | declared_type | adjusted_type | mode             | value_kind | named_decl | reference_semantics | default |
      | return    | -        | -     | Widget &      | Widget &      | lvalue-reference | record     | Widget     | -                   | -       |
      | parameter | 0        | value | Widget &      | Widget &      | lvalue-reference | record     | Widget     | from-forwarding     | -       |
    And callable "forward_value<Widget>(Widget &&)" has signature slots:
      | role      | position | name  | declared_type | adjusted_type | mode             | value_kind | named_decl | reference_semantics | default |
      | return    | -        | -     | Widget &&     | Widget &&     | rvalue-reference | record     | Widget     | -                   | -       |
      | parameter | 0        | value | Widget &&     | Widget &&     | rvalue-reference | record     | Widget     | from-forwarding     | -       |

  Scenario: A function-template pattern preserves pointer-reference distinctions
    Then callable "pointer_template<T>" has parameter slots:
      | position | name                       | declared_type                          | adjusted_type                          | mode             | value_kind | named_decl | reference_semantics |
      | 0        | value                      | type-parameter-0-0                     | type-parameter-0-0                     | value            | template-param | -        | -                   |
      | 1        | pointer                    | type-parameter-0-0 *                   | type-parameter-0-0 *                   | value            | pointer    | -          | -                   |
      | 2        | pointer_to_pointer         | type-parameter-0-0 **                  | type-parameter-0-0 **                  | value            | pointer    | -          | -                   |
      | 3        | pointer_to_const           | const type-parameter-0-0 *             | const type-parameter-0-0 *             | value            | pointer    | -          | -                   |
      | 4        | const_pointer              | type-parameter-0-0 * const             | type-parameter-0-0 *                   | value            | pointer    | -          | -                   |
      | 5        | const_pointer_to_const     | const type-parameter-0-0 * const       | const type-parameter-0-0 *             | value            | pointer    | -          | -                   |
      | 6        | pointer_ref                | type-parameter-0-0 *&                  | type-parameter-0-0 *&                  | lvalue-reference | pointer    | -          | ordinary            |
      | 7        | pointer_to_const_ref       | const type-parameter-0-0 *&            | const type-parameter-0-0 *&            | lvalue-reference | pointer    | -          | ordinary            |
      | 8        | const_pointer_ref          | type-parameter-0-0 * const &           | type-parameter-0-0 * const &           | lvalue-reference | pointer    | -          | ordinary            |
      | 9        | const_pointer_to_const_ref | const type-parameter-0-0 * const &     | const type-parameter-0-0 * const &     | lvalue-reference | pointer    | -          | ordinary            |
      | 10       | pointer_rvalue_ref         | type-parameter-0-0 *&&                 | type-parameter-0-0 *&&                 | rvalue-reference | pointer    | -          | ordinary            |

  Scenario: Implicit and explicit function-template instances preserve pointer forms
    Then the following concrete slots are recorded:
      | callable                 | template_form          | position | declared_type          | mode             | value_kind | named_decl |
      | pointer_template<double> | implicit-instantiation | 0        | double                 | value            | builtin    | -          |
      | pointer_template<double> | implicit-instantiation | 1        | double *               | value            | pointer    | -          |
      | pointer_template<double> | implicit-instantiation | 2        | double **              | value            | pointer    | -          |
      | pointer_template<double> | implicit-instantiation | 3        | const double *         | value            | pointer    | -          |
      | pointer_template<double> | implicit-instantiation | 4        | double * const         | value            | pointer    | -          |
      | pointer_template<double> | implicit-instantiation | 5        | const double * const   | value            | pointer    | -          |
      | pointer_template<double> | implicit-instantiation | 6        | double *&              | lvalue-reference | pointer    | -          |
      | pointer_template<double> | implicit-instantiation | 7        | const double *&        | lvalue-reference | pointer    | -          |
      | pointer_template<double> | implicit-instantiation | 8        | double * const &       | lvalue-reference | pointer    | -          |
      | pointer_template<double> | implicit-instantiation | 9        | const double * const & | lvalue-reference | pointer    | -          |
      | pointer_template<double> | implicit-instantiation | 10       | double *&&             | rvalue-reference | pointer    | -          |
      | pointer_template<int>    | explicit-instantiation | 0        | int                    | value            | builtin    | -          |
      | pointer_template<int>    | explicit-instantiation | 1        | int *                  | value            | pointer    | -          |
      | pointer_template<int>    | explicit-instantiation | 2        | int **                 | value            | pointer    | -          |
      | pointer_template<int>    | explicit-instantiation | 3        | const int *            | value            | pointer    | -          |
      | pointer_template<int>    | explicit-instantiation | 4        | int * const            | value            | pointer    | -          |
      | pointer_template<int>    | explicit-instantiation | 5        | const int * const      | value            | pointer    | -          |
      | pointer_template<int>    | explicit-instantiation | 6        | int *&                 | lvalue-reference | pointer    | -          |
      | pointer_template<int>    | explicit-instantiation | 7        | const int *&           | lvalue-reference | pointer    | -          |
      | pointer_template<int>    | explicit-instantiation | 8        | int * const &          | lvalue-reference | pointer    | -          |
      | pointer_template<int>    | explicit-instantiation | 9        | const int * const &    | lvalue-reference | pointer    | -          |
      | pointer_template<int>    | explicit-instantiation | 10       | int *&&                | rvalue-reference | pointer    | -          |

  Scenario: An explicit function-template specialization uses the same pointer slots
    Then callable "pointer_template<Widget>" has parameter slots:
      | position | name                       | declared_type                 | adjusted_type                 | mode             | value_kind | named_decl | reference_semantics |
      | 0        | value                      | Widget                       | Widget                       | value            | record     | Widget     | -                   |
      | 1        | pointer                    | Widget *                     | Widget *                     | value            | pointer    | Widget     | -                   |
      | 2        | pointer_to_pointer         | Widget **                    | Widget **                    | value            | pointer    | Widget     | -                   |
      | 3        | pointer_to_const           | const Widget *               | const Widget *               | value            | pointer    | Widget     | -                   |
      | 4        | const_pointer              | Widget * const               | Widget *                     | value            | pointer    | Widget     | -                   |
      | 5        | const_pointer_to_const     | const Widget * const         | const Widget *               | value            | pointer    | Widget     | -                   |
      | 6        | pointer_ref                | Widget *&                    | Widget *&                    | lvalue-reference | pointer    | Widget     | ordinary            |
      | 7        | pointer_to_const_ref       | const Widget *&              | const Widget *&              | lvalue-reference | pointer    | Widget     | ordinary            |
      | 8        | const_pointer_ref          | Widget * const &             | Widget * const &             | lvalue-reference | pointer    | Widget     | ordinary            |
      | 9        | const_pointer_to_const_ref | const Widget * const &       | const Widget * const &       | lvalue-reference | pointer    | Widget     | ordinary            |
      | 10       | pointer_rvalue_ref         | Widget *&&                   | Widget *&&                   | rvalue-reference | pointer    | Widget     | ordinary            |

  Scenario: A member-function template preserves nested pointer qualifiers
    Then callable "Holder::bind<U>(const U * const &)" parameter 0 has:
      | declared_type                              | mode             | value_kind | named_decl | reference_semantics |
      | const type-parameter-0-0 * const &         | lvalue-reference | pointer    | -          | ordinary            |
    And callable "Holder::bind<int>(const int * const &)" parameter 0 has:
      | declared_type       | mode             | value_kind | named_decl | reference_semantics |
      | const int * const & | lvalue-reference | pointer    | -          | ordinary            |

  Scenario: Class-template constructors and methods preserve pointer-reference forms
    Then callable "PointerBox<T>::PointerBox(T * const &)" has no return slot
    And the following class-template member slots are recorded:
      | callable                                  | callable_kind | position | declared_type                      | mode             | value_kind | named_decl |
      | PointerBox<T>::PointerBox(T * const &)    | constructor   | 0        | type-parameter-0-0 * const &      | lvalue-reference | pointer    | -          |
      | PointerBox<T>::reset(T *&)                | method        | 0        | type-parameter-0-0 *&             | lvalue-reference | pointer    | -          |
      | PointerBox<T>::observe(const T *&) const  | const-method  | 0        | const type-parameter-0-0 *&       | lvalue-reference | pointer    | -          |
      | PointerBox<int>::PointerBox(int * const &) | constructor   | 0        | int * const &                     | lvalue-reference | pointer    | -          |
      | PointerBox<int>::reset(int *&)            | method        | 0        | int *&                            | lvalue-reference | pointer    | -          |
      | PointerBox<int>::observe(const int *&) const | const-method | 0      | const int *&                      | lvalue-reference | pointer    | -          |
      | PointerBox<Widget>::PointerBox(Widget * const &) | constructor | 0   | Widget * const &                  | lvalue-reference | pointer    | Widget     |
      | PointerBox<Widget>::reset(Widget *&)      | method        | 0        | Widget *&                         | lvalue-reference | pointer    | Widget     |
      | PointerBox<Widget>::observe(const Widget *&) const | const-method | 0 | const Widget *&                   | lvalue-reference | pointer    | Widget     |

  Scenario: Template array and function references retain their shapes
    Then callable "array_template<T, N>(T (&)[N])" parameter 0 has:
      | declared_type                  | mode             | value_kind | extent | element_type       |
      | type-parameter-0-0 (&)[N]      | lvalue-reference | array      | N      | type-parameter-0-0 |
    And callable "array_template<Widget, 4>(Widget (&)[4])" parameter 0 has:
      | declared_type  | mode             | value_kind | extent | element_type | named_decl |
      | Widget (&)[4]  | lvalue-reference | array      | 4      | Widget       | Widget     |
    And callable "callback_template<T>(T (&)(const T &))" parameter 0 has:
      | declared_type                                            | mode             | value_kind | reference_semantics |
      | type-parameter-0-0 (&)(const type-parameter-0-0 &)       | lvalue-reference | function   | ordinary            |
    And callable "callback_template<Widget>(Widget (&)(const Widget &))" parameter 0 has:
      | declared_type                      | mode             | value_kind | named_decl | reference_semantics |
      | Widget (&)(const Widget &)         | lvalue-reference | function   | Widget     | ordinary            |

  Scenario: A function parameter pack expands into ordered concrete slots
    Then callable "visit<Ts...>(Ts...)" has parameter slots:
      | role      | position | name   | declared_type          | adjusted_type          | mode  | value_kind    | named_decl | reference_semantics | default |
      | parameter | 0        | values | type-parameter-0-0... | type-parameter-0-0... | value | pack-expansion | -          | -                   | -       |
    And callable "visit<int, Widget>(int, Widget)" has parameter slots:
      | role      | position | pack_index | name   | declared_type | adjusted_type | mode  | value_kind | named_decl | reference_semantics | default |
      | parameter | 0        | 0          | values | int           | int           | value | builtin    | -          | -                   | -       |
      | parameter | 0        | 1          | values | Widget        | Widget        | value | record     | Widget     | -                   | -       |

  Scenario: Template parameters record kinds, types, defaults, and links
    Then the templates declare these parameters:
      | template           | position | name | kind              | type | type_decl | default    | default_type | default_ref |
      | Buffer<T, N>       | 0        | T    | type              | -    | -         | Widget     | Widget       | -           |
      | Buffer<T, N>       | 1        | N    | non-type          | int  | -         | 4          | int          | -           |
      | Access<M>          | 0        | M    | non-type          | Mode | Mode      | Mode::Read | Mode         | -           |
      | Pipeline<C, Ts...> | 0        | C    | template-template | -    | -         | Box        | -            | Box<T>      |
      | Pipeline<C, Ts...> | 1        | Ts   | type-pack         | -    | -         | -          | -            | -           |
      | pointer_template<T> | 0        | T    | type              | -    | -         | -          | -            | -           |
      | PointerBox<T>      | 0        | T    | type              | -    | -         | -          | -            | -           |
      | array_template<T, N> | 0       | T    | type              | -    | -         | -          | -            | -           |
      | array_template<T, N> | 1       | N    | non-type          | int  | -         | -          | -            | -           |
      | callback_template<T> | 0       | T    | type              | -    | -         | -          | -            | -           |

  Scenario: Template arguments retain values, value types, packs, and references
    Then the following symbols bind template arguments:
      | symbol                     | position | pack_index | kind     | value       | type     | type_kind | type_decl | ref    |
      | Buffer<Widget, 4>          | 0        | -          | type     | Widget      | Widget   | record    | Widget    | Widget |
      | Buffer<Widget, 4>          | 1        | -          | non-type | 4           | int      | builtin   | -         | -      |
      | Buffer<int, 2>             | 0        | -          | type     | int         | int      | builtin   | -         | -      |
      | Buffer<int, 2>             | 1        | -          | non-type | 2           | int      | builtin   | -         | -      |
      | Buffer<Widget *, 1>        | 0        | -          | type     | Widget *    | Widget * | pointer   | -         | Widget |
      | Buffer<Widget *, 1>        | 1        | -          | non-type | 1           | int      | builtin   | -         | -      |
      | Buffer<Widget, 1>          | 0        | -          | type     | Widget      | Widget   | record    | Widget    | Widget |
      | Buffer<Widget, 1>          | 1        | -          | non-type | 1           | int      | builtin   | -         | -      |
      | Access<Mode::Write>        | 0        | -          | non-type | Mode::Write | Mode     | enum      | Mode      | -      |
      | Pipeline<Box, Widget, int> | 0        | -          | template | Box         | -        | -         | -         | Box<T> |
      | Pipeline<Box, Widget, int> | 1        | 0          | type     | Widget      | Widget   | record    | Widget    | Widget |
      | Pipeline<Box, Widget, int> | 1        | 1          | type     | int         | int      | builtin   | -         | -      |
      | PointerBox<int>            | 0        | -          | type     | int         | int      | builtin   | -         | -      |
      | PointerBox<Widget>         | 0        | -          | type     | Widget      | Widget   | record    | Widget    | Widget |

  Scenario: Callable template arguments use the same linked type nodes
    Then the following callable symbols bind template arguments:
      | symbol                                | position | kind | value    | type     | type_kind        | type_decl | ref    |
      | identity<int>(int)                    | 0        | type | int      | int      | builtin          | -         | -      |
      | identity<double>(double)              | 0        | type | double   | double   | builtin          | -         | -      |
      | identity<Widget>(Widget)              | 0        | type | Widget   | Widget   | record           | Widget    | Widget |
      | Holder::keep<int>(int)                | 0        | type | int      | int      | builtin          | -         | -      |
      | forward_value<Widget &>(Widget &)     | 0        | type | Widget & | Widget & | lvalue-reference | -         | Widget |
      | forward_value<Widget>(Widget &&)      | 0        | type | Widget   | Widget   | record           | Widget    | Widget |
      | pointer_template<double>              | 0        | type | double   | double   | builtin          | -         | -      |
      | pointer_template<int>                 | 0        | type | int      | int      | builtin          | -         | -      |
      | pointer_template<Widget>              | 0        | type | Widget   | Widget   | record           | Widget    | Widget |
      | Holder::bind<int>(const int * const &) | 0        | type | int      | int      | builtin          | -         | -      |
      | array_template<Widget, 4>             | 0        | type | Widget   | Widget   | record           | Widget    | Widget |
      | array_template<Widget, 4>             | 1        | non-type | 4    | int      | builtin          | -         | -      |
      | callback_template<Widget>             | 0        | type | Widget   | Widget   | record           | Widget    | Widget |
    And primary callable template patterns bind no template arguments

  Scenario: Callable template form is separate from its signature slots
    Then callable template provenance is:
      | callable                                  | callable_kind | template_form          | template_origin       | owner               |
      | identity<T>(T)                            | free-function | pattern                | -                     | -                   |
      | identity<int>(int)                        | free-function | implicit-instantiation | identity<T>(T)        | -                   |
      | identity<double>(double)                  | free-function | explicit-instantiation | identity<T>(T)        | -                   |
      | identity<Widget>(Widget)                  | free-function | explicit-specialization | identity<T>(T)        | -                   |
      | Holder::keep<U>(U)                        | method        | pattern                | -                     | Holder              |
      | Holder::keep<int>(int)                    | method        | implicit-instantiation | Holder::keep<U>(U)    | Holder              |
      | Holder::bind<U>(const U * const &)        | method        | pattern                | -                     | Holder              |
      | Holder::bind<int>(const int * const &)    | method        | implicit-instantiation | Holder::bind<U>(const U * const &) | Holder |
      | pointer_template<T>                       | free-function | pattern                | -                     | -                   |
      | pointer_template<double>                  | free-function | implicit-instantiation | pointer_template<T>   | -                   |
      | pointer_template<int>                     | free-function | explicit-instantiation | pointer_template<T>   | -                   |
      | pointer_template<Widget>                  | free-function | explicit-specialization | pointer_template<T>  | -                   |
      | Buffer<T, N>::get(T)                      | method        | owner-pattern-member   | -                     | Buffer<T, N>        |
      | Buffer<Widget, 4>::get(Widget)            | method        | owner-instance-member  | -                     | Buffer<Widget, 4>   |
      | Buffer<Widget, 4>::Buffer(Widget)         | constructor   | owner-instance-member  | -                     | Buffer<Widget, 4>   |

  Scenario: Template relationships belong only to real template entities
    Then the template relationships are:
      | symbol                                 | relationship | target              |
      | Buffer<Widget, 4>                      | instantiates | Buffer<T, N>        |
      | Buffer<int, 2>                         | instantiates | Buffer<T, N>        |
      | Buffer<T *, 1>                         | specializes  | Buffer<T, N>        |
      | Buffer<Widget *, 1>                    | instantiates | Buffer<T *, 1>      |
      | Buffer<Widget, 1>                      | specializes  | Buffer<T, N>        |
      | identity<int>(int)                     | instantiates | identity<T>(T)       |
      | identity<double>(double)               | instantiates | identity<T>(T)       |
      | identity<Widget>(Widget)               | specializes  | identity<T>(T)       |
      | Holder::keep<int>(int)                 | instantiates | Holder::keep<U>(U)   |
      | Holder::relay<Widget>(Widget &&)       | instantiates | Holder::relay<U>(U &&) |
      | forward_value<Widget &>(Widget &)      | instantiates | forward_value<T>(T &&) |
      | forward_value<Widget>(Widget &&)       | instantiates | forward_value<T>(T &&) |
      | visit<int, Widget>(int, Widget)        | instantiates | visit<Ts...>(Ts...)  |
      | Sink<Widget>                           | instantiates | Sink<T>              |
      | Access<Mode::Write>                    | instantiates | Access<M>            |
      | Pipeline<Box, Widget, int>             | instantiates | Pipeline<C, Ts...>   |
      | pointer_template<double>               | instantiates | pointer_template<T>  |
      | pointer_template<int>                  | instantiates | pointer_template<T>  |
      | pointer_template<Widget>               | specializes  | pointer_template<T>  |
      | Holder::bind<int>(const int * const &) | instantiates | Holder::bind<U>(const U * const &) |
      | PointerBox<int>                        | instantiates | PointerBox<T>        |
      | PointerBox<Widget>                     | specializes  | PointerBox<T>        |
      | array_template<Widget, 4>              | instantiates | array_template<T, N> |
      | callback_template<Widget>              | instantiates | callback_template<T> |
    And callable "Buffer<Widget, 4>::get(Widget)" has no template relationship
    And callable "Buffer<Widget, 4>::Buffer(Widget)" has no template relationship
    And both callables belong to "Buffer<Widget, 4>" through `method_of`
    And callable "PointerBox<int>::reset(int *&)" has no template relationship
    And callable "PointerBox<Widget>::reset(Widget *&)" has no template relationship
    And those callables belong to their concrete "PointerBox" owners through `method_of`

  Scenario: Reverse lookup traverses signature and template-argument layers
    Then the type "Widget" is used by:
      | use_kind         | role      | position | symbol                                  | through         |
      | signature-slot   | return    | -        | Buffer<Widget, 4>::get(Widget)          | direct          |
      | signature-slot   | parameter | 0        | Buffer<Widget, 4>::get(Widget)          | direct          |
      | signature-slot   | parameter | 0        | forward_value<Widget>(Widget &&)        | rvalue-reference |
      | signature-slot   | parameter | 8        | pointer_template<Widget>                | const-pointer-reference |
      | signature-slot   | parameter | 0        | PointerBox<Widget>::reset(Widget *&)    | pointer-reference |
      | template-argument | -         | 0        | Buffer<Widget *, 1>                     | pointer          |
      | template-argument | -         | 0        | pointer_template<Widget>                | direct           |
      | template-argument | -         | 1        | Pipeline<Box, Widget, int>              | pack             |

  Scenario: Recording template signature facts adds no synthetic graph topology
    Then interning a signature slot or template-argument type creates no symbol
    And interning a signature slot or template-argument type creates no semantic edge
    And only source-backed template relationships appear in the semantic graph

  Scenario: The CLI exposes template provenance and unified signature slots
    Then `cidx graph signature --name forward_value --first` lists:
      | line                                                                      |
      | return: type-parameter-0-0 && [rvalue-reference, template-param]           |
      | param 0: value: type-parameter-0-0 && [rvalue-reference, template-param, forwarding] |
    And `cidx graph signature --name Buffer<Widget,4>::get --first` lists:
      | line                                                                      |
      | return: Widget [value, record] -> Widget                                  |
      | param 0: fallback: Widget [value, record] -> Widget = T{} [from Buffer<T, N>::get(T)] |
    And `cidx graph template --name Buffer<Widget,4> --first` lists:
      | line                               |
      | instantiates: Buffer<T, N>         |
      | arg 0: type: Widget -> Widget      |
      | arg 1: non-type: 4: int            |
