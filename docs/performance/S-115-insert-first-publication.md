# S-115 insert-first publication evidence

Both single-TU captures use a fresh database, the release build, and
`cidx index src/query/exec.cpp --profile-json <path>` on the same arm64 macOS
26.1 host. The normalized capture files retain the acceptance metrics from the
full disposable profile JSON.

| Metric | Before | After | Delta |
| --- | ---: | ---: | ---: |
| Writer rows staged | 294,316 | 294,316 | 0.00% |
| Writer rows inserted | 51,906 | 51,906 | 0.00% |
| Writer rows updated | 234,714 | 0 | -100.00% |
| SQLite VDBE time | 4.8058s | 2.0420s | -57.51% |
| SQLite VM instructions | 421,805,936 | 126,646,598 | -69.98% |
| SQLite step calls | 354,291 | 354,318 | +0.01% |
| SQLite full-scan steps | 50,367 | 1,147 | -97.72% |

The five-TU batch used `plan.cpp`, `args.cpp`, `usr.cpp`, `names.cpp`, and
`location.cpp` with automatic worker selection and profiling enabled. The
pre-change `origin/main` build completed in 18.19s; the candidate completed in
9.10s (-49.97%), below the 17.00s limit. `scripts/dump_layer0.sh` produced the
same SHA-256 for both databases:
`a301ced10fb16efc75c48e92db1cc80417c5ae5cbdcd35dfc0b61d3d561f455f`.

The focused C++ BDD also compares a fresh final publication with an
incremental base-to-final publication and requires identical queryable symbol,
edge, definition, type, and include projections.
