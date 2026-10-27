"""Tests of tools/nexus/nexus_publish.py, the Nexus release tool, against a fake Nexus: a threaded http.server on
127.0.0.1 that answers the API v3 and v1 endpoints the tool uses and takes the presigned upload with the storage's
checks (Content-Disposition and Content-MD5 must match the upload session, and no key may reach it). No real network:
every request the tool makes is checked to go to 127.0.0.1.
Run: python -m unittest discover -s tests/tools"""

import base64
import contextlib
import hashlib
import io
import json
import os
import re
import shutil
import sys
import tempfile
import threading
import unittest
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tools", "nexus"))
import nexus_publish  # noqa: E402
# the tests never read the key saved on this PC (setx): only the test about it sets its own
nexus_publish.user_env_key = lambda: None

GAME, MOD, MOD_API_ID = "enshrouded", "123", "9001"


def problem(status, title, detail, errors=None):
    doc = {"type": "about:blank", "title": title, "status": status, "detail": detail, "instance": "/x"}
    if errors is not None:
        doc["errors"] = errors
    return status, {"Content-Type": "application/problem+json"}, json.dumps(doc).encode()


def ok(status, doc, headers=None):
    h = {"Content-Type": "application/json"}
    h.update(headers or {})
    return status, h, json.dumps(doc).encode() if doc is not None else b""


def s3_error(code, message):
    body = ("<?xml version=\"1.0\" encoding=\"UTF-8\"?><Error><Code>%s</Code><Message>%s</Message></Error>"
            % (code, message)).encode()
    return 403, {"Content-Type": "application/xml"}, body


