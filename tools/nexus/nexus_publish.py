#!/usr/bin/env python3
r"""Publish a mod's releases on Nexus Mods through its API v3: upload files, update the page text, add changelogs.

A release in three commands, with a release config (below):

    powershell -ExecutionPolicy Bypass -File scripts\package.ps1
    python tools\nexus\nexus_publish.py release <release.json>         (dry run: prints the plan)
    python tools\nexus\nexus_publish.py release <release.json> --yes   (does it)

A release config is one JSON object: game, mod_id, root (relative to the config file; the other paths are relative
to it), version or version_file, summary, description_file, files (each with name, file, category, description,
allow_mod_manager_download, update_mod_version, archive_old, and file_id or existing_names to add a version to a file
already on the page; {version} in name and file is the release's version) and changelog ({version: [entries]}). Keys
that start with _ are notes.

Commands:
    whoami                         checks the key: the account's name and whether it is Premium
    files <game>/<mod>             the mod's files: id, name, latest version, category
    upload <game>/<mod> --file <zip> --name <name> --version <v> --category main|optional|miscellaneous
           [--description <text or @file>] [--replace <file id or name> [--archive-old]]
           [--no-mod-manager-download] [--update-mod-version]
    page <game>/<mod> [--summary <text or @file>] [--description <text or @file>]
    changelog <game>/<mod> --version <v> --entries <@file or text>     (one entry per line)
    release <config.json>          a whole release from a config file

<game>/<mod> is the end of the mod page's address: enshrouded/123 for https://www.nexusmods.com/enshrouded/mods/123
(the whole address works too).

Safety:
- A command that changes something prints its plan and stops, unless --yes is given.
- The key comes from the environment variable NEXUS_API_KEY or from --key-file <path>. It is never printed, logged,
  written to a file or put in an address; it travels only in the apikey header, only to api.nexusmods.com, and never
  with a redirect (redirects are not followed). The upload to the storage's presigned address carries no key.
- Names, versions, the files (.zip/.7z/.rar, at most 100 MiB) and the summary (350 characters) are checked against
  the API's rules before the first request.

Python 3, standard library only. Exit codes: 0 done (or a dry run with nothing missing), 1 Nexus, the storage or the
network failed, 2 refused (a check failed, no key, the config is not ready).
"""

import argparse
import base64
import email.utils
import hashlib
import html
import json
import os
import re
import sys
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
from decimal import Decimal, InvalidOperation

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))

API_BASE = "https://api.nexusmods.com/v3"   # the spec's servers entry (https://api.nexusmods.com/openapi.yaml)
V1_BASE = "https://api.nexusmods.com/v1"    # only for whoami (users/validate) and reading changelogs
KEY_PAGE = "https://www.nexusmods.com/settings/api-keys"
APP_NAME = "EMF-publish"
KEY_ENV = "NEXUS_API_KEY"

# The API's rules (CreateModFileRequest, CreateModFileVersionRequest, EditModRequest, AddModChangelogEntriesRequest).
MAX_SINGLE_PART = 100 * 1024 * 1024         # larger files need a multipart upload, which this tool does not do yet
NAME_MAX = 50
NAME_RE = re.compile(r"^[a-zA-Z0-9 _'().-]+$")
VERSION_MAX = 50
VERSION_RE = re.compile(r"^[a-zA-Z0-9.-]+$")
SUMMARY_MAX = 350
CHANGELOG_MAX = 65535
ARCHIVE_EXTS = (".zip", ".7z", ".rar")
CATEGORIES = ("main", "optional", "miscellaneous")   # NewModFileCategory
GAME_RE = re.compile(r"^[a-z0-9]{1,64}$")
MOD_RE = re.compile(r"^[0-9]{1,12}$")
KEY_SHAPE = re.compile(r"^[\x21-\x7e]{8,1024}$")
UPLOAD_ID_RE = re.compile(r"^[0-9A-Za-z-]{1,64}$")

API_TIMEOUT = 60            # seconds per API request
PUT_TIMEOUT = 600           # seconds for the upload of one file
MAX_REPLY = 8 * 1024 * 1024
RETRIES = 4                 # after a 429 (or a 5xx on a request that is safe to repeat)
MAX_RETRY_WAIT = 300        # a Retry-After longer than this ends the command instead of waiting
DEFAULT_WAIT = 600          # seconds to wait for Nexus to process an upload
POLL_FIRST = 2.0
POLL_MAX = 15.0
LOW_REQUESTS = 20           # below this many requests left, say so once

_ALLOW_LOOPBACK_HTTP = False   # tests only: plain http to 127.0.0.1 (the fake Nexus); never set by the command line
_sleep = time.sleep            # tests replace both with a fake clock
_now = time.monotonic
_SECRETS = []                  # the key, scrubbed from everything this tool writes


class Refused(Exception):
    """A check before anything was sent failed (exit 2)."""

    def __init__(self, problems):
        self.problems = [problems] if isinstance(problems, str) else list(problems)
        super().__init__("\n".join(self.problems))


class NexusError(Exception):
    """Nexus, the storage or the network failed (exit 1)."""


# ---------------------------------------------------------------------------------------------------------- output

def _scrub(text):
    text = str(text)
    for secret in _SECRETS:
        if secret:
            text = text.replace(secret, "<the key>")
    return text


def _write(stream, text):
    text = _scrub(text)
    enc = getattr(stream, "encoding", None) or "utf-8"
    try:
        text.encode(enc)
    except UnicodeEncodeError:
        text = text.encode(enc, "replace").decode(enc)
    except LookupError:
        text = text.encode("ascii", "replace").decode("ascii")
    stream.write(text + "\n")
    stream.flush()


def say(text=""):
    _write(sys.stdout, text)


def warn(text):
    _write(sys.stderr, text)


def clean(value, limit=500):
    """Text that came from a server, safe to print: no control characters, one line, at most `limit` characters."""
    text = "" if value is None else str(value)
    text = " ".join(re.sub(r"[\x00-\x1f\x7f]", " ", text).split())
    return text if len(text) <= limit else text[:limit] + "..."


def short(text, limit=60):
    text = " ".join(str(text).split())
    return text if len(text) <= limit else text[:limit - 3] + "..."


def size_text(n):
    return "{:,} bytes ({:.1f} MiB)".format(n, n / (1024 * 1024))


def tool_version():
    """EMF's VERSION, sent as Application-Version (never blank)."""
    try:
        with open(os.path.join(REPO, "VERSION"), encoding="utf-8-sig") as f:
            v = f.readline().strip()
        if v and re.match(r"^[0-9A-Za-z.+-]{1,32}$", v):
            return v
    except OSError:
        pass
    return "0.0.0"


# ---------------------------------------------------------------------------------------------------------- the key

def key_help():
    return ("No Nexus API key. Copy your personal API key from " + KEY_PAGE + ", then either:\n"
            '    setx NEXUS_API_KEY "<key>"\n'
            "  and open a new terminal (setx only reaches terminals started after it), or save the key alone in a file\n"
            "  outside the repository and add --key-file <path>. The tool never prints the key, never writes it\n"
            "  anywhere and sends it only to api.nexusmods.com.")


