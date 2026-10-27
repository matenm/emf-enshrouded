# From the main menu, starts the LOCAL test world (slot 1) offline with the local test character.
# Screen coordinates for a 3840x2160 screen with the game windowed at its default size (see ui.ps1).
# game_session.ps1 uses these clicks only as the fallback for a build whose bridge has no load_world (which starts the
# world without clicks, on any screen).
# Check the main menu shows your "[LOCAL] <name>" test character before running this; never use it with the Steam Cloud character.
# Exit code 3 when ui.ps1 refuses a click (the game window is not in front): the rest is not clicked.
$ui = Join-Path $PSScriptRoot "ui.ps1"
$steps = @(
    @{ name = "Play"; x = 878; y = 852 },
    @{ name = "Start a Session"; x = 996; y = 696 },
    @{ name = "Start... (SAVEGAMES:LOCAL, slot 1)"; x = 1824; y = 1399 },
    @{ name = "Offline: Start"; x = 1555; y = 1174 }
)
# what the coordinates above (and game_session.ps1's character switch) are for: the primary screen's size, and the
# game window's outer size and top-left corner (the game's default window, local_settings.json windowPosition 114,114)
$clickScreen = @{ width = 3840; height = 2160 }
$clickWindow = @{ width = 2880; height = 1537; left = 114; top = 114 }
if ($MyInvocation.InvocationName -eq ".") { return }  # dot-sourced (game_session.ps1): the list only
& $ui focus | Out-Null
foreach ($s in $steps) {
    $out = "$(& $ui click $s.x $s.y)"
    if ($LASTEXITCODE -eq 3) { "stopped before $($s.name): $out"; exit 3 }
    "clicked $($s.name)"
    Start-Sleep -Milliseconds 2500
}