class FakeNexus:
    """The endpoints of api.nexusmods.com the tool uses, plus a presigned storage address, with their checks."""

    def __init__(self):
        self.key = "fk" + uuid.uuid4().hex + "Q7"
        self.log = []
        self.lock = threading.Lock()
        self.mods = {(GAME, MOD): {"id": MOD_API_ID, "game_scoped_id": MOD, "name": "Test Mod", "version": "0.2.0",
                                   "description": "old text"}}
        self.files = {}          # file id -> {"id", "name", "versions": [...]}
        self.uploads = {}
        self.page = {}
        self.changelogs = {}     # version -> [entries]
        self.polls_needed = 1
        self.state_after_finalise = None   # e.g. "failed"
        self.never_available = False
        self.fail = {}           # (method, path) -> [replies to give first]
        self.next_id = 500
        fake = self

        class Handler(BaseHTTPRequestHandler):
            def _any(self):
                n = int(self.headers.get("Content-Length") or 0)
                body = self.rfile.read(n) if n else b""
                entry = {"method": self.command, "path": self.path, "body": body,
                         "headers": {k.lower(): v for k, v in self.headers.items()}}
                with fake.lock:
                    fake.log.append(entry)
                    status, headers, payload = fake.route(entry)
                self.send_response(status)
                for k, v in headers.items():
                    self.send_header(k, v)
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

            do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = _any

            def log_message(self, *args):
                pass

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.base = "http://127.0.0.1:%d" % self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        self.thread.start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join(5)

    # ------------------------------------------------------------------ helpers for the tests
    def add_file(self, name, versions, fid=None):
        """versions: [(version, category)], oldest first."""
        fid = str(fid or self.new_id())
        self.files[fid] = {"id": fid, "name": name, "versions": []}
        for i, (v, cat) in enumerate(versions, 1):
            self.files[fid]["versions"].append(self.version_doc(fid, name, v, cat, str(i)))
        return fid

    def new_id(self):
        self.next_id += 1
        return self.next_id

    def version_doc(self, fid, name, version, category, position):
        vid = str(self.new_id())
        return {"id": vid, "file": {"id": fid, "name": name}, "position": position, "game_scoped_id": vid,
                "name": name, "version": version, "category": category, "uploaded_at": "2026-10-27T10:00:00Z",
                "manifest_url": None}

    def requests(self, method=None, prefix=""):
        return [e for e in self.log if (method is None or e["method"] == method) and e["path"].startswith(prefix)]

    def changes(self):
        """Every request that is not a GET."""
        return [e for e in self.log if e["method"] != "GET"]

    def body(self, method, path):
        hits = [e for e in self.log if e["method"] == method and urllib.parse.urlsplit(e["path"]).path == path]
        return json.loads(hits[-1]["body"]) if hits else None

    # ------------------------------------------------------------------ the routes
    def route(self, e):
        parts = urllib.parse.urlsplit(e["path"])
        path, method, h = parts.path, e["method"], e["headers"]
        queued = self.fail.get((method, path))
        if queued:
            return queued.pop(0)
        if path.startswith("/storage/"):
            return self.storage_put(e, path[len("/storage/"):], parts.query)
        if not (path.startswith("/v3/") or path.startswith("/v1/")):
            return problem(404, "Not Found", "no such address")
        if h.get("user-agent", "").startswith("Python-urllib"):
            return 403, {"Content-Type": "text/html"}, b"<!DOCTYPE html><html><title>Just a moment...</title></html>"
        if h.get("apikey") != self.key:
            return problem(401, "Unauthorized", "A valid API key is required")
        if h.get("application-name") != "EMF-publish" or not h.get("application-version"):
            return problem(400, "Bad Request", "Application-Name and Application-Version are required")
        doc = json.loads(e["body"]) if e["body"] else None
        limits = {"X-RL-Daily-Remaining": "19990", "X-RL-Hourly-Remaining": "499",
                  "X-RL-Hourly-Reset": "2026-10-27T12:00:00+00:00"}

        if method == "GET" and path == "/v1/users/validate.json":
            return ok(200, {"user_id": 4242, "key": self.key, "name": "Test Author", "email": "author@example.invalid",
                            "is_premium?": False, "is_supporter?": False, "is_premium": True, "is_supporter": False,
                            "profile_url": "https://example.invalid/p.png"}, limits)
        m = re.match(r"^/v1/games/(\w+)/mods/(\d+)/changelogs\.json$", path)
        if method == "GET" and m:
            return ok(200, self.changelogs or [], limits)
        m = re.match(r"^/v3/games/(\w+)/mods/(\d+)$", path)
        if method == "GET" and m:
            mod = self.mods.get((m.group(1), m.group(2)))
            return ok(200, {"data": mod}, limits) if mod else problem(404, "Not Found", "Mod not found")
        m = re.match(r"^/v3/mods/(\d+)/files$", path)
        if method == "GET" and m:
            files = [{"id": f["id"], "name": f["name"], "is_active": True, "last_file_uploaded_at": None,
                      "versions_count": len(f["versions"]), "archived_count": 0, "removed_count": 0}
                     for f in self.files.values()]
            return ok(200, {"data": {"mod_files": files}}, limits)
        m = re.match(r"^/v3/mod-files/(\d+)/versions$", path)
        if method == "GET" and m:
            f = self.files.get(m.group(1))
            if not f:
                return problem(404, "Not Found", "The mod file was not found.")
            return ok(200, {"data": {"versions": list(reversed(f["versions"]))}}, limits)   # any order
        if method == "POST" and path == "/v3/uploads":
            return self.create_upload(doc)
        m = re.match(r"^/v3/uploads/([0-9a-f-]+)/finalise$", path)
        if method == "POST" and m:
            u = self.uploads.get(m.group(1))
            if not u:
                return problem(404, "Not Found", "Upload not found")
            if u["data"] is None:
                return problem(422, "Unprocessable Entity", "No data was uploaded")
            u["finalised"] = True
            return ok(200, {"data": {"id": u["id"], "user": {"id": "4242"}, "state": "created"}})
        m = re.match(r"^/v3/uploads/([0-9a-f-]+)$", path)
        if method == "GET" and m:
            u = self.uploads.get(m.group(1))
            if not u:
                return problem(404, "Not Found", "Upload not found")
            u["polls"] += 1
            state = "created"
            if u["finalised"] and self.state_after_finalise:
                state = self.state_after_finalise
            elif u["finalised"] and not self.never_available and u["polls"] >= self.polls_needed:
                state = "available"
            u["state"] = state
            return ok(200, {"data": {"id": u["id"], "user": {"id": "4242"}, "state": state}}, limits)
        if method == "POST" and path == "/v3/mod-files":
            return self.create_file(doc)
        m = re.match(r"^/v3/mod-files/(\d+)/versions$", path)
        if method == "POST" and m:
            return self.create_version(m.group(1), doc)
        if method == "PATCH" and path == "/v3/mods/" + MOD_API_ID:
            if "summary" in doc and len(doc["summary"]) > 350:
                return problem(422, "Validation failed", "summary is too long")
            self.page.update(doc)
            return 204, {}, b""
        if method == "POST" and path == "/v3/mods/%s/changelogs" % MOD_API_ID:
            self.changelogs.setdefault(doc["version"], []).extend(doc["changelog"].split("\n"))
            return ok(201, {"data": doc})
        return problem(404, "Not Found", "no route for %s %s" % (method, path))

    def create_upload(self, doc):
        if not isinstance(doc.get("size_bytes"), int) or doc["size_bytes"] <= 0 or not doc.get("filename"):
            return problem(422, "Unprocessable Entity", "size_bytes and filename are required")
        if not re.match(r"^[0-9a-fA-F]{32}$", doc.get("md5") or ""):
            return problem(422, "Unprocessable Entity", "md5 must be a hex digest")
        uid = str(uuid.uuid4())
        self.uploads[uid] = {"id": uid, "filename": doc["filename"], "size": doc["size_bytes"],
                             "md5": doc["md5"].lower(), "data": None, "finalised": False, "polls": 0,
                             "state": "created", "put_headers": None}
        url = "%s/storage/%s?X-Amz-Signature=fake%s" % (self.base, uid, uid[:8])
        return ok(201, {"data": {"id": uid, "user": {"id": "4242"}, "state": "created", "presigned_url": url}})

    def storage_put(self, e, uid, query):
        u = self.uploads.get(uid)
        if e["method"] != "PUT" or not u or not query.startswith("X-Amz-Signature="):
            return s3_error("AccessDenied", "Request has expired or is not signed")
        h = e["headers"]
        u["put_headers"] = h
        if "apikey" in h:
            return s3_error("InvalidRequest", "An API key must never reach the storage")
        if h.get("content-disposition") != 'attachment; filename="%s"' % u["filename"]:
            return s3_error("SignatureDoesNotMatch", "Content-Disposition does not match the signature")
        if h.get("content-md5") != base64.b64encode(bytes.fromhex(u["md5"])).decode():
            return s3_error("SignatureDoesNotMatch", "Content-MD5 does not match the signature")
        if hashlib.md5(e["body"]).hexdigest() != u["md5"] or len(e["body"]) != u["size"]:
            return s3_error("BadDigest", "The Content-MD5 you specified did not match what we received.")
        u["data"] = e["body"]
        return 200, {"ETag": '"%s"' % u["md5"]}, b""

    def available_upload(self, doc):
        u = self.uploads.get(doc.get("upload_id") or "")
        if not u or u["state"] != "available":
            return None, problem(422, "Unprocessable Entity", "The upload is not available")
        return u, None

    def check_file_fields(self, doc):
        errors = []
        if not re.match(r"^[a-zA-Z0-9 _'().-]+$", doc.get("name") or "") or len(doc["name"]) > 50:
            errors.append({"pointer": "/name", "detail": "The name is invalid."})
        if not re.match(r"^[a-zA-Z0-9.-]+$", doc.get("version") or ""):
            errors.append({"pointer": "/version", "detail": "The version is invalid."})
        if doc.get("file_category") not in ("main", "optional", "miscellaneous"):
            errors.append({"pointer": "/file_category", "detail": "The category is invalid."})
        return problem(422, "Validation failed", "The request is invalid.", errors) if errors else None

    def create_file(self, doc):
        u, err = self.available_upload(doc)
        if err:
            return err
        if doc.get("mod_id") != MOD_API_ID:
            return problem(404, "Not Found", "Mod not found")
        err = self.check_file_fields(doc)
        if err:
            return err
        fid = str(self.new_id())
        self.files[fid] = {"id": fid, "name": doc["name"], "versions": [
            self.version_doc(fid, doc["name"], doc["version"], doc["file_category"], "1")]}
        if doc.get("update_mod_version"):
            self.mods[(GAME, MOD)]["version"] = doc["version"]
        u["state"] = "claimed"
        return ok(201, {"data": {"id": fid, "game_scoped_id": fid, "name": doc["name"],
                                 "file_category": doc["file_category"]}})

    def create_version(self, fid, doc):
        f = self.files.get(fid)
        if not f:
            return problem(404, "Not Found", "The mod file was not found.")
        u, err = self.available_upload(doc)
        if err:
            return err
        err = self.check_file_fields(doc)
        if err:
            return err
        latest = max(f["versions"], key=lambda v: float(v["position"]))
        if doc.get("previous_version_id") not in (None, latest["id"]):
            return problem(422, "Unprocessable Entity", "previous_version_id is not the latest version")
        if doc.get("archive_existing_file"):
            for v in f["versions"]:
                v["category"] = "archived"
        new = self.version_doc(fid, doc["name"], doc["version"], doc["file_category"],
                               str(int(float(latest["position"])) + 1))
        f["versions"].append(new)
        if doc.get("update_mod_version"):
            self.mods[(GAME, MOD)]["version"] = doc["version"]
        u["state"] = "claimed"
        return ok(201, {"data": {"file": {"id": fid, "game_scoped_id": fid, "name": f["name"],
                                          "file_category": doc["file_category"]},
                                 "version": {"id": new["id"], "position": new["position"]}}})


class FakeClock:
    def __init__(self):
        self.t = 1000.0
        self.sleeps = []

    def now(self):
        return self.t

    def sleep(self, s):
        self.sleeps.append(s)
        self.t += s


