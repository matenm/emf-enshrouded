# Nexus Mods in EMF (src/nexus/)

The library behind the app's Browse page: it searches Enshrouded mods on Nexus Mods, reads a mod's page
and files, lists the paths inside a file's archive without downloading it, says what that file is and what the
player should know before installing it, and turns the mod's description into safe HTML. With the player's API key
it checks the key, reads `nxm://` links, downloads files and signs in with Nexus SSO (the last needs an application
slug, which Nexus issues to a registered app; the release build has none). The app's side (where the key is kept,
the `nxm://` handler setting, the page) is in `docs/app.md`.

| File | What |
|---|---|
| `api.h` | the GraphQL request bodies and the parsers for the replies (pure) |
| `client.h` | the requests over HTTPS, with the headers Nexus asks for, a cancel flag and a deadline (what the app calls) |
| `http.h` | a small WinHTTP client: one synchronous HTTPS request with timeouts, gzip unpacked, no redirects, 4 MiB at most, the reply's headers; and a streamed download (`http_stream`) that follows redirects only to `https://` |
| `classify.h` | what a download is, from its archive's paths, plus plain-English notes and clash warnings (pure) |
| `bbcode.h` | descriptions (BBCode) to HTML that is safe on its own (pure) |
| `nxm.h` | `nxm://` links from the website's Mod Manager Download button (pure) |
| `v1.h` | the REST API v1 calls that need the player's key: validate, a file's details, download links; the rate-limit headers |
| `download.h` | one file from Nexus into the downloads folder: details, link, the CDN, with the size checks |
| `sso.h` | single sign-on over a WebSocket (the protocol, tested with a fake socket; WinHTTP's WebSocket API for real) |
| `updates.h` | the update check: the batched request, its parser and the rules for update / can't tell / up to date / gone (pure) |
| `tools/nexus_cli/` | `emf_nexus`, the same from the command line |
| `tools/nexus/nexus_publish.py` | not part of EMF.exe: the release tool for mod authors, which uploads a mod's files, page text and changelog through the API v3 with the author's own key (usage: the comment at the top of the file) |

Tests: `tests/test_nexus_*.cpp`, with real replies saved in `tests/fixtures/nexus/` (fetched once and
trimmed; the tests make no network calls). The mod authors' descriptions, summaries and changelogs in them are replaced
with invented text in the same BBCode shapes. `test_nexus_updates.cpp` checks the update check's request texts and caps,
the recorded replies `updates_reply.json` (a hidden, a removed and a missing mod) and `changelogs_reply.json` (a
changelog line for each of two files), failed parts of a reply, version compares, the upload time in an archive's name,
file-name matching, every rule of `decide_update` on real mods (a page lagging its files, an archived beta, variants)
and on edge cases (a rollback, a hotfix within the hour, update files, a legacy build, a mod name shared
by variants), and `Client::updates` / `Client::changelogs` batching, cancel, deadline and a failed request with a fake
transport. `test_nexus_http_local.cpp` runs the real WinHTTP code against a small HTTP
server on 127.0.0.1: requests and streams take `allow_loopback_http`, a seam only tests set, and it shows that API
requests never follow a redirect and a cancelled download stops at once. The API v1 replies (`v1_*.json`) are written from Nexus's documented shapes,
except `v1_validate_401.json`, the real answer without a key.

## The API calls

All go to `POST https://api.nexusmods.com/v2/graphql` with `{"query": ..., "variables": ...}`. Public data needs no
key. User text only goes into variables, never into the query text.

| Call | Query | Gives |
|---|---|---|
| `search` | `mods(filter: {gameDomainName: enshrouded, name: WILDCARD <text>, categoryName}, sort, count, offset)` | mods with name, summary, version, author, uploader, downloads, endorsements, dates, category, pictures, adult flag; the total |
| `mod` | `mod(modId, gameId: 5811)` | the same plus the description (BBCode) |
| `files` | `modFiles(modId, gameId: 5811)` | each file's id, name, version, category (MAIN, UPDATE, OPTIONAL, OLD_VERSION, MISCELLANEOUS, REMOVED, ARCHIVED), size, date, primary flag |
| `contents` | `modFileContents(filter: {gameId, modId, fileId}, count, offset)` | the paths and sizes inside the file's archive |
| `updates` | `legacyModsByDomain(ids, count)` plus `modFiles` under an alias per mod (below) | for up to 50 mods: status, name, version, update time, and every file |
| `changelogs` | `modFilesByUid(uids, count)` (below) | the changelogs of up to 50 files to take |

What the replies taught us:
- `name` with `WILDCARD` matches words as typed: `loot` finds "Auto Loot Plus"; `*loot*` finds nothing.
- Sort keys: downloads, endorsements, updatedAt, createdAt, name (also relevance, uniqueDownloads, size, random).
- `sizeInBytes` and `fileSize` arrive as strings (BigInt); the parsers read numbers as numbers or strings.
- A query above complexity 10000 is refused (`QUERY_TOO_COMPLEX`); a page of 500 archive paths is 2000, so the
  client asks in pages of 500: at most 4 requests (2000 paths) per file, and a page shorter than 500 is the last,
  whatever `totalCount` says (one real listing counted 82 and sent 80).
- **Nexus does not list the contents of many newer files.** The current files of Creative Mode, Auto Loot Plus, Quick
  Deposit and ShroudForge (recent uploads) all came back empty, while older files of the same mods were listed. The page must expect "no list" and check the archive itself after downloading.
- A missing mod is a GraphQL error ("Mod not found") with `data: null`; errors are reported as text, never thrown.
  Lists (search, archive contents) keep what arrived when `errors` comes with data; one mod and its files do not.
- Python's urllib is refused by Cloudflare (403); curl and WinHTTP with a normal User-Agent work. (`nexus_publish.py`
  uses urllib with a User-Agent of its own for this reason. If Cloudflare still refuses, the tool says that the answer
  was a web page.)

