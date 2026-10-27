# Save backups

EMF keeps copies of your Enshrouded saves on your PC, so a mod (or anything else) can never cost you a character or a
world. It makes one before it installs anything, one each time the game starts with EMF (when a save changed), and one
whenever you ask. You can restore a whole backup or single files, and restoring never removes the backup.

## What is backed up

Enshrouded keeps saves in two places, and EMF backs up both:

| | Where the game keeps them |
|---|---|
| **Steam Cloud** saves | `<Steam>\userdata\<your Steam id>\1203620\remote\` |
| **Local** saves | `%USERPROFILE%\Saved Games\keen games\Enshrouded\default\`, and `local_settings.json` one folder up |

From each: every character (`character0.ksd` ... `character15.ksd`), every world (`world0.ksd` ... `world9.ksd`),
`global.ksd` and `user_settings.json`. Not the game's own `backup\` and `migrated\` folders: the game keeps up to ten
older copies of each file in `backup\` itself, and EMF leaves them alone.

A dedicated server keeps its world in its own `savegame` folder. EMF does not back that up: copy it yourself while the
server is stopped.

## Where the backups are

`%LOCALAPPDATA%\EMF\save-backups\`, one folder per backup, named after its time and kind, such as
`2026-10-27_18-05-12_manual`. Each holds `backup.json` (what is in it, with a checksum for every file) and the files
under `steam\<Steam id>\`, `local\default\` and `local\`. Saves are small (a few hundred KB), but worlds grow as you
build; EMF.exe's Saves tab shows how much space the backups take.

## The kinds

- **Starter** ("Before EMF"): your saves as they were before EMF. `install.cmd` makes it before it changes anything in
  the game folder; EMF.exe at its first start, or the game at its first start with EMF, makes it if there is none yet.
  There is only ever one: if it could not copy a file (one was in use), it says so, and the game's automatic backup at
  its next start holds the whole set.
- **Manual**: Back up now in EMF.exe, or `saves.backup` in the game's console.
- **Automatic**: each time the game starts with EMF, before any world is loaded. At most one an hour, and only when a
  save changed since the newest backup. The newest 10 are kept.
- **Before restore**: a copy of the files a restore replaces, made just before it.

Only automatic backups are ever deleted by EMF (beyond the newest 10 that hold every file; one that could not copy a
file does not count, so the newest complete copies always stay). Starter, manual and before-restore backups stay until
you delete them. Uninstalling EMF never deletes backups, not even `uninstall.cmd -All`.

## Settings

In `<game>\emf\emf_core.ini` (EMF.exe's Saves tab has the on/off switch):

```ini
[backup]
on_start=1      ; 0: no automatic backups when the game starts
keep=10         ; automatic backups kept (1-100)
min_minutes=60  ; at most one automatic backup this often
```

## Making a backup

- **EMF.exe**: the Saves tab, Back up now (a label is optional). It works while the game is closed.
- **In the game**: open the console (the key under Esc) at the main menu and type `saves.backup` or
  `saves.backup before the boss`. While a world runs it refuses, because the game writes its saves every 30 seconds
  then. `saves.list` lists the backups and where they are.

## Restoring with EMF.exe

1. Close the game.
2. Open EMF.exe, the Saves tab, and click **Restore...** on a backup.
3. Choose the whole backup, or only the files you want (a character, a world).
4. Click Restore. EMF first backs up the files it replaces ("Before restore"), then copies the backup's files back and
   checks each one.

**Steam Cloud saves.** Steam keeps a copy of these online. A restored file gets today's time, so at the next game start
Steam should upload it as the newest. If Steam instead says the files on this PC and in the cloud are in conflict,
choose the files on this PC; choosing the cloud copy brings back what you wanted to replace. (This has not been tried
against Steam itself.)

## Restoring by hand

Without EMF.exe (the manual download), copy the files back yourself:

1. Close the game.
2. Open `%LOCALAPPDATA%\EMF\save-backups\` and the backup's folder (`saves.list` in the console shows them).
3. First copy the files you are about to replace somewhere safe.
4. Copy each file back to the same name:
   - `steam\<id>\<file>` to `<Steam>\userdata\<id>\1203620\remote\<file>`
   - `local\default\<file>` to `%USERPROFILE%\Saved Games\keen games\Enshrouded\default\<file>`
   - `local\local_settings.json` to `%USERPROFILE%\Saved Games\keen games\Enshrouded\local_settings.json`
5. Put nothing else into those folders: the game moves files it does not know.

For Steam Cloud saves, the note above applies.

## Good to know

- **Restore files that belong together.** A character and `global.ksd` (the game's account-wide data) were saved
  together; restore them together when you can, and a world with the character you played it with.
- **A restore that stops part-way** (a save file in use by another program) says which files it put back. The files as
  they were before are in the backup marked "Before restore".
- **While EMF reads a save** (a few milliseconds per file), Windows does not let another program replace that file. EMF
  reads each file in one go and lets go of it at once, and the backup at game start runs before any world is loaded; a
  save that changes meanwhile is copied again, or named as not copied.
- **Steam Cloud's handling of restored files** has not been tried (see "Restoring with EMF.exe").
- **Your logs stay private.** `saves.list`'s lines show in the console but are not written to `emf\emf_core.log`, and EMF
  writes its folders as `%LOCALAPPDATA%\...`, not with your user name.
- Without `%LOCALAPPDATA%` (a very unusual Windows setup), EMF.exe keeps no backups and says so.

## Names and levels

The Saves tab names what is in each backup, such as "Steam: Ember (level 8), world Embervale". EMF learns the names
and levels when the game's main menu shows them (EMF's core writes them to `%LOCALAPPDATA%\EMF\save-info.json`, on game
build 1273043); before that it says "character slot 1" or "world slot 2". The restore list also shows when each world
was last saved and its play time. Quest progress is not shown: the save files are packed, and EMF found no simple,
reliable place for it.