class Base(unittest.TestCase):
    def setUp(self):
        self.fake = FakeNexus()
        self.addCleanup(self.fake.close)
        self.clock = FakeClock()
        real_open = nexus_publish._open

        def guarded_open(req, timeout):
            host = urllib.parse.urlsplit(req.full_url).hostname
            if host != "127.0.0.1":
                raise AssertionError("a test tried to reach %s" % host)
            return real_open(req, timeout)

        for name, value in (("API_BASE", self.fake.base + "/v3"), ("V1_BASE", self.fake.base + "/v1"),
                            ("_ALLOW_LOOPBACK_HTTP", True), ("_sleep", self.clock.sleep),
                            ("_now", self.clock.now), ("_open", guarded_open)):
            p = mock.patch.object(nexus_publish, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.tmp = tempfile.mkdtemp(prefix="emf_nexus_publish_")
        self.addCleanup(shutil.rmtree, self.tmp)

    def make_zip(self, name="EMF-0.3.0.zip", data=None):
        path = os.path.join(self.tmp, name)
        with open(path, "wb") as f:
            f.write(data if data is not None else b"PK\x03\x04" + os.urandom(3000))
        return path

    def write(self, name, text):
        path = os.path.join(self.tmp, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return path

    def run_tool(self, *argv, key=True):
        env = {k: v for k, v in os.environ.items() if k != nexus_publish.KEY_ENV}
        if key:
            env[nexus_publish.KEY_ENV] = self.fake.key
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.dict(os.environ, env, clear=True), contextlib.redirect_stdout(out), \
                contextlib.redirect_stderr(err):
            code = nexus_publish.main([str(a) for a in argv])
        self.assertNotIn(self.fake.key, out.getvalue() + err.getvalue())
        return code, out.getvalue(), err.getvalue()

    def upload_args(self, path, *extra, name="EMF - manual install"):
        return ["upload", "%s/%s" % (GAME, MOD), "--file", path, "--name", name, "--version", "0.3.0",
                "--category", "main"] + list(extra)


class Uploads(Base):
    def test_new_file_end_to_end(self):
        path = self.make_zip()
        with open(path, "rb") as f:
            data = f.read()
        code, out, err = self.run_tool(*self.upload_args(path, "--description", "The same without EMF.exe.",
                                                         "--no-mod-manager-download", "--yes"))
        self.assertEqual(code, 0, err)
        order = [(e["method"], urllib.parse.urlsplit(e["path"]).path) for e in self.fake.log]
        upload_id = next(iter(self.fake.uploads))
        self.assertEqual(order, [
            ("GET", "/v3/games/enshrouded/mods/123"), ("GET", "/v3/mods/9001/files"),
            ("POST", "/v3/uploads"), ("PUT", "/storage/" + upload_id),
            ("POST", "/v3/uploads/%s/finalise" % upload_id), ("GET", "/v3/uploads/" + upload_id),
            ("POST", "/v3/mod-files")])
        self.assertEqual(self.fake.body("POST", "/v3/uploads"),
                         {"size_bytes": len(data), "filename": "EMF-0.3.0.zip",
                          "md5": hashlib.md5(data).hexdigest()})
        self.assertEqual(self.fake.body("POST", "/v3/mod-files"), {
            "upload_id": upload_id, "mod_id": "9001", "name": "EMF - manual install", "version": "0.3.0",
            "file_category": "main", "allow_mod_manager_download": False, "update_mod_version": False,
            "description": "The same without EMF.exe."})
        self.assertEqual(self.fake.uploads[upload_id]["data"], data)
        [f] = self.fake.files.values()
        self.assertEqual((f["name"], f["versions"][0]["version"]), ("EMF - manual install", "0.3.0"))
        self.assertIn('new file %s "EMF - manual install", version 0.3.0' % f["id"], out)

    def test_new_version_of_an_existing_file_archives_the_old_one(self):
        fid = self.fake.add_file("EMF and mod manager", [("0.1.0", "archived"), ("0.2.0", "main")])
        old_latest = self.fake.files[fid]["versions"][-1]["id"]
        path = self.make_zip()
        code, out, err = self.run_tool(*self.upload_args(path, "--replace", "emf AND mod manager", "--archive-old",
                                                         "--update-mod-version", "--yes",
                                                         name="EMF and mod manager"))
        self.assertEqual(code, 0, err)
        body = self.fake.body("POST", "/v3/mod-files/%s/versions" % fid)
        self.assertEqual(body["previous_version_id"], old_latest)
        self.assertIs(body["archive_existing_file"], True)
        self.assertIs(body["update_mod_version"], True)
        self.assertIs(body["allow_mod_manager_download"], True)
        self.assertNotIn("mod_id", body)
        self.assertIsNone(self.fake.body("POST", "/v3/mod-files"))
        versions = self.fake.files[fid]["versions"]
        self.assertEqual([v["category"] for v in versions], ["archived", "archived", "main"])
        self.assertEqual(versions[-1]["version"], "0.3.0")
        self.assertEqual(self.fake.mods[(GAME, MOD)]["version"], "0.3.0")
        self.assertIn("0.2.0 -> 0.3.0; the old version is archived", out)

    def test_replace_by_id_and_unknown_file_refused(self):
        fid = self.fake.add_file("Main", [("1.0", "main")])
        code, out, err = self.run_tool(*self.upload_args(self.make_zip(), "--replace", fid, name="Main"))
        self.assertEqual(code, 0, err)
        self.assertIn('a new version of file %s "Main" (1.0 -> 0.3.0; the old version stays)' % fid, out)
        code, out, err = self.run_tool(*self.upload_args(self.make_zip(), "--replace", "Nope", "--yes"))
        self.assertEqual(code, 2)
        self.assertIn("--replace Nope: the mod has no file with that id or name", err)
        self.assertIn('%s "Main" (1.0, main)' % fid, err)
        self.assertEqual(self.fake.changes(), [])

    def test_md5_and_content_md5(self):
        self.assertEqual(nexus_publish.content_md5(hashlib.md5(b"hello").hexdigest()), "XUFAKrxLKna5cZ2REBfFkg==")
        path = self.make_zip(data=b"hello")
        self.assertEqual(nexus_publish.file_digest(path), ("5d41402abc4b2a76b9719d911017c592", 5))
        code, out, err = self.run_tool(*self.upload_args(path, "--yes"))
        self.assertEqual(code, 0, err)
        self.assertIn("md5 5d41402abc4b2a76b9719d911017c592", out)
        [u] = self.fake.uploads.values()
        self.assertEqual(u["md5"], "5d41402abc4b2a76b9719d911017c592")
        self.assertEqual(u["put_headers"]["content-md5"], "XUFAKrxLKna5cZ2REBfFkg==")
        self.assertEqual(u["put_headers"]["content-disposition"], 'attachment; filename="EMF-0.3.0.zip"')
        self.assertNotIn("apikey", u["put_headers"])

    def test_the_storage_refuses_headers_that_do_not_match(self):
        api = nexus_publish.Api(self.fake.key)
        doc = api.v3("POST", "/uploads", {"size_bytes": 5, "filename": "a.zip",
                                          "md5": hashlib.md5(b"hello").hexdigest()}, what="x").data("x")
        good = nexus_publish.content_md5(hashlib.md5(b"hello").hexdigest())
        with self.assertRaises(nexus_publish.NexusError) as e:
            api.put_presigned(doc["presigned_url"], b"hello", "b.zip", good, what="taking a.zip")
        self.assertIn("SignatureDoesNotMatch: Content-Disposition", str(e.exception))
        self.assertIn("127.0.0.1 refused taking a.zip (HTTP 403)", str(e.exception))
        with self.assertRaises(nexus_publish.NexusError) as e:
            api.put_presigned(doc["presigned_url"], b"hello", "a.zip", base64.b64encode(b"0" * 16).decode(),
                              what="taking a.zip")
        self.assertIn("Content-MD5 does not match", str(e.exception))
        api.put_presigned(doc["presigned_url"], b"hello", "a.zip", good, what="taking a.zip")

    def test_a_file_changed_after_the_plan_is_not_uploaded(self):
        path = self.make_zip(data=b"one")
        job = nexus_publish.FileJob(path, "Main", "1.0", "main")
        job.check([])
        with open(path, "wb") as f:
            f.write(b"two")
        with self.assertRaises(nexus_publish.NexusError) as e:
            nexus_publish.upload_archive(nexus_publish.Api(self.fake.key), job, 10)
        self.assertIn("changed since the plan", str(e.exception))
        self.assertEqual(self.fake.log, [])

    def test_a_second_file_with_the_same_name_is_pointed_out(self):
        fid = self.fake.add_file("EMF - manual install", [("0.2.0", "main")])
        code, out, err = self.run_tool(*self.upload_args(self.make_zip()))
        self.assertEqual(code, 0, err)
        self.assertIn('this makes a second one. To add a version to it instead: --replace %s' % fid, out)


class Polling(Base):
    def test_polls_until_available(self):
        self.fake.polls_needed = 3
        code, out, err = self.run_tool(*self.upload_args(self.make_zip(), "--yes"))
        self.assertEqual(code, 0, err)
        self.assertEqual(self.clock.sleeps, [2.0, 3.0])
        [u] = self.fake.uploads.values()
        self.assertEqual(u["polls"], 3)
        self.assertIn("Nexus has processed it (available after 5 s)", out)

    def test_gives_up_after_the_wait_and_adds_nothing(self):
        self.fake.never_available = True
        code, out, err = self.run_tool(*self.upload_args(self.make_zip(), "--yes", "--wait", "10"))
        self.assertEqual(code, 1)
        self.assertEqual(self.clock.sleeps, [2.0, 3.0, 4.5, 0.5])
        self.assertIn("still processing the upload", err)
        self.assertIn("after 10 s", err)
        self.assertEqual(self.fake.requests("POST", "/v3/mod-files"), [])

    def test_a_failed_state_stops_at_once(self):
        self.fake.state_after_finalise = "failed"
        code, out, err = self.run_tool(*self.upload_args(self.make_zip(), "--yes"))
        self.assertEqual(code, 1)
        self.assertIn("is failed; nothing was added to the mod page", err)
        self.assertEqual(self.clock.sleeps, [])


class DryRun(Base):
    def test_dry_runs_send_no_change_requests(self):
        self.fake.add_file("EMF and mod manager", [("0.2.0", "main")])
        summary = self.write("summary.txt", "Short.")
        entries = self.write("entries.txt", "Fixed a thing\n")
        for argv in (self.upload_args(self.make_zip(), "--replace", "EMF and mod manager", "--archive-old"),
                     self.upload_args(self.make_zip()),
                     ["page", "enshrouded/123", "--summary", "@" + summary, "--description", "[b]x[/b]"],
                     ["changelog", "enshrouded/123", "--version", "0.3.0", "--entries", "@" + entries]):
            code, out, err = self.run_tool(*argv)
            self.assertEqual(code, 0, (argv, err))
            self.assertIn("Dry run: nothing was sent. Run the same command with --yes to do it.", out)
        self.assertEqual(self.fake.changes(), [])
        self.assertTrue(self.fake.log)

    def test_the_plan_names_everything(self):
        fid = self.fake.add_file("EMF and mod manager", [("0.2.0", "main")])
        path = self.make_zip(data=b"x" * 2048)
        code, out, err = self.run_tool(*self.upload_args(path, "--replace", fid, "--archive-old",
                                                         "--no-mod-manager-download", "--update-mod-version",
                                                         "--description", "All of it.", name="EMF and mod manager"))
        self.assertEqual(code, 0, err)
        for text in ('enshrouded/123: "Test Mod" (mod id 9001)', "2,048 bytes (0.0 MiB)",
                     "md5 " + hashlib.md5(b"x" * 2048).hexdigest(), 'name "EMF and mod manager"', "version 0.3.0",
                     "category main", "mod manager download off", "sets the mod's version to 0.3.0",
                     'a new version of file %s "EMF and mod manager" (0.2.0 -> 0.3.0; the old version is archived)'
                     % fid, 'description: "All of it."'):
            self.assertIn(text, out)

    def test_without_a_key_the_plan_is_local_and_says_how_to_set_one(self):
        code, out, err = self.run_tool(*self.upload_args(self.make_zip()), key=False)
        self.assertEqual(code, 2)
        self.assertIn("(not checked: no key)", out)
        self.assertIn("md5 ", out)
        self.assertIn('setx NEXUS_API_KEY "<key>"', err)
        self.assertIn("--key-file", err)
        self.assertEqual(self.fake.log, [])
        code, out, err = self.run_tool(*self.upload_args(self.make_zip(), "--yes"), key=False)
        self.assertEqual(code, 2)
        self.assertEqual(self.fake.log, [])


class Errors(Base):
    def test_problem_details_are_shown_plainly(self):
        self.fake.fail[("POST", "/v3/mod-files")] = [problem(
            422, "Validation failed", "The file name is already used on this mod.",
            [{"pointer": "/name", "detail": "Pick another name."}])]
        code, out, err = self.run_tool(*self.upload_args(self.make_zip(), "--yes"))
        self.assertEqual(code, 1)
        self.assertIn('error: Nexus refused creating the file "EMF - manual install" (HTTP 422): Validation failed: '
                      "The file name is already used on this mod.\n  - name: Pick another name.", err)
        self.assertNotIn("Traceback", err)
        self.assertNotIn("{", err)

    def test_a_web_page_instead_of_the_api_is_explained(self):
        self.fake.fail[("GET", "/v3/games/enshrouded/mods/123")] = [
            (403, {"Content-Type": "text/html"}, b"<!DOCTYPE html><html>Attention Required! | Cloudflare</html>")]
        code, out, err = self.run_tool("files", "enshrouded/123")
        self.assertEqual(code, 1)
        self.assertIn("the answer was a web page, not the API's. Cloudflare", err)

    def test_the_user_agent_is_not_pythons(self):
        # the fake refuses Python-urllib's default User-Agent the way Cloudflare does in front of Nexus
        code, out, err = self.run_tool("whoami")
        self.assertEqual(code, 0, err)
        ua = self.fake.log[0]["headers"]["user-agent"]
        self.assertTrue(ua.startswith("EMF-publish/"), ua)
        self.assertEqual(self.fake.log[0]["headers"]["application-name"], "EMF-publish")
        self.assertEqual(self.fake.log[0]["headers"]["application-version"], nexus_publish.tool_version())

    def test_redirects_are_not_followed(self):
        self.fake.fail[("GET", "/v3/games/enshrouded/mods/123")] = [
            (302, {"Location": self.fake.base + "/v3/elsewhere"}, b"")]
        code, out, err = self.run_tool("files", "enshrouded/123")
        self.assertEqual(code, 1)
        self.assertIn("redirect (HTTP 302)", err)
        self.assertEqual(self.fake.requests(prefix="/v3/elsewhere"), [])

    def test_a_mod_that_does_not_exist(self):
        code, out, err = self.run_tool("files", "enshrouded/999")
        self.assertEqual(code, 1)
        self.assertIn("Nexus refused reading the mod enshrouded/999 (HTTP 404): Not Found: Mod not found", err)
        self.assertIn("the mod's number is the one in its page's address", err)


class TheKey(Base):
    def test_the_key_never_appears_in_output_addresses_or_storage(self):
        self.fake.add_file("EMF and mod manager", [("0.2.0", "main")])
        outputs = []
        for argv in (["whoami"], ["files", "enshrouded/123"],
                     self.upload_args(self.make_zip(), "--replace", "EMF and mod manager", "--yes",
                                      name="EMF and mod manager")):
            code, out, err = self.run_tool(*argv)
            self.assertEqual(code, 0, err)
            outputs.append(out + err)
        # an error whose text repeats the key (a server that echoes what it got) has it hidden
        self.fake.fail[("GET", "/v3/games/enshrouded/mods/123")] = [
            problem(401, "Unauthorized", "The key %s is not valid." % self.fake.key)]
        code, out, err = self.run_tool("files", "enshrouded/123")
        self.assertEqual(code, 1)
        self.assertIn("The key <the key> is not valid.", err)
        self.assertIn("nexus_publish.py whoami", err)
        outputs.append(out + err)
        everything = "\n".join(outputs)
        self.assertNotIn(self.fake.key, everything)
        self.assertNotIn("author@example.invalid", everything)
        self.assertIn("The key works: Test Author (user id 4242). Premium: yes. Supporter: no.", outputs[0])
        self.assertIn("Requests left: 19990 today, 499 this hour", outputs[0])
        for e in self.fake.log:
            self.assertNotIn(self.fake.key, e["path"])
            self.assertNotIn(self.fake.key.encode(), e["body"])
            if e["path"].startswith("/storage/"):
                self.assertNotIn("apikey", e["headers"])
            else:
                self.assertEqual(e["headers"].get("apikey"), self.fake.key)

    def test_a_bug_still_hides_the_key(self):
        def boom(*a, **k):
            raise RuntimeError("something broke near " + self.fake.key)
        with mock.patch.object(nexus_publish, "resolve_mod", boom):
            code, out, err = self.run_tool("files", "enshrouded/123")
        self.assertEqual(code, 1)
        self.assertIn("unexpected error", err)
        self.assertIn("something broke near <the key>", err)

    def test_no_key_says_how_to_set_one(self):
        code, out, err = self.run_tool("whoami", key=False)
        self.assertEqual(code, 2)
        self.assertIn('setx NEXUS_API_KEY "<key>"', err)
        self.assertIn("open a new terminal", err)
        self.assertIn("--key-file <path>", err)
        self.assertEqual(self.fake.log, [])

    def test_a_key_saved_by_setx_after_the_terminal_started(self):
        # the terminal's own environment has no key, the saved Windows settings do (setx): the tool uses those
        saved = "savedKey1234567890"
        old_env = os.environ.pop(nexus_publish.KEY_ENV, None)
        old_fn = nexus_publish.user_env_key
        nexus_publish.user_env_key = lambda: saved
        try:
            self.assertEqual(nexus_publish.load_key(None), saved)
            nexus_publish.user_env_key = lambda: None
            self.assertIsNone(nexus_publish.load_key(None))
        finally:
            nexus_publish.user_env_key = old_fn
            if old_env is not None:
                os.environ[nexus_publish.KEY_ENV] = old_env

    def test_key_file(self):
        path = self.write("nexus.key", self.fake.key + "\n")
        code, out, err = self.run_tool("whoami", "--key-file", path, key=False)
        self.assertEqual(code, 0, err)
        empty = self.write("empty.key", "\n")
        code, out, err = self.run_tool("whoami", "--key-file", empty, key=False)
        self.assertEqual(code, 2)
        self.assertIn("is empty", err)
        two = self.write("two.key", self.fake.key + " extra\n")
        code, out, err = self.run_tool("whoami", "--key-file", two, key=False)
        self.assertEqual(code, 2)
        self.assertIn("does not look like a Nexus API key", err)
        code, out, err = self.run_tool("whoami", "--key-file", os.path.join(self.tmp, "missing.key"), key=False)
        self.assertEqual(code, 2)
        self.assertIn("cannot read the key file", err)
        self.assertEqual(len(self.fake.log), 1)

    def test_a_wrong_key(self):
        with mock.patch.dict(os.environ, {nexus_publish.KEY_ENV: "not-the-right-key"}):
            out, err = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = nexus_publish.main(["whoami"])
        self.assertEqual(code, 1)
        self.assertIn("(HTTP 401): Unauthorized: A valid API key is required", err.getvalue())
        self.assertNotIn("not-the-right-key", out.getvalue() + err.getvalue())

    def test_there_is_no_key_option_and_no_address_override(self):
        for argv in (["whoami", "--key", "typed-key-1234"], ["whoami", "--api-key=typed-key-1234"]):
            code, out, err = self.run_tool(*argv)
            self.assertEqual(code, 2)
            self.assertIn("There is no --key option", err)
            self.assertNotIn("typed-key-1234", out + err)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            nexus_publish.main(["whoami", "--key-fil", "x"])   # no abbreviations
        self.assertEqual(self.fake.log, [])
        with open(nexus_publish.__file__, encoding="utf-8") as f:
            self.assertNotIn("NEXUS_API_BASE", f.read())


class Validation(Base):
    def refused(self, *argv):
        code, out, err = self.run_tool(*argv)
        self.assertEqual(code, 2, out + err)
        self.assertIn("Refused, nothing was sent", err)
        self.assertEqual(self.fake.log, [])
        return err

    def test_names_and_versions(self):
        z = self.make_zip()
        err = self.refused(*self.upload_args(z, name="EMF + mod manager"))
        self.assertIn('name "EMF + mod manager" has characters Nexus does not allow in file names: +', err)
        self.assertIn("is 51 characters long; Nexus allows 50", self.refused(*self.upload_args(z, name="x" * 51)))
        err = self.refused("upload", "enshrouded/123", "--file", z, "--name", "Main", "--version", "0.3.0 beta",
                           "--category", "main")
        self.assertIn('version "0.3.0 beta" may only have letters, digits, dots and hyphens', err)

    def test_files(self):
        self.assertIn("file not found", self.refused(*self.upload_args(os.path.join(self.tmp, "none.zip"))))
        exe = os.path.join(self.tmp, "EMF.exe")
        with open(exe, "wb") as f:
            f.write(b"MZ")
        self.assertIn("is not a .zip, .7z or .rar archive", self.refused(*self.upload_args(exe)))
        self.assertIn("is empty", self.refused(*self.upload_args(self.make_zip("e.7z", b""))))
        self.assertEqual(nexus_publish.MAX_SINGLE_PART, 100 * 1024 * 1024)
        with mock.patch.object(nexus_publish, "MAX_SINGLE_PART", 1000):
            err = self.refused(*self.upload_args(self.make_zip("big.rar", b"x" * 1001)))
        self.assertIn("single uploads stop at 100 MiB", err)
        self.assertIn("multipart", err)

    def test_summary_and_other_refusals(self):
        long_summary = self.write("long.txt", "word " * 80)
        err = self.refused("page", "enshrouded/123", "--summary", "@" + long_summary)
        self.assertIn("the summary is 399 characters long; Nexus allows 350", err)
        self.assertIn("nothing to change", self.refused("page", "enshrouded/123"))
        self.assertIn("--archive-old needs --replace", self.refused(*self.upload_args(self.make_zip(),
                                                                                      "--archive-old")))
        args = self.upload_args(self.make_zip())
        args[1] = "enshrouded"
        self.assertIn("is not a mod", self.refused(*args))
        self.assertIn("no changelog entries", self.refused("changelog", "enshrouded/123", "--version", "1.0",
                                                           "--entries", "\n \n"))

    def test_targets(self):
        for text in ("enshrouded/123", "enshrouded/mods/123", "https://www.nexusmods.com/enshrouded/mods/123",
                     "https://www.nexusmods.com/enshrouded/mods/123?tab=files"):
            self.assertEqual(nexus_publish.parse_target(text), ("enshrouded", "123"))
        for text in ("enshrouded", "123", "enshrouded/abc", "https://evil.example/enshrouded/mods/1", "a b/1"):
            with self.assertRaises(nexus_publish.Refused):
                nexus_publish.parse_target(text)


class PageAndChangelog(Base):
    def test_page(self):
        summary = self.write("summary.txt", "One line.\nAnd   another.\n")
        desc = self.write("desc.txt", "[b]Hello[/b]\n\nText.\n")
        code, out, err = self.run_tool("page", "enshrouded/123", "--summary", "@" + summary, "--description",
                                       "@" + desc, "--yes")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.fake.body("PATCH", "/v3/mods/9001"),
                         {"summary": "One line. And another.", "description": "[b]Hello[/b]\n\nText."})

    def test_changelog_entries_and_no_duplicates(self):
        entries = self.write("entries.txt", "- Fixed the clock\n\n* Faster saves\nNew: a tab\n")
        code, out, err = self.run_tool("changelog", "enshrouded/123", "--version", "0.3.1", "--entries",
                                       "@" + entries, "--yes")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.fake.body("POST", "/v3/mods/9001/changelogs"),
                         {"version": "0.3.1", "changelog": "Fixed the clock\nFaster saves\nNew: a tab"})
        # adding is additive on Nexus, so a second run leaves out what is there already
        more = self.write("more.txt", "Fixed the clock\nAnother fix\n")
        code, out, err = self.run_tool("changelog", "enshrouded/123", "--version", "0.3.1", "--entries",
                                       "@" + more, "--yes")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.fake.body("POST", "/v3/mods/9001/changelogs")["changelog"], "Another fix")
        code, out, err = self.run_tool("changelog", "enshrouded/123", "--version", "0.3.1", "--entries",
                                       "@" + more, "--yes")
        self.assertIn("nothing to add", out)
        self.assertEqual(len(self.fake.requests("POST", "/v3/mods/9001/changelogs")), 2)