## The rules we follow

- Every request says who it is: `Application-Name: EMF`, `Application-Version: <EMF's VERSION>` (never blank),
  `User-Agent: EMF/<version> (Enshrouded Modding Framework; Windows)`.
- Requests only when the player does something (opens the page, searches, opens a mod, looks at a file, clicks Check
  for updates). No background fetching, no copying Nexus's catalogue, no rehosting. A long archive listing is at most 4
  requests; an update check is 1 request per 50 installed mods, at most 4, plus 1 for the changelogs of the files to
  take, and after a failed one EMF waits a minute.
- Show Nexus's own data as it is (names, authors, pictures from Nexus's servers), and link to the mod's page.
- Downloads go through Nexus's own "Mod Manager Download" button (`nxm://`) or a Premium key, never by scraping.
- Nexus's acceptable-use rules for the API (help.nexusmods.com, article 114): personal API keys are only for testing and
  pre-release (a registered app signs in with SSO); a user's key is never kept on a server (EMF keeps it on the
  player's PC, DPAPI); requests only when the user acts; the right Application-Name and Application-Version; no mass
  fetching. The rate-limit headers are respected: when both allowances are used up, EMF stops asking.
- A public app must be registered with Nexus. The release build of EMF.exe has no application slug, so it makes no
  Nexus request at all; the tester switch turns everything on for testing (docs/app.md, "Nexus Mods: the three modes").

## The update check (updates.h)

EMF.exe's **Check for updates** (`docs/app.md`): one request for up to 50 installed mods, only when the player clicks.

```graphql
query EmfUpdates($ids: [CompositeDomainWithIdInput!]!, $count: Int, $gameId: ID!, $f170: ID!, $f195: ID!) {
  mods: legacyModsByDomain(ids: $ids, count: $count) { nodes { modId name version updatedAt status } }
  f170: modFiles(modId: $f170, gameId: $gameId) { fileId uid name version category date size sizeInBytes }
  f195: modFiles(modId: $f195, gameId: $gameId) { fileId uid name version category date size sizeInBytes }
}
```

with `{"ids": [{"gameDomain": "enshrouded", "modId": 170}, ...], "count": 2, "gameId": 5811, "f170": 170, "f195": 195}`.
The ids are variables; the query text only names them. Then, only when the check names files to take, their changelogs:

```graphql
query EmfChangelogs($uids: [ID!]!, $count: Int) {
  files: modFilesByUid(uids: $uids, count: $count) { nodes { uid fileId modId changelogText } }
}
```

| | |
|---|---|
| Requests | 1 per 50 mods, at most 4 per click (200 mods; the rest is reported as not checked), plus 1 for the changelogs of up to 50 files to take |
| Reply | about 50 KB of JSON for 50 mods with all their files; changelogs only for the files to take |
| Time limit | the whole check: one minute; a newer click cancels it |
| After a failure | no new check for a minute (the host refuses it; the page shows the wait) |

