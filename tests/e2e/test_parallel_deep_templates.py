"""B-006: binds features/parallel_deep_templates.feature. Steps live in steps/.

Run it the way the whole suite is run (it is NOT part of ctest):

    CIDX_BIN=$(pwd)/build/cidx uv run --project python \
        pytest tests/e2e/test_parallel_deep_templates.py
"""

from pytest_bdd import scenarios

scenarios("features/parallel_deep_templates.feature")