class RateLimits(Base):
    def test_waits_as_retry_after_says(self):
        path = "/v3/games/enshrouded/mods/123"
        self.fake.fail[("GET", path)] = [(429, {"Retry-After": "3"}, b"")] * 2
        code, out, err = self.run_tool("files", "enshrouded/123")
        self.assertEqual(code, 0, err)
        self.assertEqual(self.clock.sleeps, [3.0, 3.0])
        self.assertIn("too many requests (429)", err)
        self.assertIn("waiting 3 s, then try 2 of 5", err)
        self.assertEqual(len(self.fake.requests("GET", path)), 3)

    def test_backs_off_without_retry_after_also_for_posts(self):
        self.fake.fail[("POST", "/v3/uploads")] = [problem(429, "Too Many Requests", "Slow down")] * 2
        code, out, err = self.run_tool(*self.upload_args(self.make_zip(), "--yes"))
        self.assertEqual(code, 0, err)
        self.assertEqual(self.clock.sleeps[:2], [5.0, 10.0])
        self.assertEqual(len(self.fake.uploads), 1)

    def test_gives_up_with_the_limits(self):
        self.fake.fail[("GET", "/v1/users/validate.json")] = [
            (429, {"X-RL-Hourly-Remaining": "0", "X-RL-Daily-Remaining": "0",
                   "X-RL-Hourly-Reset": "2026-10-27T12:00:00+00:00"}, b"")] * 5
        code, out, err = self.run_tool("whoami")
        self.assertEqual(code, 1)
        self.assertEqual(self.clock.sleeps, [5.0, 10.0, 20.0, 40.0])
        self.assertIn("still says too many requests (429) after 5 tries", err)
        self.assertIn("Requests left: 0 today, 0 this hour (the hourly allowance resets at 2026-10-27T12:00:00", err)

    def test_a_long_retry_after_ends_the_command(self):
        self.fake.fail[("GET", "/v1/users/validate.json")] = [(429, {"Retry-After": "3600"}, b"")]
        code, out, err = self.run_tool("whoami")
        self.assertEqual(code, 1)
        self.assertIn("says to wait 60 minutes", err)
        self.assertEqual(self.clock.sleeps, [])

    def test_server_errors_are_retried_only_for_reads(self):
        self.fake.fail[("GET", "/v3/mods/9001/files")] = [problem(503, "Service Unavailable", "busy")]
        code, out, err = self.run_tool("files", "enshrouded/123")
        self.assertEqual(code, 0, err)
        self.fake.fail[("POST", "/v3/uploads")] = [problem(503, "Service Unavailable", "busy")]
        code, out, err = self.run_tool(*self.upload_args(self.make_zip(), "--yes"))
        self.assertEqual(code, 1)
        self.assertIn("(HTTP 503): Service Unavailable: busy", err)


