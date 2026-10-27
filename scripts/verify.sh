#!/usr/bin/env bash
# Unit tests, the release package test (install scripts, template build from the zip), the EMF.exe smoke test, the
# bridge clients and the agent tools against fake games + offline signature check against the installed game exe. Fails when a test fails, a signature or code
# offset does not match, the exe is a build EMF does not know yet, or the check itself fails (emf_sigcheck exits 1-3).
# The game folder is EMF_GAME_DIR, else the Steam library that has the game; on a PC without the game the signature
# check is skipped (with a line that says so) and the rest still runs.
set -euo pipefail
cd "$(dirname "$0")/.."
# Every test runs with a %TEMP% of this run's own (TMP and TEMP, and TMPDIR, which Python reads first). It must be empty
# at the end: a test that leaves a folder or file in %TEMP% fails verify. It is removed when verify ends.
own_tmp=$(mktemp -d "${TMPDIR:-/tmp}/emf_verify_XXXXXX")
trap 'rm -rf "$own_tmp"' EXIT
TMP=$(cygpath -w "$own_tmp")
TEMP=$TMP
TMPDIR=$own_tmp
export TMP TEMP TMPDIR
bash scripts/test.sh
# The package test's error output (a step that threw, with where) goes to a file and is shown when the test fails.
if ! powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_package.ps1 2>build/test_package.err | grep -v "^ok "; then
    echo "test_package.ps1 failed; its error output (build/test_package.err):"
    cat build/test_package.err
    exit 1
fi
# EMF.exe against a fake game folder: the page's first render and one switch clicked end to end (a few seconds).
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/app_smoke.ps1 | grep -v "^ok "
# The debug bridge's two clients against emf_bridge_fake (no game).
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/bridge_clients_test.ps1 | grep -v "^ok "
# The tools for agents: game_session.ps1 without the game, and the Python tools (scenario runner, MCP server, llms.txt
# up to date; skipped without Python 3).
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test_tools.ps1 | grep -v "^ok "
# install_emf.ps1's install stamp, uninstall_probe.ps1's archiving of logs and crash dumps, and how the developer scripts
# find the game without -Game, in %TEMP% (no game).
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/uninstall_probe_test.ps1 | grep -v "^ok "
# The byte signatures against the installed game's exe (scripts/find_game.ps1: EMF_GAME_DIR, else Steam's libraries).
if game=$(powershell -NoProfile -ExecutionPolicy Bypass -File scripts/find_game.ps1 2>/dev/null | tr -d '\r') && [ -n "$game" ]; then
    exe="$game\\enshrouded.exe"
    echo "sigcheck: $exe"
    ./build/msvc/emf_sigcheck.exe "$exe" | tail -1
    ./build/msvc/emf_sigcheck.exe --patterns mods/sim_rate/sim_rate.patterns "$exe" | tail -1
    ./build/msvc/emf_sigcheck.exe --patterns mods/loot_scale/loot_scale.patterns "$exe" | tail -1
else
    echo "sigcheck skipped: no game found (set EMF_GAME_DIR)"
fi
# The public copy of the committed HEAD (scripts/export_public.ps1) holds no personal data: the generic checks, plus your
# own rules from .git/info/emf-export-private.txt when that file exists.
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/export_public.ps1 -Out "$TMP\\export" -Ref HEAD -NoGitInit
rm -rf "$own_tmp/export"
left=$(ls -A "$own_tmp")
if [ -n "$left" ]; then
    echo "the tests left these in their temp folder (each test must remove what it makes):"
    echo "$left"
    exit 1
fi
