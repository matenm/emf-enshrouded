# Finds the game folder for the developer scripts (install_emf.ps1, install_probe.ps1, uninstall_probe.ps1,
# content_test.ps1, game_session.ps1) and verify.sh: the folder passed with -Game, else EMF_GAME_DIR, else the Steam
# library that has Enshrouded (found the way the player's install.cmd finds it, package\emf_common.ps1).
#   find_game.ps1      prints the game folder; exit code 1 and a message when it is not found
#   . find_game.ps1    (dot-sourced) defines Resolve-GameDir only
# Tests: EMF_TEST_STEAM_ROOT names a fake Steam folder whose steamapps\libraryfolders.vdf is read instead of the real
# one; EMF_TEST_NO_STEAM (emf_common.ps1) hides the real Steam.
$script:FindGameCommon = Join-Path $PSScriptRoot "..\package\emf_common.ps1"

# The game folder: $Game when given, else EMF_GAME_DIR, else the Steam library that has the game. Throws a plain message
# when none of them gives one.
function Resolve-GameDir([string]$Game = "") {
    if ($Game) { return $Game }
    if ($env:EMF_GAME_DIR) { return $env:EMF_GAME_DIR }
    . $script:FindGameCommon  # dot-sourced inside this function: its functions do not reach the caller
    $found = Find-EnshroudedDir $env:EMF_TEST_STEAM_ROOT
    if ($found) { return $found }
    throw "the game folder was not found (EMF_GAME_DIR is not set and no Steam library has Enshrouded): pass -Game <folder>"
}

if ($MyInvocation.InvocationName -eq ".") { return }
try {
    Resolve-GameDir
    exit 0
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