What the replies taught us (checked against Nexus, with these headers):
- `legacyModsByDomain(ids, count)` takes `{gameDomain, modId}` pairs and answers every mod Nexus has, published or not,
  with a `status`: `published`, `hidden`, `removed`, `wastebinned`, `not_published`, `under_moderation`. An id Nexus does
  not have is left out of the answer, without an error. Without `count`, only 20 come back. The order is not the order
  asked: the parser maps by `modId`.
- `modFiles` under an alias per mod works in one request; a mod Nexus does not have gives `[]` (no error), and a hidden
  or removed mod still lists its files. An error for one alias names it in `errors[].path`; that mod is "not checked",
  never a wrong answer. Aliases on `mod(...)` would not do: a missing mod there is an error that empties the whole
  `data` (`mod` is non-null).
- 50 mods with all their files passed Nexus's complexity limit of 10000. With `changelogText` on every file the reply
  was about 130 KB, two thirds of it changelogs; without, about 50 KB. So the changelogs come in a second request, for
  the files to take only.
- A file's `uid` is the game id shifted 32 bits plus the file id (24958054958253 for file 1197); `modFilesByUid` takes
  them and leaves out a uid it does not know.
- `ModFile.changelogText` is the file's changelog, one string per line; GraphQL has no address of a changelog page. The
  parser keeps 30 lines of 300 bytes at most.
- A Nexus download's name ends in the file's upload time (`...-170-1-1-8-1792790875.zip`, the `date` of file 1197), so
  an archive installed by hand still names its Nexus file.
- A mod's `version` and its main file's `version` often disagree, either way (a page saying 1.8 with 1.7 files, a page
  saying 1.1 with 1.2 files, or "1" with files named 2.5 and 3.0), and mods often have several main files (Standalone
  and ModLoader variants, server and client, builds for two game versions). So the rules (`decide_update`) compare the
  installed file when EMF knows it, take the newest version of the page and its main files otherwise, match file names
  by their words without numbers, treat a file uploaded within an hour at the same version as a variant, never offer an
  older file as an update, and say "can't tell" rather than guess: see `docs/app.md`, "Checking for updates".

## The API v1, with the player's key (v1.h)

`https://api.nexusmods.com/v1/`, every request a `GET` with the headers `apikey: <key>`, `Accept: application/json`,
`Application-Name: EMF`, `Application-Version: <EMF's VERSION>` and EMF's User-Agent. The key travels in its header only
(never in the address), the request never follows a redirect, and `V1Client` sends nothing for a key that is not 8 to
1024 printable characters without spaces.

| Call | Address | Gives |
|---|---|---|
| `validate` | `users/validate.json` | `user_id`, `name`, `is_premium`, `is_supporter` (also the older `is_premium?` spellings), `profile_url`. The reply also has `email` and `key`; neither is read |
| `file` | `games/enshrouded/mods/<mod>/files/<file>.json` | the file's `name`, `version`, `category_name`, `file_name` (the archive's name) and `size_in_bytes` (exact; can be `null` for old files) |
| `download_links` | `games/enshrouded/mods/<mod>/files/<file>/download_link.json` | `[{name, short_name, URI}]`: CDN addresses, Nexus's preferred first; only `https://` ones are kept. A Premium member asks without parameters; a free account only with `?key=<k>&expires=<e>` from an nxm link (escaped) |

Errors come back as text, never thrown: 401 "Nexus did not accept the API key" (with Nexus's message: without a key it
is "Please provide an authentication method"), 403 refused (a free account asking for a link without an
nxm key: "...this is for premium users only"), 404, 410 (a link's key that expired or is for another account), 429 and
5xx, and a reply that is not JSON.

**Rate limits.** Every reply has `X-RL-Daily-Limit/Remaining/Reset` and `X-RL-Hourly-Limit/Remaining/Reset`
(`parse_rate_limits`). Nexus allows a daily number of requests and, once that is used up, an hourly one; when both
remaining counts are 0 (`RateLimits::exhausted`), or a reply is 429, `rate_limit_message` says so with the reset times,
and the app makes no request until the hourly reset time has passed (`iso8601_to_unix`; five minutes when Nexus gave
none). The message shows the reset in the PC's own time ("opens again at 21:00"; `local_clock`). The app shows the
counts on the account card.

## nxm:// links (nxm.h)

`nxm://enshrouded/mods/<mod>/files/<file>?key=<k>&expires=<unix>&user_id=<id>`: `parse_nxm` takes the scheme and the
game without case, ignores a trailing slash, a fragment and parameters it does not know, and decodes `%XX` in the key
(`+` stays `+`). Refused, with the reason: anything that is not `nxm://`, a link longer than 2048 characters or with
spaces or control characters, another game (`other_game`, "This link is for another game (skyrimspecialedition)..."),
a collection, paths other than `mods/<id>/files/<id>`, ids that are not 1 to 12 digits, a key with characters keys
never have (base64 and URL-safe base64, `.`, `~`; 256 at most), an expiry or user that is not digits. A message never
repeats the link's query, so the one-time key stays out of the page. `nxm_expired` and `nexus_file_page` (the file's
Files tab, where a free account downloads) go with it.

