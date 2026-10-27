#!/usr/bin/env bash
# Build everything and run the test suite (MSVC environment via dev.cmd). The tests get a %TEMP% of their own, which
# must be empty again when they end: a test that leaves a folder or file there fails the run. (EMF.exe's emf-install
# folder may stay when it is empty: the app keeps it and reuses it, it does not pile up.)
set -e
cd "$(dirname "$0")/.."
cmd //c "scripts\dev.cmd cmake --build --preset msvc" > build/last_build.log 2>&1 || { tail -40 build/last_build.log; exit 1; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/emf_unit_XXXXXX")
trap 'rm -rf "$tmp"' EXIT
wtmp=$(cygpath -w "$tmp")
TMP="$wtmp" TEMP="$wtmp" cmd //c "scripts\dev.cmd ctest --preset msvc"
rmdir "$tmp/emf-install" 2>/dev/null || true
left=$(ls -A "$tmp")
if [ -n "$left" ]; then
    echo "the unit tests left these in their temp folder (each test must remove what it makes):"
    echo "$left"
    exit 1
fi
