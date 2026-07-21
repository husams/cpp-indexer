# Functional calculation experiment

This small C++23 project is a design aid for translating imperative C++ into a
functional model suitable for later formal reasoning. It calculates a
configuration-driven transfer fee using integer cents and basis points.

The important boundary is explicit:

```text
configuration file --I/O--> ConfigurationReader
configuration text --pure--> parse_configuration
configuration + input --pure--> FeeCalculator
result --I/O--> main
```

`ConfigurationReader::read` and `main` are effectful. The parsing and
calculation functions have no shared state, hidden inputs, or external calls.
Errors are returned as values.

## Build and run

```sh
cmake -S examples/functional-calculation \
      -B /tmp/functional-calculation-build
cmake --build /tmp/functional-calculation-build
ctest --test-dir /tmp/functional-calculation-build --output-on-failure

/tmp/functional-calculation-build/functional-calculation \
  examples/functional-calculation/config/example.conf 10000
```

The example produces:

```text
percentage_fee_cents=150
fee_cents=175
total_debit_cents=10175
```

## Candidate properties

For every accepted configuration and non-negative input:

- the result is deterministic;
- the fee and total debit are non-negative;
- `total_debit_cents == transfer_amount_cents + fee_cents`;
- when a maximum fee exists, `fee_cents <= maximum_fee_cents`;
- an arithmetic result is returned only when all intermediate operations fit
  in `Money`; otherwise the result is `arithmetic_overflow`.

These statements are candidates for translation and proof. They are not
claimed as formally proved by this C++ test suite.
