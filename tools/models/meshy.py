"""Meshy API client for the creature pipeline (standard library only). Spends credits: every call says what it costs.

The key comes from the environment variable MESHY_API_KEY (on Windows also read from the user's registry environment,
so a key set with [Environment]::SetEnvironmentVariable works without restarting). It is never printed.

  python meshy.py balance
  python meshy.py preview "<prompt>" [--polycount 12000] [--pose t-pose|a-pose]    (about 20 credits)
  python meshy.py refine <preview task id> ["<texture prompt>"]                    (about 10 credits)
  python meshy.py status <task id>
  python meshy.py download <task id> <folder>          (model .glb, textures, thumbnail)
Every request is appended to meshy_log.txt next to the downloads folder you pass (or the current folder).
"""
import json
import os
import sys
import time
import urllib.request

API = "https://api.meshy.ai/openapi"


def api_key():
    key = os.environ.get("MESHY_API_KEY")
    if not key and sys.platform == "win32":
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment") as k:
            key = winreg.QueryValueEx(k, "MESHY_API_KEY")[0]
    if not key:
        raise SystemExit("MESHY_API_KEY is not set")
    return key


def call(method, path, body=None):
    req = urllib.request.Request(API + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Authorization": "Bearer " + api_key(), "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read().decode() or "{}")
    except urllib.error.HTTPError as e:
        raise SystemExit("Meshy said %d: %s" % (e.code, e.read().decode()[:500]))


def log(line, folder="."):
    with open(os.path.join(folder, "meshy_log.txt"), "a", encoding="utf-8") as f:
        f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + line + "\n")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "balance":
        print(json.dumps(call("GET", "/v1/balance")))
    elif cmd == "preview":
        prompt = args[0]
        poly = int(args[args.index("--polycount") + 1]) if "--polycount" in args else 12000
        body = {"mode": "preview", "prompt": prompt, "ai_model": "latest", "should_remesh": True,
                "target_polycount": poly, "topology": "triangle"}
        if "--pose" in args:
            body["pose_mode"] = args[args.index("--pose") + 1]
        r = call("POST", "/v2/text-to-3d", body)
        log("preview %s polycount %d prompt=%r" % (r.get("result"), poly, prompt))
        print("preview task", r.get("result"))
    elif cmd == "refine":
        body = {"mode": "refine", "preview_task_id": args[0], "enable_pbr": True, "remove_lighting": True}
        if len(args) > 1:
            body["texture_prompt"] = args[1]
        r = call("POST", "/v2/text-to-3d", body)
        log("refine %s of %s" % (r.get("result"), args[0]))
        print("refine task", r.get("result"))
    elif cmd == "status":
        r = call("GET", "/v2/text-to-3d/" + args[0])
        keep = {k: r.get(k) for k in ("id", "mode", "status", "progress", "task_error")}
        print(json.dumps(keep))
    elif cmd == "download":
        task, folder = args[0], args[1]
        os.makedirs(folder, exist_ok=True)
        r = call("GET", "/v2/text-to-3d/" + task)
        if r.get("status") != "SUCCEEDED":
            raise SystemExit("task is %s (%s%%)" % (r.get("status"), r.get("progress")))
        files = []
        for fmt, url in (r.get("model_urls") or {}).items():
            if fmt == "glb" and url:
                files.append(("%s.glb" % task, url))
        for i, tex in enumerate(r.get("texture_urls") or []):
            for kind, url in tex.items():
                if url:
                    files.append(("%s_tex%d_%s.png" % (task, i, kind), url))
        if r.get("thumbnail_url"):
            files.append(("%s_thumb.png" % task, r["thumbnail_url"]))
        for name, url in files:
            urllib.request.urlretrieve(url, os.path.join(folder, name))
            print("saved", os.path.join(folder, name))
        with open(os.path.join(folder, task + ".json"), "w", encoding="utf-8") as f:
            json.dump({k: v for k, v in r.items() if k not in ("model_urls", "texture_urls", "thumbnail_url")}, f, indent=2)
        log("downloaded %s (%d files)" % (task, len(files)), folder)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