## Downloads (download.h)

`Downloader::run(job)`: the file's details, then its link (with the nxm key and expiry when there are some), then the
file from the first CDN address with `http_stream`, written to `<dir>\<name>.part` and renamed to `<dir>\<name>`
(replacing an older download of that name) only when every byte is there. Checked on the way:
- the archive name must end in `.zip`, `.7z` or `.rar` (`safe_download_name`: the last part of Nexus's name, characters
  Windows refuses replaced by `_`, at most 160 characters, never a device name), else nothing is fetched;
- a known `size_in_bytes` above the 2 GB cap (the install pipeline's) refuses before the link; a `Content-Length` above
  the cap, or different from `size_in_bytes`, stops before the body; more bytes than either (or than the
  `Content-Length`) stops at once; a download that ends with another size than `size_in_bytes`, or without
  `size_in_bytes` shorter than the `Content-Length` (a connection that ended early), is deleted;
- redirects only to `https://` (`redirect_target`: absolute, `//host`, `/path` and relative addresses resolved; `http:`,
  other schemes, a user name or no host refused), at most five; the CDN request carries Application-Name/Version and
  the User-Agent, never the key;
- a 403 for a link without an nxm key says Premium is needed and the website's button is the way for free accounts; a
  403 or 410 with an nxm key says to click Mod Manager Download again; both allowances used up after the file's details
  stops before the link;
- the cancel flag is checked between reads, and a watcher closes the WinHTTP request as soon as it is set, so a
  transfer that hangs stops at once instead of after the 30-second timeout; a failure or a cancel removes the `.part`
  file.
Progress goes to a callback at most every 256 KiB (and at the end); the app turns it into at most five messages a second.

## Single sign-on (sso.h)

As Nexus's demo (github.com/Nexus-Mods/sso-integration-demo) does it: connect to `wss://sso.nexusmods.com`, send
`{"id": <uuid v4>, "token": null, "protocol": 2}`, receive `{"success":true,"data":{"connection_token":"..."}}`, open
`https://www.nexusmods.com/sso?id=<uuid>&application=<slug>` in the browser, receive
`{"success":true,"data":{"api_key":"..."}}`. A dropped connection is made again with the same id and the token instead
of `null` (three times at most). `{"success":false,"error":"..."}` ends it with Nexus's words. `run_sso` has a time
limit (five minutes) and a cancel flag; a watcher thread aborts the socket that waits, on every tick until it stops
(closing its WinHTTP handles makes a waiting call return; a handle made after the abort is closed at once). The socket is an interface: the tests drive the protocol with a fake one, and
`winhttp_sso_sockets` is WinHTTP's WebSocket API (`WinHttpWebSocketCompleteUpgrade`, messages put together from
fragments, binary messages ignored, 64 KiB at most). `kSsoSlug` is empty in the release build; `run_sso`
refuses without one, and the app hides the button (and, without the tester switch, every Nexus feature). `scripts/sso_selftest.ps1` runs the real WebSocket client against a
local fake of the service (`scripts/sso_fake_server.py`, `ws://127.0.0.1` only): the hello, the token, the browser
address, a key in two fragments, a dropped connection made again with the token, and the time limit aborting a socket
that waits.

## Calling it from the app (client.h)

Every call is synchronous; run it on a worker thread. Each takes an optional cancel flag and an overall deadline
(30 s by default), checked before every request and between reads:

```cpp
std::atomic<bool> cancel{false};             // set it from the UI thread to stop
Client nexus(EMF_VERSION);
auto page = nexus.search(query, &cancel);    // or (query, &cancel, std::chrono::seconds(10))
auto list = nexus.contents(mod, file, 2000, &cancel);
// page.error: "" on success, "cancelled", "timed out", "the reply was too large",
// "could not reach Nexus: ...", or Nexus's own message
```