def _git_root_of(path):
    d = os.path.dirname(os.path.abspath(path))
    while True:
        if os.path.exists(os.path.join(d, ".git")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


def user_env_key():
    """Windows: NEXUS_API_KEY from the user's saved environment (HKCU\\Environment), which `setx` writes. A terminal
    opened before `setx` does not have it in its own environment. None elsewhere or when it is not saved."""
    if os.name != "nt":
        return None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment") as k:
            value, _ = winreg.QueryValueEx(k, KEY_ENV)
        return str(value) if value else None
    except OSError:
        return None


def load_key(key_file=None):
    """The key from --key-file or NEXUS_API_KEY; None when neither is set. Refused when it cannot be a key."""
    if key_file:
        try:
            with open(key_file, encoding="utf-8-sig") as f:
                key = f.read().strip()
        except OSError as e:
            raise Refused("cannot read the key file %s: %s" % (key_file, e.strerror or type(e).__name__))
        where = "the key file " + key_file
        root = _git_root_of(key_file)
        if root:
            warn("note: the key file is inside the git repository %s. Keep it out of commits (.gitignore), or "
                 "better, move it outside." % root)
    else:
        key = (os.environ.get(KEY_ENV) or "").strip()
        where = KEY_ENV
        if not key:
            key = (user_env_key() or "").strip()  # saved by setx after this terminal started
            where = KEY_ENV + " (from your saved Windows settings)"
        if not key:
            return None
    if key:
        _SECRETS.append(key)
    if not key:
        raise Refused(where + " is empty.\n" + key_help())
    if not KEY_SHAPE.match(key):
        raise Refused("%s does not look like a Nexus API key: a key is one line of 8 to 1024 visible characters, "
                      "without spaces. Copy it again from %s." % (where, KEY_PAGE))
    return key


def require_key(key_file):
    key = load_key(key_file)
    if not key:
        raise Refused(key_help())
    return key


# ---------------------------------------------------------------------------------------------------------- checks

def check_name(name, problems, label="name"):
    if not name or not name.strip():
        problems.append("%s is empty" % label)
        return
    if len(name) > NAME_MAX:
        problems.append('%s "%s" is %d characters long; Nexus allows %d' % (label, name, len(name), NAME_MAX))
    if not NAME_RE.match(name):
        bad = " ".join(sorted(set(c for c in name if not NAME_RE.match(c))))
        problems.append("%s \"%s\" has characters Nexus does not allow in file names: %s (allowed: letters, digits, "
                        "spaces and _ ' ( ) . -)" % (label, name, bad))


def check_version(version, problems, label="version"):
    if not version:
        problems.append("%s is empty" % label)
        return
    if len(version) > VERSION_MAX:
        problems.append('%s "%s" is %d characters long; Nexus allows %d' % (label, version, len(version),
                                                                           VERSION_MAX))
    if not VERSION_RE.match(version):
        problems.append('%s "%s" may only have letters, digits, dots and hyphens (for example 1.2.0)'
                        % (label, version))


def check_category(category, problems):
    if category not in CATEGORIES:
        problems.append('category "%s" is not one Nexus takes for a new file or version: %s'
                        % (category, ", ".join(CATEGORIES)))


def check_archive(path, problems, shown=None):
    """The file's size when it can be uploaded; otherwise a problem is added and None returned."""
    shown = shown or path
    if not os.path.exists(path):
        problems.append("file not found: %s" % shown)
        return None
    if not os.path.isfile(path):
        problems.append("not a file: %s" % shown)
        return None
    base = os.path.basename(path)
    if not base.lower().endswith(ARCHIVE_EXTS):
        problems.append("%s is not a .zip, .7z or .rar archive" % shown)
        return None
    if not re.match(r"^[\x20-\x7e]+$", base) or '"' in base or "\\" in base:
        problems.append("the archive's name %s must be plain ASCII without quotes: Nexus puts it in a header" % base)
        return None
    size = os.path.getsize(path)
    if size == 0:
        problems.append("%s is empty" % shown)
        return None
    if size > MAX_SINGLE_PART:
        problems.append("%s is %.1f MiB; single uploads stop at 100 MiB, and this tool does not do multipart "
                        "uploads (POST /uploads/multipart) yet: upload it on the website" % (shown, size / 1048576))
        return None
    return size


def collapse_summary(text):
    """The summary as Nexus stores it: line breaks and runs of spaces become one space."""
    return " ".join(text.split())


def check_summary(text, problems):
    summary = collapse_summary(text)
    if not summary:
        problems.append("the summary is empty")
    elif len(summary) > SUMMARY_MAX:
        problems.append("the summary is %d characters long; Nexus allows %d" % (len(summary), SUMMARY_MAX))
    return summary


def read_text_arg(value, option):
    """`@path` reads the file (UTF-8); anything else is the text itself."""
    if value is None:
        return None
    if value.startswith("@"):
        path = value[1:]
        try:
            with open(path, encoding="utf-8-sig") as f:
                return f.read()
        except OSError as e:
            raise Refused("%s: cannot read %s: %s" % (option, path, e.strerror or type(e).__name__))
        except UnicodeDecodeError:
            raise Refused("%s: %s is not UTF-8 text" % (option, path))
    return value


def changelog_entries(lines):
    """One entry per line: blank lines dropped, a leading "- " or "* " (list marks) taken off."""
    out = []
    for line in lines:
        line = re.sub(r"^\s*[-*]\s+", "", line).strip()
        if line:
            out.append(line)
    return out


def check_wait(wait, problems):
    if wait is None:
        return DEFAULT_WAIT
    if wait < 1 or wait > 3 * 3600:
        problems.append("--wait must be 1 to 10800 seconds")
    return wait


def file_digest(path):
    """(md5 hex digest, size) of a file, read in chunks."""
    try:
        h = hashlib.md5(usedforsecurity=False)
    except TypeError:
        h = hashlib.md5()
    size = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
            size += len(chunk)
    return h.hexdigest(), size


def content_md5(md5_hex):
    """The Content-MD5 header: the same digest in base64."""
    return base64.b64encode(bytes.fromhex(md5_hex)).decode("ascii")


def parse_target(text):
    """'enshrouded/123', 'enshrouded/mods/123' or the page's address -> ('enshrouded', '123')."""
    m = re.match(r"^\s*(?:https?://(?:www\.)?nexusmods\.com/)?([A-Za-z0-9]+)/(?:mods/)?([0-9]+)/?(?:[?#].*)?\s*$",
                 text or "")
    if not m or not GAME_RE.match(m.group(1).lower()) or not MOD_RE.match(m.group(2)):
        raise Refused('"%s" is not a mod: write <game>/<mod>, for example enshrouded/123 (the number in the mod '
                      "page's address)" % text)
    return m.group(1).lower(), m.group(2)


def q(part):
    return urllib.parse.quote(str(part), safe="")


# ---------------------------------------------------------------------------------------------------------- HTTP

class Reply:
    def __init__(self, status, headers, body):
        self.status, self.headers, self.body = status, headers, body

    def json(self, what):
        try:
            return json.loads(self.body.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            raise NexusError("Nexus's answer to %s was not JSON (HTTP %d)" % (what, self.status))

    def data(self, what):
        doc = self.json(what)
        if not isinstance(doc, dict) or not isinstance(doc.get("data"), dict):
            raise NexusError("Nexus's answer to %s did not have the expected data (HTTP %d)" % (what, self.status))
        return doc["data"]


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Redirects are never followed: the 3xx comes back as an error, so a header never travels anywhere else."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _open(req, timeout):
    host = urllib.parse.urlsplit(req.full_url).hostname or ""
    handlers = [_NoRedirect()]
    if host in ("127.0.0.1", "localhost"):
        handlers.append(urllib.request.ProxyHandler({}))
    return urllib.request.build_opener(*handlers).open(req, timeout=timeout)


def _is_api(url):
    return url.startswith(API_BASE + "/") or url.startswith(V1_BASE + "/")


def _check_address(url, auth):
    parts = urllib.parse.urlsplit(url)
    host = parts.hostname or ""
    if parts.username or parts.password:
        raise NexusError("refusing an address with a user name in it (%s)" % clean(host, 80))
    plain_ok = _ALLOW_LOOPBACK_HTTP and parts.scheme == "http" and host in ("127.0.0.1", "localhost")
    if parts.scheme != "https" and not plain_ok:
        raise NexusError("refusing to send anything to %s://%s: only https addresses are used"
                         % (clean(parts.scheme, 20), clean(host, 80)))
    if auth and not _is_api(url):
        raise NexusError("internal error: the key is only ever sent to the Nexus API")


def _retry_after(headers):
    value = (headers.get("Retry-After") or "").strip() if headers else ""
    if not value:
        return None
    try:
        return max(1.0, float(value))
    except ValueError:
        pass
    try:
        when = email.utils.parsedate_to_datetime(value)
        return max(1.0, when.timestamp() - time.time())
    except (TypeError, ValueError, IndexError, OverflowError):
        return None


STATUS_HINTS = {
    401: "Nexus did not accept the key. Check it with: nexus_publish.py whoami (a new key: %s)" % KEY_PAGE,
    403: "The account behind the key may not be on this mod's team, or the mod is under moderation.",
    404: "Check <game>/<mod> and the ids: the mod's number is the one in its page's address.",
}


def problem_message(status, body, what, host, api):
    """A plain message from an error reply: RFC 9457 Problem Details (title, detail, errors), the v1 API's
    {"message"}, the storage's XML, or a web page in front of the API. Never the request's headers."""
    text = body.decode("utf-8", "replace") if body else ""
    who = "Nexus" if api else clean(host, 80)
    if status >= 500:
        head = "%s had a problem while %s (HTTP %d)" % (who, what, status)
    else:
        head = "%s refused %s (HTTP %d)" % (who, what, status)
    doc = None
    try:
        doc = json.loads(text) if text.strip() else None
    except ValueError:
        doc = None
    lines = []
    if isinstance(doc, dict):
        parts = [clean(doc.get(k), 300) for k in ("title", "detail") if doc.get(k)]
        if not parts:
            parts = [clean(doc.get(k), 300) for k in ("message", "error") if isinstance(doc.get(k), str)]
        if parts and len(parts) == 2 and parts[0].rstrip(".") == parts[1].rstrip("."):
            parts = parts[:1]
        lines.append(head + (": " + ": ".join(parts) if parts else ""))
        errors = doc.get("errors")
        if isinstance(errors, list):
            for item in errors[:10]:
                if isinstance(item, dict):
                    where = clean(item.get("pointer") or item.get("field") or "", 80).lstrip("/")
                    lines.append("  - %s%s" % (where + ": " if where else "", clean(item.get("detail") or
                                                                                   item.get("message"), 200)))
                else:
                    lines.append("  - " + clean(item, 200))
        elif isinstance(errors, dict):
            for field, msgs in list(errors.items())[:10]:
                msgs = msgs if isinstance(msgs, list) else [msgs]
                lines.append("  - %s: %s" % (clean(field, 80), "; ".join(clean(m, 200) for m in msgs)))
    elif re.search(r"<html|<!doctype", text[:2000], re.I):
        lines.append(head + ": the answer was a web page, not the API's. Cloudflare (Nexus's front door) may have "
                     "blocked the request; wait a few minutes and try again, or try from another network.")
    elif "<Error>" in text or text.lstrip().startswith("<?xml"):
        code = re.search(r"<Code>([^<]*)</Code>", text)
        message = re.search(r"<Message>([^<]*)</Message>", text)
        parts = [clean(m.group(1), 200) for m in (code, message) if m]
        lines.append(head + (": " + ": ".join(parts) if parts else ""))
    elif text.strip():
        lines.append(head + ": " + clean(text, 300))
    else:
        lines.append(head + " without saying why")
    if api and status in STATUS_HINTS:
        lines.append(STATUS_HINTS[status])
    if not api and status in (400, 403):
        lines.append("The storage checks the archive's name and md5 against the upload session; if the file changed "
                     "since then, run the command again.")
    return "\n".join(lines)


class Api:
    """The requests, with Nexus's headers, retries after 429 and plain error messages."""

    def __init__(self, key, version=None):
        self.key = key
        self.version = version or tool_version()
        self.limits = {}
        self.low_said = False

    def _headers(self):
        return {"Accept": "application/json",
                "Application-Name": APP_NAME,
                "Application-Version": self.version,
                "User-Agent": "%s/%s (Enshrouded Modding Framework release tool)" % (APP_NAME, self.version)}

    def v3(self, method, path, body=None, what="a request"):
        headers = self._headers()
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        elif method in ("POST", "PUT", "PATCH"):
            data = b""
        return self._send(method, API_BASE + path, data, headers, what, auth=True, repeatable=(method == "GET"))

    def v1(self, path, what):
        return self._send("GET", V1_BASE + path, None, self._headers(), what, auth=True, repeatable=True)

    def put_presigned(self, url, data, filename, md5_b64, what):
        headers = {"Content-Disposition": 'attachment; filename="%s"' % filename,
                   "Content-MD5": md5_b64,
                   "Content-Type": "application/octet-stream",
                   "User-Agent": self._headers()["User-Agent"]}
        return self._send("PUT", url, data, headers, what, auth=False, repeatable=True, timeout=PUT_TIMEOUT)

    def _send(self, method, url, data, headers, what, auth, repeatable, timeout=API_TIMEOUT):
        _check_address(url, auth)
        host = urllib.parse.urlsplit(url).hostname or ""
        api = _is_api(url)
        tries = 0
        while True:
            req = urllib.request.Request(url, data=data, method=method)
            for name, value in headers.items():
                req.add_header(name, value)
            if auth:
                req.add_unredirected_header("apikey", self.key)
            try:
                resp = _open(req, timeout)
                try:
                    status, reply_headers, body = resp.status, resp.headers, resp.read(MAX_REPLY + 1)
                finally:
                    resp.close()
            except urllib.error.HTTPError as e:
                status, reply_headers = e.code, e.headers
                try:
                    body = e.read(MAX_REPLY + 1)
                except Exception:
                    body = b""
                finally:
                    e.close()
            except urllib.error.URLError as e:
                raise NexusError("could not reach %s while %s: %s" % (clean(host, 80), what,
                                                                     clean(getattr(e, "reason", e), 200)))
            except OSError as e:   # timeouts and dropped connections
                raise NexusError("no answer from %s while %s (%s)" % (clean(host, 80), what,
                                                                     clean(e, 200) or type(e).__name__))
            if api:
                self._note_limits(reply_headers)
            if len(body) > MAX_REPLY:
                raise NexusError("the answer to %s was larger than %d MiB" % (what, MAX_REPLY // 1048576))
            again = status == 429 or (repeatable and status in (500, 502, 503, 504))
            if again and tries < RETRIES:
                wait = _retry_after(reply_headers)
                if wait is None:
                    wait = 5.0 * (2 ** tries)
                if wait > MAX_RETRY_WAIT:
                    raise NexusError("%s says to wait %d minutes before %s; nothing more was sent. Run the "
                                     "command again later.%s" % ("Nexus" if api else clean(host, 80),
                                                                 round(wait / 60), what, self._limits_suffix()))
                tries += 1
                why = "too many requests (429)" if status == 429 else "HTTP %d" % status
                warn("    %s says %s while %s; waiting %g s, then try %d of %d"
                     % ("Nexus" if api else clean(host, 80), why, what, wait, tries + 1, RETRIES + 1))
                _sleep(wait)
                continue
            if status == 429:
                raise NexusError("Nexus still says too many requests (429) after %d tries while %s; nothing more "
                                 "was sent. Run the command again later.%s" % (RETRIES + 1, what,
                                                                               self._limits_suffix()))
            if 300 <= status < 400:
                raise NexusError("%s answered %s with a redirect (HTTP %d). The tool does not follow redirects, so "
                                 "nothing was sent anywhere else." % (clean(host, 80), what, status))
            if status >= 400:
                raise NexusError(problem_message(status, body, what, host, api))
            return Reply(status, reply_headers, body)

    def _note_limits(self, headers):
        if not headers:
            return
        for name in ("X-RL-Daily-Remaining", "X-RL-Hourly-Remaining", "X-RL-Daily-Reset", "X-RL-Hourly-Reset",
                     "RateLimit-Remaining", "X-RateLimit-Remaining"):
            value = headers.get(name)
            if value is not None:
                self.limits[name] = clean(value, 64)
        left = [int(v) for k, v in self.limits.items() if k.endswith("Remaining") and v.isdigit()]
        if left and max(left) < LOW_REQUESTS and not self.low_said:
            self.low_said = True
            warn("    note: Nexus says only %d requests are left for now.%s" % (max(left), self._limits_suffix()))

    def limits_text(self):
        parts = []
        if "X-RL-Daily-Remaining" in self.limits:
            parts.append("%s today" % self.limits["X-RL-Daily-Remaining"])
        if "X-RL-Hourly-Remaining" in self.limits:
            parts.append("%s this hour" % self.limits["X-RL-Hourly-Remaining"])
        for name in ("RateLimit-Remaining", "X-RateLimit-Remaining"):
            if name in self.limits:
                parts.append("%s (%s)" % (self.limits[name], name))
        text = "Requests left: " + ", ".join(parts) if parts else ""
        for name, which in (("X-RL-Hourly-Reset", "hourly"), ("X-RL-Daily-Reset", "daily")):
            if text and name in self.limits:
                return text + " (the %s allowance resets at %s)" % (which, self.limits[name])
        return text

    def _limits_suffix(self):
        text = self.limits_text()
        return " " + text + "." if text else ""


# ---------------------------------------------------------------------------------------------------------- Nexus

def resolve_mod(api, game, mod):
    """The mod's API id (not the number in its address), name and version."""
    what = "reading the mod %s/%s" % (game, mod)
    d = api.v3("GET", "/games/%s/mods/%s" % (q(game), q(mod)), what=what).data(what)
    if not d.get("id"):
        raise NexusError("Nexus's answer to %s had no mod id" % what)
    return {"id": str(d["id"]), "name": clean(d.get("name"), 120), "version": clean(d.get("version"), 60),
            "game": game, "mod": mod}


def _latest(versions):
    def order(v):
        try:
            pos = Decimal(str(v.get("position")))
        except (InvalidOperation, ValueError):
            pos = Decimal(-1)
        return (pos, str(v.get("uploaded_at") or ""))
    return max(versions, key=order) if versions else None


def list_files(api, mod_id):
    """Every file of the mod with its latest version (the end of its update chain)."""
    what = "listing the mod's files"
    d = api.v3("GET", "/mods/%s/files" % q(mod_id), what=what).data(what)
    out = []
    for f in d.get("mod_files") or []:
        if not isinstance(f, dict) or f.get("id") is None:
            continue
        fid = str(f["id"])
        what_v = "reading the versions of file %s" % fid
        vd = api.v3("GET", "/mod-files/%s/versions" % q(fid), what=what_v).data(what_v)
        versions = [v for v in (vd.get("versions") or []) if isinstance(v, dict)]
        latest = _latest(versions) or {}
        out.append({"id": fid, "name": clean(f.get("name"), 120),
                    "version": clean(latest.get("version"), 60), "category": clean(latest.get("category"), 30),
                    "version_id": str(latest["id"]) if latest.get("id") is not None else None,
                    "version_name": clean(latest.get("name"), 120), "versions": len(versions),
                    "active": bool(f.get("is_active", True))})
    return out


def find_file(files, ref, by_id_only=False):
    """The file named by id or by name (no case); None when none matches."""
    ref = str(ref).strip()
    if ref.isdigit():
        hits = [f for f in files if f["id"] == ref]
        if hits or by_id_only:
            return hits[0] if hits else None
    low = ref.casefold()
    hits = [f for f in files if f["name"].casefold() == low]
    if not hits:
        hits = [f for f in files if f["version_name"].casefold() == low]
    if len(hits) > 1:
        active = [f for f in hits if f["active"]]
        if len(active) == 1:
            return active[0]
        raise Refused('%d files are named "%s" (ids %s): name the one you mean by its id (upload --replace <id>, or '
                      '"file_id" in a release config)' % (len(hits), ref, ", ".join(f["id"] for f in hits)))
    return hits[0] if hits else None


def files_list_text(files):
    if not files:
        return "  (the mod has no files yet)"
    return "\n".join('  %s "%s" (%s, %s)' % (f["id"], f["name"], f["version"] or "no version", f["category"] or "?")
                     for f in files)


def _norm_entry(text):
    return " ".join(html.unescape(str(text)).split()).casefold()


def read_changelog(api, game, mod):
    """The mod's changelogs {version: [entries]} from the v1 API, to leave out entries that are already there (adding
    is additive on Nexus). None when it could not be read: then every entry is sent."""
    try:
        doc = api.v1("/games/%s/mods/%s/changelogs.json" % (q(game), q(mod)), "reading the changelog").json(
            "reading the changelog")
    except NexusError as e:
        warn("    note: could not read the mod's changelog, so every entry will be sent (%s)"
             % clean(str(e).splitlines()[0], 200))
        return None
    if isinstance(doc, list):    # an empty changelog comes as []
        return {}
    if not isinstance(doc, dict):
        return None
    return {str(k): [_norm_entry(e) for e in (v if isinstance(v, list) else [v])] for k, v in doc.items()}


def new_entries(entries, existing, version):
    if not existing:
        return list(entries)
    have = set(existing.get(version, []))
    return [e for e in entries if _norm_entry(e) not in have]


# ---------------------------------------------------------------------------------------------------------- uploads

class FileJob:
    """One archive to upload: as a new file, or as a new version of an existing one (`target`)."""

    def __init__(self, path, name, version, category, description=None, allow_mod_manager_download=True,
                 update_mod_version=False, archive_old=False, replace=None, file_id=None, shown=None,
                 existing_names=None):
        self.path = path
        self.shown = shown or path
        self.filename = os.path.basename(path)
        self.name = name
        self.version = version
        self.category = category
        self.description = description
        self.allow_mm = allow_mod_manager_download
        self.update_mod_version = update_mod_version
        self.archive_old = archive_old
        self.replace = replace          # a file id or name (upload --replace)
        self.file_id = file_id          # a file id (the release config's file_id)
        self.existing_names = list(existing_names or [])   # other names the file has on the page (only to find it)
        self.size = None
        self.md5 = None
        self.target = None              # the existing file, or None for a new file
        self.skip = None                # why this upload is left out
        self.notes = []

    def check(self, problems, label="name"):
        check_name(self.name, problems, label)
        check_version(self.version, problems)
        check_category(self.category, problems)
        if check_archive(self.path, problems, self.shown) is not None:
            self.md5, self.size = file_digest(self.path)

    def match(self, files):
        """Find the existing file this upload continues: the release config's file_id, upload --replace, the same
        name or one of `existing_names` (a file uploaded on the website under a name the API does not take, such as
        one with a "+"). Refused when an id or --replace names no file."""
        if self.file_id:
            self.target = find_file(files, self.file_id, by_id_only=True)
            if not self.target:
                raise Refused("file_id %s of \"%s\" is not one of the mod's files:\n%s"
                              % (self.file_id, self.name, files_list_text(files)))
        elif self.replace:
            self.target = find_file(files, self.replace)
            if not self.target:
                raise Refused('--replace %s: the mod has no file with that id or name. Its files:\n%s'
                              % (self.replace, files_list_text(files)))
        else:
            for ref in [self.name] + self.existing_names:
                self.target = find_file(files, ref)
                if self.target:
                    break
        if self.target and self.target["name"].casefold() != self.name.casefold():
            self.notes.append('the file is "%s" on the page; the new version is named "%s"'
                              % (self.target["name"], self.name))

    def as_text(self, checked=True, why="no key"):
        if self.target:
            t = self.target
            text = 'a new version of file %s "%s" (%s -> %s' % (t["id"], t["name"], t["version"] or "?",
                                                                self.version)
            text += "; the old version is archived)" if self.archive_old else "; the old version stays)"
            return text
        if not checked:
            if self.replace:
                return 'a new version of the file "%s" (not checked: %s)' % (self.replace, why)
            return 'a new file "%s", or a new version of a file with that name (not checked: %s)' % (self.name, why)
        return 'a new file "%s"' % self.name

    def plan_lines(self, indent="  "):
        lines = ["%s%s: %s, md5 %s" % (indent, self.shown, size_text(self.size), self.md5),
                 "%sname %s, version %s, category %s, mod manager download %s%s"
                 % (indent, '"%s"' % self.name, self.version, self.category, "on" if self.allow_mm else "off",
                    ", sets the mod's version to %s" % self.version if self.update_mod_version else "")]
        if self.description:
            lines.append('%sdescription: "%s" (%d characters)' % (indent, short(self.description),
                                                                  len(self.description)))
        for note in self.notes:
            lines.append("%snote: %s" % (indent, note))
        return lines


def upload_archive(api, job, wait):
    """Create an upload session, PUT the bytes to its presigned address, finalise it and wait until Nexus has
    processed it. Returns the upload id."""
    with open(job.path, "rb") as f:
        data = f.read()
    try:
        md5_now = hashlib.md5(data, usedforsecurity=False).hexdigest()
    except TypeError:
        md5_now = hashlib.md5(data).hexdigest()
    if md5_now != job.md5 or len(data) != job.size:
        raise NexusError("%s changed since the plan was made; nothing was uploaded. Run the command again."
                         % job.shown)
    what = "creating the upload session for %s" % job.filename
    d = api.v3("POST", "/uploads", {"size_bytes": job.size, "filename": job.filename, "md5": job.md5},
               what=what).data(what)
    upload_id, url = str(d.get("id") or ""), d.get("presigned_url")
    if not UPLOAD_ID_RE.match(upload_id) or not isinstance(url, str) or not url:
        raise NexusError("Nexus's answer to %s had no upload id or upload address" % what)
    host = clean(urllib.parse.urlsplit(url).hostname, 80)
    say("      upload session %s" % upload_id)
    api.put_presigned(url, data, job.filename, content_md5(job.md5), what="taking %s" % job.filename)
    say("      sent %s to %s" % (size_text(job.size), host))
    what = "finalising the upload"
    fin = api.v3("POST", "/uploads/%s/finalise" % q(upload_id), what=what).data(what)
    say("      finalised")
    start = _now()
    state = clean(fin.get("state"), 40)
    if state != "available":
        state = wait_until_available(api, upload_id, wait)
    say("      Nexus has processed it (%s after %g s)" % (state, round(_now() - start, 1)))
    return upload_id


BAD_STATES = ("fail", "reject", "error", "quarantin", "cancel", "expire", "infect")


def wait_until_available(api, upload_id, wait):
    deadline = _now() + wait
    delay = POLL_FIRST
    what = "reading the upload's state"
    while True:
        state = clean(api.v3("GET", "/uploads/%s" % q(upload_id), what=what).data(what).get("state"), 40)
        if state == "available":
            return state
        if any(word in state.lower() for word in BAD_STATES):
            raise NexusError("Nexus says the upload %s is %s; nothing was added to the mod page." % (upload_id,
                                                                                                    state))
        left = deadline - _now()
        if left <= 0:
            raise NexusError("Nexus was still processing the upload %s after %g s (state: %s); nothing was added to "
                             "the mod page. Wait a few minutes and run the command again (or give it more time "
                             "with --wait <seconds>)." % (upload_id, wait, state or "none"))
        _sleep(min(delay, left))
        delay = min(delay * 1.5, POLL_MAX)


def run_job(api, mod_info, job, wait):
    """Upload the archive and attach it: a new version of `job.target`, or a new file. Returns what was made."""
    upload_id = upload_archive(api, job, wait)
    body = {"upload_id": upload_id, "name": job.name, "version": job.version, "file_category": job.category,
            "allow_mod_manager_download": bool(job.allow_mm), "update_mod_version": bool(job.update_mod_version)}
    if job.description:
        body["description"] = job.description
    if job.target:
        t = job.target
        body["archive_existing_file"] = bool(job.archive_old)
        if t.get("version_id"):
            body["previous_version_id"] = t["version_id"]
        what = "adding the version to file %s" % t["id"]
        d = api.v3("POST", "/mod-files/%s/versions" % q(t["id"]), body, what=what).data(what)
        version = d.get("version") if isinstance(d.get("version"), dict) else {}
        made = 'file %s "%s" has version %s (version id %s)%s' % (
            t["id"], job.name, job.version, clean(version.get("id"), 40) or "?",
            ", the old version archived" if job.archive_old else "")
    else:
        body["mod_id"] = mod_info["id"]
        what = "creating the file \"%s\"" % job.name
        d = api.v3("POST", "/mod-files", body, what=what).data(what)
        made = 'new file %s "%s", version %s' % (clean(d.get("id"), 40) or "?", job.name, job.version)
    say("      " + made)
    return made


# ---------------------------------------------------------------------------------------------------------- commands

def ready(yes, blockers):
    """None to go ahead; otherwise the exit code after the dry run or the reasons it cannot go ahead."""
    if blockers:
        warn("")
        warn("Not ready, nothing was sent:")
        for b in blockers:
            warn("  - " + b)
        return 2
    if not yes:
        say("")
        say("Dry run: nothing was sent. Run the same command with --yes to do it.")
        return 0
    return None


def mod_line(game, mod, mod_info):
    if mod_info:
        return '%s/%s: "%s" (mod id %s)' % (game, mod, mod_info["name"], mod_info["id"])
    return "%s/%s (not checked: no key)" % (game, mod)


def cmd_whoami(args):
    api = Api(require_key(args.key_file))
    what = "checking the key"
    d = api.v1("/users/validate.json", what).json(what)
    if not isinstance(d, dict):
        raise NexusError("Nexus's answer to %s was not what it should be" % what)
    premium = bool(d.get("is_premium") or d.get("is_premium?"))
    supporter = bool(d.get("is_supporter") or d.get("is_supporter?"))
    say("The key works: %s (user id %s). Premium: %s. Supporter: %s."
        % (clean(d.get("name"), 80) or "?", clean(d.get("user_id"), 20) or "?", "yes" if premium else "no",
           "yes" if supporter else "no"))
    if api.limits_text():
        say(api.limits_text())
    return 0


def cmd_files(args):
    game, mod = parse_target(args.target)
    api = Api(require_key(args.key_file))
    info = resolve_mod(api, game, mod)
    files = list_files(api, info["id"])
    say(mod_line(game, mod, info) + (", page version %s" % info["version"] if info["version"] else ""))
    if not files:
        say("  (the mod has no files yet)")
        return 0
    rows = [("file id", "name", "latest", "category", "version id", "versions")]
    rows += [(f["id"], f["name"], f["version"] or "-", f["category"] or "-", f["version_id"] or "-",
              str(f["versions"])) for f in files]
    widths = [max(len(r[i]) for r in rows) for i in range(len(rows[0]))]
    for r in rows:
        say("  " + "  ".join(c.ljust(w) for c, w in zip(r, widths)).rstrip())
    return 0


def cmd_upload(args):
    game, mod = parse_target(args.target)
    problems = []
    description = read_text_arg(args.description, "--description")
    description = description.strip() if description else None
    job = FileJob(args.file, args.name, args.version, args.category, description,
                  allow_mod_manager_download=not args.no_mod_manager_download,
                  update_mod_version=args.update_mod_version, archive_old=args.archive_old, replace=args.replace)
    if args.archive_old and not args.replace:
        problems.append("--archive-old needs --replace: only a new version of an existing file archives the old one")
    job.check(problems)
    wait = check_wait(args.wait, problems)
    if problems:
        raise Refused(problems)
    key = load_key(args.key_file)
    blockers, info, api = [], None, None
    if key:
        api = Api(key)
        info = resolve_mod(api, game, mod)
        files = list_files(api, info["id"])
        if args.replace:
            job.match(files)
        else:
            same = next((f for f in files if f["name"].casefold() == job.name.casefold()), None)
            if same:
                job.notes.append('the mod already has a file named "%s" (file %s); this makes a second one. To add a '
                                 "version to it instead: --replace %s" % (same["name"], same["id"], same["id"]))
    else:
        blockers.append(key_help())
    say(mod_line(game, mod, info))
    say("  upload %s as %s" % (job.filename, job.as_text(checked=bool(key))))
    for line in job.plan_lines("    "):
        say(line)
    code = ready(args.yes, blockers)
    if code is not None:
        return code
    say("")
    say("[1/1] upload %s as %s" % (job.filename, job.as_text()))
    made = run_job(api, info, job, wait)
    say("Done: " + made + ".")
    return 0


def cmd_page(args):
    game, mod = parse_target(args.target)
    problems = []
    summary = description = None
    if args.summary is not None:
        summary = check_summary(read_text_arg(args.summary, "--summary"), problems)
    if args.description is not None:
        description = read_text_arg(args.description, "--description").replace("\r\n", "\n").rstrip()
        if not description.strip():
            problems.append("the description is empty")
    if args.summary is None and args.description is None:
        problems.append("nothing to change: give --summary, --description or both")
    if problems:
        raise Refused(problems)
    key = load_key(args.key_file)
    blockers, info, api = [], None, None
    if key:
        api = Api(key)
        info = resolve_mod(api, game, mod)
    else:
        blockers.append(key_help())
    say(mod_line(game, mod, info))
    for line in page_plan(summary, description, args.description):
        say("  " + line)
    code = ready(args.yes, blockers)
    if code is not None:
        return code
    update_page(api, info, summary, description)
    say("Done: the page text is updated.")
    return 0


def page_plan(summary, description, description_source):
    lines = []
    if summary is not None:
        lines.append('summary (%d characters): "%s"' % (len(summary), short(summary, 80)))
    if description is not None:
        src = description_source[1:] if description_source and description_source.startswith("@") else "the text"
        lines.append("description from %s (%s characters of BBCode)" % (src, "{:,}".format(len(description))))
    return lines


def update_page(api, info, summary, description):
    body = {}
    if summary is not None:
        body["summary"] = summary
    if description is not None:
        body["description"] = description
    api.v3("PATCH", "/mods/%s" % q(info["id"]), body, what="updating the page text")


def cmd_changelog(args):
    game, mod = parse_target(args.target)
    problems = []
    check_version(args.version, problems)
    entries = changelog_entries(read_text_arg(args.entries, "--entries").splitlines())
    check_entries(entries, problems)
    if problems:
        raise Refused(problems)
    key = load_key(args.key_file)
    blockers, info, api, todo = [], None, None, entries
    if key:
        api = Api(key)
        info = resolve_mod(api, game, mod)
        todo = new_entries(entries, read_changelog(api, game, mod), args.version)
    else:
        blockers.append(key_help())
    say(mod_line(game, mod, info))
    say("  " + changelog_plan(args.version, entries, todo))
    code = ready(args.yes, blockers)
    if code is not None:
        return code
    if todo:
        add_changelog(api, info, args.version, todo)
    say("Done.")
    return 0


def check_entries(entries, problems):
    if not entries:
        problems.append("no changelog entries: one entry per line")
    elif len("\n".join(entries)) > CHANGELOG_MAX:
        problems.append("the changelog is longer than Nexus allows (%d characters)" % CHANGELOG_MAX)


def changelog_plan(version, entries, todo):
    if not todo:
        return "changelog %s: nothing to add (%s already there)" % (
            version, "the entry is" if len(entries) == 1 else "all %d entries are" % len(entries))
    text = 'changelog %s: %d %s: %s' % (version, len(todo), "entry" if len(todo) == 1 else "entries",
                                         "; ".join('"%s"' % short(e, 50) for e in todo[:3]))
    if len(todo) > 3:
        text += "; ..."
    if len(todo) < len(entries):
        text += " (%d already there)" % (len(entries) - len(todo))
    return text


def add_changelog(api, info, version, entries):
    api.v3("POST", "/mods/%s/changelogs" % q(info["id"]), {"version": version, "changelog": "\n".join(entries)},
           what="adding the changelog for %s" % version)


# ---------------------------------------------------------------------------------------------------------- release

CONFIG_KEYS = {"game", "mod_id", "root", "version", "version_file", "summary", "description_file", "files",
               "changelog"}
FILE_KEYS = {"name", "file", "category", "description", "allow_mod_manager_download", "update_mod_version",
             "archive_old", "file_id", "existing_names"}


class Release:
    pass


def _bool(value, label, problems, default):
    if value is None:
        return default
    if not isinstance(value, bool):
        problems.append("%s must be true or false" % label)
        return default
    return value


def load_release(config_path):
    """Read and check a release config. Raises Refused with every problem found; returns a Release whose
    `blockers` say what is missing before --yes (an unset mod_id)."""
    try:
        with open(config_path, encoding="utf-8-sig") as f:
            cfg = json.load(f)
    except OSError as e:
        raise Refused("cannot read %s: %s" % (config_path, e.strerror or type(e).__name__))
    except ValueError as e:
        raise Refused("%s is not valid JSON: %s" % (config_path, e))
    if not isinstance(cfg, dict):
        raise Refused("%s must hold one JSON object" % config_path)
    problems, blockers = [], []
    unknown = sorted(k for k in cfg if not k.startswith("_") and k not in CONFIG_KEYS)
    if unknown:
        problems.append("unknown keys in the config: %s (notes go in keys that start with _)" % ", ".join(unknown))
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(config_path)), str(cfg.get("root", "."))))

    def at_root(rel):
        return os.path.normpath(os.path.join(root, rel))

    rel = Release()
    rel.config = config_path
    rel.game = cfg.get("game")
    if not isinstance(rel.game, str) or not GAME_RE.match(rel.game):
        problems.append('"game" must be the game\'s name in Nexus addresses, for example "enshrouded"')
    mod_id = cfg.get("mod_id")
    rel.mod = None
    if mod_id is None:
        blockers.append('"mod_id" is not set in %s: set it to the number in the mod page\'s address '
                        "(https://www.nexusmods.com/%s/mods/<number>). Creating the page is a step on the website."
                        % (config_path, rel.game if isinstance(rel.game, str) else "<game>"))
    elif isinstance(mod_id, bool) or not (isinstance(mod_id, int) or (isinstance(mod_id, str) and mod_id.isdigit())):
        problems.append('"mod_id" must be the number in the mod page\'s address, or null')
    else:
        rel.mod = str(mod_id)

    version = cfg.get("version")
    if version is None:
        vf = cfg.get("version_file")
        if not isinstance(vf, str):
            problems.append('give "version" or "version_file"')
        else:
            try:
                with open(at_root(vf), encoding="utf-8-sig") as f:
                    version = f.readline().strip()
            except OSError:
                problems.append("cannot read the version file %s" % vf)
    rel.version = version if isinstance(version, str) else ""
    if rel.version:
        check_version(rel.version, problems)
    elif not problems:
        problems.append("the version is empty")

    rel.summary = None
    if cfg.get("summary") is not None:
        if not isinstance(cfg["summary"], str):
            problems.append('"summary" must be text')
        else:
            rel.summary = check_summary(cfg["summary"], problems)

    rel.description = rel.description_file = None
    if cfg.get("description_file") is not None:
        rel.description_file = str(cfg["description_file"])
        try:
            with open(at_root(rel.description_file), encoding="utf-8-sig") as f:
                rel.description = f.read().replace("\r\n", "\n").rstrip()
            if not rel.description.strip():
                problems.append("the description file %s is empty" % rel.description_file)
        except OSError:
            problems.append("cannot read the description file %s" % rel.description_file)
        except UnicodeDecodeError:
            problems.append("the description file %s is not UTF-8 text" % rel.description_file)

    rel.jobs = []
    files = cfg.get("files")
    if not isinstance(files, list) or not files:
        problems.append('"files" must list at least one file')
        files = []
    names = set()
    for i, entry in enumerate(files, 1):
        label = "files[%d]" % i
        if not isinstance(entry, dict):
            problems.append("%s must be an object" % label)
            continue
        bad = sorted(k for k in entry if not k.startswith("_") and k not in FILE_KEYS)
        if bad:
            problems.append("%s has unknown keys: %s" % (label, ", ".join(bad)))
        name = str(entry.get("name") or "").replace("{version}", rel.version)
        shown = str(entry.get("file") or "").replace("{version}", rel.version)
        if not shown:
            problems.append('%s has no "file"' % label)
            continue
        description = entry.get("description")
        if description is not None:
            description = str(description).replace("{version}", rel.version).strip() or None
        existing = entry.get("existing_names") or []
        if not isinstance(existing, list) or not all(isinstance(n, str) and n.strip() and len(n) <= 255
                                                     and not re.search(r"[\x00-\x1f]", n) for n in existing):
            problems.append("%s.existing_names must be a list of names (the file's names on the page)" % label)
            existing = []
        file_id = entry.get("file_id")
        if file_id is not None and not (isinstance(file_id, (int, str)) and str(file_id).isdigit()
                                        and not isinstance(file_id, bool)):
            problems.append("%s: file_id must be a file's id (a number)" % label)
            file_id = None
        job = FileJob(at_root(shown), name, rel.version, entry.get("category", "main"), description,
                      allow_mod_manager_download=_bool(entry.get("allow_mod_manager_download"),
                                                       label + ".allow_mod_manager_download", problems, True),
                      update_mod_version=_bool(entry.get("update_mod_version"), label + ".update_mod_version",
                                               problems, False),
                      archive_old=_bool(entry.get("archive_old"), label + ".archive_old", problems, False),
                      file_id=str(file_id) if file_id is not None else None, shown=shown,
                      existing_names=existing)
        job.check(problems, label="%s name" % label)
        if name.casefold() in names:
            problems.append('two files are named "%s"' % name)
        names.add(name.casefold())
        rel.jobs.append(job)

    rel.entries = []
    changelog = cfg.get("changelog")
    if changelog is not None:
        if not isinstance(changelog, dict):
            problems.append('"changelog" must map versions to lists of entries')
        else:
            for v in changelog:
                check_version(v, problems, label="changelog version")
            raw = changelog.get(rel.version) if rel.version else None
            if raw is None:
                problems.append('the changelog has no entries for %s: add "%s": ["..."] under "changelog"'
                                % (rel.version or "the version", rel.version))
            else:
                lines = raw if isinstance(raw, list) else [raw]
                rel.entries = changelog_entries(str(x) for x in lines)
                check_entries(rel.entries, problems)
    if problems:
        raise Refused(problems)
    rel.blockers = blockers
    return rel


def cmd_release(args):
    rel = load_release(args.config)
    problems = []
    wait = check_wait(args.wait, problems)
    if problems:
        raise Refused(problems)
    key = load_key(args.key_file)
    blockers = list(rel.blockers)
    info, api, todo = None, None, list(rel.entries)
    if not key:
        blockers.append(key_help())
    elif rel.mod:
        api = Api(key)
        info = resolve_mod(api, rel.game, rel.mod)
        files = list_files(api, info["id"])
        for job in rel.jobs:
            job.match(files)
        taken = {}
        for job in rel.jobs:
            if job.target and job.target["id"] in taken:
                raise Refused('"%s" and "%s" would both continue file %s "%s": give them different names, '
                              '"existing_names" or "file_id"' % (taken[job.target["id"]], job.name,
                                                                job.target["id"], job.target["name"]))
            if job.target:
                taken[job.target["id"]] = job.name
        for job in rel.jobs:
            if job.target and job.target["version"] == rel.version:
                job.skip = ('file %s "%s" is already at %s (to send it anyway, use the upload command)'
                            % (job.target["id"], job.target["name"], rel.version))
        if rel.entries:
            todo = new_entries(rel.entries, read_changelog(api, rel.game, rel.mod), rel.version)

    steps = []   # (kind, text, job)
    for job in rel.jobs:
        if job.skip:
            steps.append(("skip", "skip %s: %s" % (job.filename, job.skip), job))
        else:
            text = job.as_text(checked=bool(info), why="mod_id not set" if key else "no key")
            steps.append(("file", "upload %s as %s" % (job.filename, text), job))
    if rel.summary is not None or rel.description is not None:
        steps.append(("page", "page text: " + " and ".join(page_plan(rel.summary, rel.description,
                                                                     "@" + (rel.description_file or ""))), None))
    if rel.entries:
        steps.append(("changelog", changelog_plan(rel.version, rel.entries, todo), None))

    target = mod_line(rel.game, rel.mod, info) if rel.mod else "%s/<mod_id not set>" % rel.game
    say("Release %s of %s" % (rel.version, target))
    for i, (kind, text, job) in enumerate(steps, 1):
        say("  %d. %s" % (i, text))
        if kind == "file":
            for line in job.plan_lines("       "):
                say(line)
    code = ready(args.yes, blockers)
    if code is not None:
        return code

    say("")
    for i, (kind, text, job) in enumerate(steps, 1):
        say("[%d/%d] %s" % (i, len(steps), text))
        try:
            if kind == "file":
                run_job(api, info, job, wait)
            elif kind == "page":
                update_page(api, info, rel.summary, rel.description)
                say("      updated")
            elif kind == "changelog" and todo:
                add_changelog(api, info, rel.version, todo)
                say("      added")
        except NexusError as e:
            finished = "" if i == 1 else "Step 1 is done. " if i == 2 else "Steps 1 to %d are done. " % (i - 1)
            raise NexusError("%s\nStopped at step %d of %d. %sRunning the same command again skips the files that "
                             "are already at %s." % (e, i, len(steps), finished, rel.version))
    say("Release %s is done." % rel.version)
    if api and api.limits_text():
        say(api.limits_text())
    return 0


# ---------------------------------------------------------------------------------------------------------- main

def build_parser():
    common = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    common.add_argument("--key-file", metavar="PATH",
                        help="read the API key from this file instead of NEXUS_API_KEY")
    changes = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    changes.add_argument("--yes", action="store_true", help="do it (without --yes only the plan is printed)")

    p = argparse.ArgumentParser(prog="nexus_publish.py",
                                description="Publish releases on Nexus Mods with its API v3. Dry run unless --yes.",
                                epilog="Details: the comment at the top of this file.", allow_abbrev=False)
    sub = p.add_subparsers(dest="command", metavar="command")
    sub.required = True

    s = sub.add_parser("whoami", allow_abbrev=False, parents=[common], help="check the key: the account and whether it is Premium")
    s.set_defaults(func=cmd_whoami)

    s = sub.add_parser("files", allow_abbrev=False, parents=[common], help="list a mod's files: id, name, version, category")
    s.add_argument("target", metavar="<game>/<mod>")
    s.set_defaults(func=cmd_files)

    s = sub.add_parser("upload", allow_abbrev=False, parents=[common, changes], help="upload one archive as a new file or version")
    s.add_argument("target", metavar="<game>/<mod>")
    s.add_argument("--file", required=True, help="the .zip, .7z or .rar (at most 100 MiB)")
    s.add_argument("--name", required=True, help="the file's name on the page (at most 50 characters)")
    s.add_argument("--version", required=True, help="the file's version, for example 1.2.0")
    s.add_argument("--category", required=True, choices=CATEGORIES)
    s.add_argument("--description", metavar="TEXT|@FILE", help="the file's description")
    s.add_argument("--replace", metavar="FILE", help="add a version to this existing file (its id or name)")
    s.add_argument("--archive-old", action="store_true", help="with --replace: archive the previous version")
    s.add_argument("--no-mod-manager-download", action="store_true",
                   help="remove the Mod Manager Download button for this file")
    s.add_argument("--update-mod-version", action="store_true", help="set the mod's version to this version")
    s.add_argument("--wait", type=int, metavar="SECONDS",
                   help="how long to wait for Nexus to process the upload (default %d)" % DEFAULT_WAIT)
    s.set_defaults(func=cmd_upload)

    s = sub.add_parser("page", allow_abbrev=False, parents=[common, changes], help="update the page's summary and/or description")
    s.add_argument("target", metavar="<game>/<mod>")
    s.add_argument("--summary", metavar="TEXT|@FILE", help="the brief overview (at most 350 characters)")
    s.add_argument("--description", metavar="TEXT|@FILE", help="the description (BBCode)")
    s.set_defaults(func=cmd_page)

    s = sub.add_parser("changelog", allow_abbrev=False, parents=[common, changes], help="add changelog entries for a version")
    s.add_argument("target", metavar="<game>/<mod>")
    s.add_argument("--version", required=True)
    s.add_argument("--entries", required=True, metavar="@FILE|TEXT", help="one entry per line")
    s.set_defaults(func=cmd_changelog)

    s = sub.add_parser("release", allow_abbrev=False, parents=[common, changes], help="a whole release from a config file")
    s.add_argument("config", metavar="<config.json>")
    s.add_argument("--wait", type=int, metavar="SECONDS",
                   help="how long to wait for Nexus to process each upload (default %d)" % DEFAULT_WAIT)
    s.set_defaults(func=cmd_release)
    return p


KEY_OPTIONS = ("--key", "--apikey", "--api-key")


def main(argv=None):
    del _SECRETS[:]
    argv = sys.argv[1:] if argv is None else list(argv)
    if any(a.split("=", 1)[0].lower() in KEY_OPTIONS for a in argv):
        warn("Refused, nothing was sent. There is no --key option: a key on the command line stays in the shell's "
             "history. Use the environment variable NEXUS_API_KEY or --key-file <path>.")
        return 2
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except Refused as e:
        warn("Refused, nothing was sent:" if len(e.problems) > 1 else "Refused, nothing was sent.")
        for problem in e.problems:
            warn(("  - " if len(e.problems) > 1 else "") + problem)
        return 2
    except NexusError as e:
        warn("error: %s" % e)
        return 1
    except KeyboardInterrupt:
        warn("stopped (Ctrl+C)")
        return 130
    except Exception:
        warn("unexpected error in nexus_publish.py (a bug; the key is hidden below):")
        warn(traceback.format_exc().rstrip())
        return 1


if __name__ == "__main__":
    sys.exit(main())
