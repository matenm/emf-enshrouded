"""Where the scripts in this folder read the game's files.

enshrouded.exe: --exe <path> on the command line, else <EMF_GAME_DIR>\\enshrouded.exe.
EML's types.json: --types <path>, else <EMF_GAME_DIR>\\.cache\\types.json.
Neither: the script stops and says what to pass. The flag is taken out of sys.argv, so each script still sees only its
own arguments (python sig.py --exe D:\\Games\\Enshrouded\\enshrouded.exe 0x140456790)."""
import os
import sys


def _take(flag):
    """The value of `flag` (`--exe X` or `--exe=X`) on the command line, removed from sys.argv; None when absent."""
    argv = sys.argv
    for i in range(1, len(argv)):
        if argv[i] == flag and i + 1 < len(argv):
            value = argv[i + 1]
            del argv[i:i + 2]
            return value
        if argv[i].startswith(flag + "="):
            value = argv[i][len(flag) + 1:]
            del argv[i]
            return value
    return None


def _path(flag, inside_game, what):
    given = _take(flag)
    if given:
        return given
    game = os.environ.get("EMF_GAME_DIR")
    if game:
        return os.path.join(game, inside_game)
    script = os.path.basename(sys.argv[0]) if sys.argv and sys.argv[0] else "this script"
    sys.exit("%s: where is %s? Pass %s <path>, or set EMF_GAME_DIR to the game folder." % (script, what, flag))


def exe_path():
    """enshrouded.exe: --exe <path>, else EMF_GAME_DIR's."""
    return _path("--exe", "enshrouded.exe", "enshrouded.exe")


def types_path():
    """EML's types.json: --types <path>, else EMF_GAME_DIR's .cache\\types.json."""
    return _path("--types", os.path.join(".cache", "types.json"), "EML's types.json")