A WinHTTP call that is already waiting runs until its own timeout, which is never longer than the time left before
the deadline; cancelling takes effect at the next check. Each request opens its own connection, and the shared JSON reader refuses
lone UTF-16 surrogates in JSON escapes.

## What a download is (classify.h)

`classify_archive(paths, game)` returns a kind, other layouts found in the same archive (`also`), the mod folders
and where they go, the proxy DLLs at the top, a wrapper folder to strip, and notes. The full rules are in
`classify.h`; in short, the first match wins:

1. EMF itself (`emf/emf_core.dll`, or `game/emf/emf_core.dll` in EMF's release zip); another loader by its own
   folder or file next to a proxy DLL (`shroudforge/`, `shroudtopia.json`, `chainloader.ini`, `aldrich-modmenu/`,
   EML's `emm.exe`);
2. EMF mod (`emf/mods/<id>/`), Greeneye mod (`greeneye/<id>/`);
3. DLL mod named `dinput8.dll` at the top: EMF starts it through `dinput8-chain.ini`;
4. ReShade preset; another proxy DLL at the top (`dbghelp`, `dbgcore`, `winmm`, `powrprof`, `version`, `xinput1_4`,
   `dxgi`, ...);
5. Shroudtopia mod (`mods/<id>/` with a DLL), ShroudForge mod (EML layout plus `extended.mod.json`), EML mod
   (`mod.json` plus `src/mod.lua` in `mods/<id>/`, `<id>/` or the top), EMF Lua mod by layout (`<id>/mod.json` with
   `main.lua`);
6. save files (`*.ksd`, the `characters` file, or a world file with its `_info` file), Cheat Engine table, unknown.
   `<id>/mod.json` next to a DLL stays unknown, with a note that it could be an EMF, Greeneye or Shroudtopia mod
   packed without its parent folder, which EMF checks after the download.

A single top folder that is not one of the game's own (`emf`, `mods`, `greeneye`, `shroudforge`, `reshade-shaders`,
`aldrich-modmenu`) is a wrapper and is stripped (two levels at most). Names lose trailing dots and spaces, as
Windows stores them. Unsafe paths are reported and left out: `unsafe_archive_path(path)` (absolute paths, drives,
`.` and `..`, `name:stream`, control characters and `<>"|?*`, reserved device names such as `CON` or `nul.txt`) is
the one check the installer uses too.

`game` is what is next to the game now, in the shape and tool words of `core::GameFolderView`: `proxies`
(`{"dbghelp.dll", "eml"}`), `known` (the app looked) and `chained` (tools EMF starts through `dinput8-chain.ini`,
which count as installed). With it the notes say what is installed and what would be replaced ("dbghelp.dll is
EML's now: installing this replaces it, and EML stops loading").

## Descriptions (bbcode.h)

`bbcode_to_html(text, image_hosts)` handles the tags Nexus descriptions use (list in `bbcode.h`), Nexus's
`\n<br />` line breaks and HTML entities (also `&rsquo; &hellip; &mdash; &copy;` and the like). All text is escaped;
links must be http(s); unknown tags stay as text; an `[url=...]` that is never closed is text, so it cannot turn
the rest of the page into a link. Only the first 256 KiB is converted. For the page:
- style the classes `bb-quote`, `bb-code` (on `<code>` and `<pre>`), `bb-spoiler` (a `<details>`);
- colours, sizes, fonts and alignment are inline `style` attributes with checked values; the page allows them with
  `style-src-attr 'unsafe-inline'` (no page-wide `'unsafe-inline'`, nothing needs scripts);
- links have `target="_blank" rel="noopener noreferrer nofollow"`; the app should open them in the browser;
- images are shown only over https from `image_hosts` (by default `staticdelivery.nexusmods.com`,
  `raw.githubusercontent.com`, `user-images.githubusercontent.com`, `i.imgur.com`), with
  `referrerpolicy="no-referrer"`; any other image is a link with the text "image";
- YouTube videos are links, not embedded players.

## emf_nexus

```
emf_nexus search <text> [count] [--sort downloads|endorsements|updated|created|name] [--asc]
emf_nexus mod <modId> [--html]
emf_nexus files <modId>
emf_nexus contents <modId> <fileId> [--owner dbghelp.dll=eml ...]
emf_nexus classify <saved listing.json> [--owner ...]     (no network)
emf_nexus nxm <nxm://link>                                (no network; the key is not printed)
emf_nexus sso-selftest ws://127.0.0.1:<port>/ [seconds]   (a local fake of the login service only)
```
