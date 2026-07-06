#!/bin/bash
#
# Run the mockatests test suite in a meson build directory configured with
# -DGCOV=true -Db_coverage=true, and collect an lcov/genhtml coverage report
# plus the raw test logs and gcov files.
#
# Usage: collect-test-results.sh <build-directory> <output-directory>
#
# The build directory must have been configured with, at least:
#   meson setup <build-directory> -DGCOV=true -Db_coverage=true
#
# -Db_coverage=true is required in addition to -DGCOV=true: the project's
# own GCOV option only builds the mockatests binaries unconditionally,
# while meson's built-in b_coverage is what actually instruments the
# production libraries (control, vhd, cbt, drivers, ...) that the tests
# exercise. Without it, only the test harness's own sources show up in
# the report.

set -eu

if [ $# -ne 2 ]; then
    echo "Usage: $0 <build-directory> <output-directory>"
    exit 1
fi

BUILD_DIR=$(realpath "$1")
OUTPUT_DIR=$(realpath "$2")
SRC_DIR=$(cd "$(dirname "$0")" && pwd)

# lcov >= 2.0 rejects gcov data it used to merely warn about (e.g. inlined
# functions counted as not-hit on a hit line); relax those checks rather
# than fighting the compiler/lcov version combination. "empty" is needed
# because, with an out-of-tree build, one of the two --directory trees
# below legitimately has no .gcno/.gcda of its own.
LCOV_ARGS="--rc branch_coverage=1 --ignore-errors inconsistent,corrupt,empty"
LCOV_DIRS="--directory $BUILD_DIR --directory $SRC_DIR"

mkdir -p "$OUTPUT_DIR"

# Discard any .gcda left over from a previous run so the initial capture
# below reflects a clean, zero-coverage baseline.
find "$BUILD_DIR" -name '*.gcda' -exec rm {} \;

lcov --capture --initial $LCOV_DIRS $LCOV_ARGS \
    --no-external --output-file "$OUTPUT_DIR/coverage_base.info"

meson test -C "$BUILD_DIR" --print-errorlogs || true

lcov --capture $LCOV_DIRS $LCOV_ARGS \
    --no-external --output-file "$OUTPUT_DIR/coverage_test.info"

lcov $LCOV_ARGS \
    --add-tracefile "$OUTPUT_DIR/coverage_base.info" \
    --add-tracefile "$OUTPUT_DIR/coverage_test.info" \
    --output-file "$OUTPUT_DIR/coverage.info"

genhtml "$OUTPUT_DIR/coverage.info" $LCOV_ARGS \
    --output-directory "$OUTPUT_DIR/coverage-html"

tar cf "$OUTPUT_DIR/test-results.tar" -C "$BUILD_DIR" meson-logs
tar cf "$OUTPUT_DIR/gcov-files.tar" -C "$BUILD_DIR" \
    $(cd "$BUILD_DIR" && find . -name '*.gcda' -o -name '*.gcno')

lcov -l "$OUTPUT_DIR/coverage.info"