class Release(Base):
    CONFIG = os.path.join(REPO, "docs", "release", "nexus-release.json")
    RELEASE_FILES = ("nexus-release.json", "nexus-description.txt", "nexus-page.md")

    def need_emf_release(self):
        """EMF's own release files (docs/release) are not in the public source; the tests that read them skip there.
        The tests with a config of their own (small_tree) still run."""
        if not all(os.path.exists(os.path.join(REPO, "docs", "release", n)) for n in self.RELEASE_FILES):
            self.skipTest("docs/release is not in this copy of the repository")

    def release_tree(self, version="0.3.0", mod_id=MOD, changelog=None):
        """A copy of EMF's release config in a folder of its own, with packages made up for the test."""
        self.need_emf_release()
        with open(self.CONFIG, encoding="utf-8") as f:
            cfg = json.load(f)
        cfg["mod_id"] = mod_id
        if changelog:
            cfg["changelog"].update(changelog)
        root = os.path.join(self.tmp, "repo")
        self.write(os.path.join("repo", "docs", "release", "nexus-release.json"), json.dumps(cfg))
        self.write(os.path.join("repo", "VERSION"), version + "\n")
        shutil.copyfile(os.path.join(REPO, "docs", "release", "nexus-description.txt"),
                        os.path.join(root, "docs", "release", "nexus-description.txt"))
        os.makedirs(os.path.join(root, "build", "package"), exist_ok=True)
        for name in ("EMF-%s.zip" % version, "EMF-%s-manual.zip" % version):
            with open(os.path.join(root, "build", "package", name), "wb") as f:
                f.write(b"PK\x03\x04" + name.encode() + os.urandom(500))
        return os.path.join(root, "docs", "release", "nexus-release.json")

    LIVE = (("8247768", "EMF 0.3.0"), ("8247780", "EMF 0.3.0 Manual"))

    def live_page(self):
        """EMF's page on Nexus before its first API release: two main files uploaded by hand, each at version "1", and the mod's
        version "1"."""
        for fid, name in self.LIVE:
            self.fake.add_file(name, [("1", "main")], fid=fid)
        self.fake.mods[(GAME, MOD)]["version"] = "1"

    def small_tree(self, version, files, changelog):
        """A small config of its own (another mod's), with its packages."""
        cfg = {"game": GAME, "mod_id": int(MOD), "root": ".", "version_file": "VERSION", "files": files,
               "changelog": changelog}
        self.write(os.path.join("small", "VERSION"), version + "\n")
        for f in files:
            name = f["file"].replace("{version}", version)
            with open(self.write(os.path.join("small", name), ""), "wb") as out:
                out.write(b"PK\x03\x04" + name.encode() + os.urandom(300))
        return self.write(os.path.join("small", "release.json"), json.dumps(cfg))

    def test_emfs_config(self):
        rel = self.load_real_config()
        self.assertEqual(rel.game, "enshrouded")
        with open(self.CONFIG, encoding="utf-8") as f:
            cfg = json.load(f)
        self.assertEqual(cfg["mod_id"], 246)   # https://www.nexusmods.com/enshrouded/mods/246
        self.assertIn("number in the mod page's URL", cfg["_mod_id"])
        # the two files on the page, by id; each release names its versions after the version
        self.assertEqual([(j.file_id, j.name, j.category, j.allow_mm, j.update_mod_version, j.archive_old,
                           os.path.basename(j.path)) for j in rel.jobs], [
            ("8247768", "EMF %s" % rel.version, "main", False, True, True, "EMF-%s.zip" % rel.version),
            ("8247780", "EMF %s Manual" % rel.version, "main", False, False, True, "EMF-%s-manual.zip" % rel.version)])
        self.assertTrue(all(j.description for j in rel.jobs))
        self.assertEqual([f["file"] for f in cfg["files"]],
                         ["build/package/EMF-{version}.zip", "build/package/EMF-{version}-manual.zip"])
        with open(os.path.join(REPO, "VERSION"), encoding="utf-8") as f:
            self.assertEqual(rel.version, f.read().strip())
        # the summary is the page's brief overview, and the description file is the page's description
        with open(os.path.join(REPO, "docs", "release", "nexus-page.md"), encoding="utf-8") as f:
            page = f.read()
        overview = re.search(r"^\| Brief overview \(max 350 characters\) \| (.+?) \|$", page, re.M).group(1)
        self.assertEqual(rel.summary, overview)
        self.assertLessEqual(len(rel.summary), 350)
        self.assertEqual(rel.description.strip(), page.split("\n---\n", 1)[1].strip())
        self.assertEqual(list(cfg["changelog"].items())[0], ("0.3.0", ["Initial release"]))
        self.assertTrue(cfg["changelog"]["0.3.1"])

    def load_real_config(self):
        # the real config with stand-ins for the packages (they are build output)
        self.need_emf_release()
        made = []
        for suffix in ("", "-manual"):
            with open(os.path.join(REPO, "VERSION"), encoding="utf-8") as f:
                path = os.path.join(REPO, "build", "package", "EMF-%s%s.zip" % (f.read().strip(), suffix))
            if not os.path.exists(path):
                made.append(path)
        real_check = nexus_publish.check_archive

        def check(path, problems, shown=None):
            return None if path in made else real_check(path, problems, shown)

        with mock.patch.object(nexus_publish, "check_archive", check):
            rel = nexus_publish.load_release(self.CONFIG)
        self.assertEqual(rel.blockers, [])
        return rel

    def test_emfs_config_is_not_ready_until_mod_id_is_set(self):
        config = self.release_tree(mod_id=None)
        code, out, err = self.run_tool("release", config, "--yes")
        self.assertEqual(code, 2)
        self.assertIn('"mod_id" is not set', err)
        self.assertIn("https://www.nexusmods.com/enshrouded/mods/<number>", err)
        self.assertIn("Release 0.3.0 of enshrouded/<mod_id not set>", out)
        self.assertIn("(not checked: mod_id not set)", out)
        self.assertEqual(self.fake.log, [])

    def test_a_release_adds_versions_to_the_live_files_and_a_rerun_skips_them(self):
        self.need_emf_release()
        self.live_page()
        with open(self.CONFIG, encoding="utf-8") as f:
            entries = json.load(f)["changelog"]["0.3.1"]
        config = self.release_tree("0.3.1")
        code, out, err = self.run_tool("release", config)
        self.assertEqual(code, 0, err)
        self.assertEqual(self.fake.changes(), [])
        for text in ('1. upload EMF-0.3.1.zip as a new version of file 8247768 "EMF 0.3.0" (1 -> 0.3.1; the old version '
                     "is archived)",
                     'note: the file is "EMF 0.3.0" on the page; the new version is named "EMF 0.3.1"',
                     '2. upload EMF-0.3.1-manual.zip as a new version of file 8247780 "EMF 0.3.0 Manual" (1 -> 0.3.1; '
                     "the old version is archived)",
                     "3. page text: summary (304 characters)", "description from docs/release/nexus-description.txt",
                     "4. changelog 0.3.1: %d entries" % len(entries), "Dry run: nothing was sent."):
            self.assertIn(text, out)

        code, out, err = self.run_tool("release", config, "--yes")
        self.assertEqual(code, 0, err)
        self.assertIsNone(self.fake.body("POST", "/v3/mod-files"))   # no new file
        self.assertEqual(sorted(self.fake.files), ["8247768", "8247780"])
        for (fid, _), name, sets_version in zip(self.LIVE, ("EMF 0.3.1", "EMF 0.3.1 Manual"), (True, False)):
            versions = self.fake.files[fid]["versions"]
            self.assertEqual([(v["version"], v["category"]) for v in versions], [("1", "archived"), ("0.3.1", "main")])
            self.assertEqual(versions[-1]["name"], name)
            body = self.fake.body("POST", "/v3/mod-files/%s/versions" % fid)
            self.assertEqual((body["version"], body["archive_existing_file"], body["previous_version_id"],
                              body["allow_mod_manager_download"], body["update_mod_version"]),
                             ("0.3.1", True, versions[0]["id"], False, sets_version))
        self.assertEqual(self.fake.mods[(GAME, MOD)]["version"], "0.3.1")   # was "1"
        self.assertTrue(self.fake.page["description"].startswith("[size=5][b]EMF - Enshrouded Modding Framework"))
        self.assertEqual(self.fake.changelogs, {"0.3.1": entries})
        self.assertIn("Release 0.3.1 is done.", out)

        # running it again sends no file twice and no changelog entry twice
        uploads = len(self.fake.uploads)
        code, out, err = self.run_tool("release", config, "--yes")
        self.assertEqual(code, 0, err)
        self.assertEqual(len(self.fake.uploads), uploads)
        self.assertIn('skip EMF-0.3.1.zip: file 8247768 "EMF 0.3.0" is already at 0.3.1', out)
        self.assertIn('skip EMF-0.3.1-manual.zip: file 8247780 "EMF 0.3.0 Manual" is already at 0.3.1', out)
        self.assertIn("changelog 0.3.1: nothing to add", out)
        self.assertEqual(self.fake.changelogs, {"0.3.1": entries})

    def test_the_skip_compares_version_text(self):
        # "already at" is the version text: the page's "1" is not "0.3.0", so a 0.3.0 release would upload again
        self.live_page()
        code, out, err = self.run_tool("release", self.release_tree("0.3.0"))
        self.assertEqual(code, 0, err)
        self.assertIn('as a new version of file 8247768 "EMF 0.3.0" (1 -> 0.3.0;', out)
        self.assertNotIn("skip", out)

    def test_a_small_config_makes_its_file_then_adds_versions_by_name(self):
        files = [{"name": "My Mod", "file": "dist/my_mod-{version}.zip", "update_mod_version": True,
                  "archive_old": True}]
        config = self.small_tree("1.0.0", files, {"1.0.0": ["Initial release"]})
        code, out, err = self.run_tool("release", config, "--yes")
        self.assertEqual(code, 0, err)
        self.assertIn('upload my_mod-1.0.0.zip as a new file "My Mod"', out)
        [fid] = self.fake.files
        body = self.fake.body("POST", "/v3/mod-files")
        self.assertEqual((body["name"], body["allow_mod_manager_download"], body["mod_id"]), ("My Mod", True, "9001"))
        config = self.small_tree("1.1.0", files, {"1.0.0": ["Initial release"], "1.1.0": ["Faster"]})
        code, out, err = self.run_tool("release", config, "--yes")
        self.assertEqual(code, 0, err)
        self.assertIn('as a new version of file %s "My Mod" (1.0.0 -> 1.1.0; the old version is archived)' % fid, out)
        self.assertEqual([(v["version"], v["category"]) for v in self.fake.files[fid]["versions"]],
                         [("1.0.0", "archived"), ("1.1.0", "main")])
        code, out, err = self.run_tool("release", config, "--yes")
        self.assertIn('skip my_mod-1.1.0.zip: file %s "My Mod" is already at 1.1.0' % fid, out)
        self.assertEqual(self.fake.changelogs, {"1.0.0": ["Initial release"], "1.1.0": ["Faster"]})

    def test_existing_names_find_a_file_named_on_the_website(self):
        # a name the API refuses ("+"), given on the website: found by existing_names, the version named validly
        fid = self.fake.add_file("My Mod + extras", [("1.0", "main")])
        files = [{"name": "My Mod and extras", "existing_names": ["My Mod + extras"], "file": "my_mod-{version}.zip",
                  "archive_old": True}]
        config = self.small_tree("1.1", files, {"1.1": ["More extras"]})
        code, out, err = self.run_tool("release", config, "--yes")
        self.assertEqual(code, 0, err)
        self.assertIn('as a new version of file %s "My Mod + extras" (1.0 -> 1.1; the old version is archived)' % fid,
                      out)
        self.assertIn('note: the file is "My Mod + extras" on the page; the new version is named "My Mod and extras"',
                      out)
        self.assertIsNone(self.fake.body("POST", "/v3/mod-files"))
        self.assertEqual([(v["name"], v["version"], v["category"]) for v in self.fake.files[fid]["versions"]],
                         [("My Mod + extras", "1.0", "archived"), ("My Mod and extras", "1.1", "main")])

    def test_two_entries_cannot_continue_the_same_file(self):
        self.live_page()
        config = self.release_tree("0.3.1")
        with open(config, encoding="utf-8") as f:
            cfg = json.load(f)
        cfg["files"][1]["file_id"] = "8247768"
        with open(config, "w", encoding="utf-8") as f:
            json.dump(cfg, f)
        code, out, err = self.run_tool("release", config)
        self.assertEqual(code, 2)
        self.assertIn('"EMF 0.3.1" and "EMF 0.3.1 Manual" would both continue file 8247768 "EMF 0.3.0"', err)
        self.assertEqual(self.fake.changes(), [])

    def test_a_failure_says_where_it_stopped_and_a_rerun_finishes(self):
        self.live_page()
        config = self.release_tree("0.3.1")
        self.fake.fail[("POST", "/v3/mod-files/8247780/versions")] = [
            problem(422, "Validation failed", "Too many files.")]
        code, out, err = self.run_tool("release", config, "--yes")
        self.assertEqual(code, 1)
        self.assertIn("Validation failed: Too many files.", err)
        self.assertIn("Stopped at step 2 of 4. Step 1 is done.", err)
        self.assertIn("Running the same command again skips the files that are already at 0.3.1", err)
        self.assertEqual(self.fake.requests("PATCH"), [])
        code, out, err = self.run_tool("release", config, "--yes")
        self.assertEqual(code, 0, err)
        self.assertIn('skip EMF-0.3.1.zip: file 8247768 "EMF 0.3.0" is already at 0.3.1', out)
        self.assertEqual([v["version"] for v in self.fake.files["8247780"]["versions"]], ["1", "0.3.1"])
        self.assertEqual(len(self.fake.requests("PATCH")), 1)

    def test_config_problems_are_refused(self):
        config = self.release_tree(changelog=None)
        with open(config, encoding="utf-8") as f:
            cfg = json.load(f)
        cfg["files"][0]["name"] = "EMF + mod manager"
        cfg["files"][1]["descripton"] = "typo"
        cfg["files"][1]["existing_names"] = "EMF, manual"
        cfg["summary"] = "x" * 351
        cfg["changelog"] = {"0.2.0": ["Old"]}
        with open(config, "w", encoding="utf-8") as f:
            json.dump(cfg, f)
        code, out, err = self.run_tool("release", config)
        self.assertEqual(code, 2)
        for text in ("files[1] name \"EMF + mod manager\" has characters Nexus does not allow",
                     "files[2] has unknown keys: descripton", "the summary is 351 characters long",
                     "files[2].existing_names must be a list of names",
                     'the changelog has no entries for 0.3.0: add "0.3.0": ["..."]'):
            self.assertIn(text, err)
        self.assertEqual(self.fake.log, [])


if __name__ == "__main__":
    unittest.main()
