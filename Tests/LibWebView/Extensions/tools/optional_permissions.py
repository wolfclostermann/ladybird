# Drives chrome.permissions through a headless WebDriver, with extensions/optional-ext. Without a UI, nobody can grant a
# request, so the grants that a user would make are written into the profile before the second run.
# Usage: python3 optional_permissions.py <work dir>
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

import extension_fixtures

S = sys.argv[1]
extension_fixtures.prepare(S)
EXT = os.path.join(S, "optional-ext")
shutil.rmtree(EXT, ignore_errors=True)
shutil.copytree(os.path.join(extension_fixtures.ASSETS, "extensions", "optional-ext"), EXT)
PROFILE = os.path.join(S, "profile")
PORT = 8790
failures = []


def req(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", data=data, method=method, headers={"Content-Type": "application/json"})
    try:
        return json.loads(urllib.request.urlopen(r, timeout=60).read())["value"]
    except urllib.error.HTTPError as e:
        raise RuntimeError(e.read().decode())


def check(label, actual, expected):
    ok = actual == expected
    print(("PASS " if ok else "FAIL ") + label + ": " + json.dumps(actual))
    if not ok:
        print("     expected: " + json.dumps(expected))
        failures.append(label)


class Session:
    def __init__(self):
        log = open(os.path.join(S, "webdriver-optional.log"), "a")
        self.wd = subprocess.Popen([os.path.join(extension_fixtures.BIN, "WebDriver"), "--headless", f"--port={PORT}", f"--profile-path={PROFILE}", f"--load-extension={EXT}"], stdout=log, stderr=log)
        time.sleep(2)
        try:
            self.sid = req("POST", "/session", {"capabilities": {}})["sessionId"]
            # The checks run in the extension's background page, which a headless browser shows in a window of its own.
            seen = []
            for _ in range(20):
                time.sleep(0.5)
                seen = []
                for handle in req("GET", f"/session/{self.sid}/window/handles"):
                    req("POST", f"/session/{self.sid}/window", {"handle": handle})
                    seen.append(req("GET", f"/session/{self.sid}/url"))
                    if seen[-1].endswith("/_generated_background_page.html"):
                        break
                else:
                    continue
                break
            else:
                raise RuntimeError(f"the extension's background page is not there, only: {seen}")
        except Exception:
            self.wd.terminate()
            raise
        self.extension_id = req("GET", f"/session/{self.sid}/url").split("/")[2]

    def run(self, script):
        return req("POST", f"/session/{self.sid}/execute/sync", {"script": script, "args": []})

    def run_async(self, expression):
        script = f"const done = arguments[arguments.length - 1]; Promise.resolve().then(() => {expression}).then(r => done({{ok: r}}), e => done({{error: e.message}}));"
        return req("POST", f"/session/{self.sid}/execute/async", {"script": script, "args": []})

    def click_request(self, wanted):
        self.run(f"self.wanted = {json.dumps(wanted)}")
        button = req("POST", f"/session/{self.sid}/element", {"using": "css selector", "value": "#request"})
        element = list(button.values())[0]
        req("POST", f"/session/{self.sid}/element/{element}/click", {})
        time.sleep(1)
        return self.run("return self.log[self.log.length - 1]")

    def close(self):
        try:
            req("DELETE", f"/session/{self.sid}")
        finally:
            self.wd.terminate()
            self.wd.wait()


def state_file():
    for root, dirs, _ in os.walk(PROFILE):
        if "Extensions" in dirs:
            return os.path.join(root, "Extensions", "extensions.json")
    for root, _, files in os.walk(PROFILE):
        if "History.db" in files:
            os.makedirs(os.path.join(root, "Extensions"), exist_ok=True)
            return os.path.join(root, "Extensions", "extensions.json")
    raise RuntimeError("the profile has no data directory")


shutil.rmtree(PROFILE, ignore_errors=True)
session = Session()
try:
    print("extension", session.extension_id)
    check("optional namespaces are there", session.run("return [typeof chrome.history, typeof chrome.bookmarks]"), ["object", "object"])
    check("history is refused before a grant", "error" in session.run_async("chrome.history.search({text: ''})"), True)
    check("request without a gesture", session.run_async("chrome.permissions.request({permissions: ['history']})"),
          {"error": "This function must be called during a user gesture"})
    check("request for what the manifest does not name", session.click_request({"permissions": ["tabs"]}).get("error", "")[:59],
          "Only permissions specified in the manifest may be requested")
    check("request for a host outside the optional ones", "error" in session.click_request({"origins": ["https://example.com/*"]}), True)
    check("request for what is held already", session.click_request({"permissions": ["storage"]}).get("granted"), True)
    check("request nobody can grant", session.click_request({"permissions": ["history"], "origins": ["https://www.example.org/*"]}).get("granted"), False)
    check("contains after declining", session.run_async("chrome.permissions.contains({permissions: ['history']})"), {"ok": False})
    check("getAll", session.run_async("chrome.permissions.getAll()"), {"ok": {"permissions": ["storage"], "origins": []}})
    check("removing a required permission", session.run_async("chrome.permissions.remove({permissions: ['storage']})"),
          {"error": "You cannot remove required permissions."})
    extension_id = session.extension_id
finally:
    session.close()

# What the user would have granted.
path = state_file()
state = json.load(open(path)) if os.path.exists(path) else {}
state["granted"] = {extension_id: {"permissions": ["history", "notifications"], "origins": ["https://www.example.org/*"]}}
json.dump(state, open(path, "w"))

session = Session()
try:
    check("granted permissions are held after a restart",
          session.run_async("chrome.permissions.contains({permissions: ['history'], origins: ['https://www.example.org/*']})"), {"ok": True})
    check("a grant the manifest no longer names is dropped", session.run_async("chrome.permissions.contains({permissions: ['notifications']})"), {"ok": False})
    check("getAll with grants", session.run_async("chrome.permissions.getAll()"),
          {"ok": {"permissions": ["storage", "history"], "origins": ["https://www.example.org/*"]}})
    check("history works once granted", "ok" in session.run_async("chrome.history.search({text: ''})"), True)
    check("removing a granted permission", session.run_async("chrome.permissions.remove({permissions: ['history']})"), {"ok": True})
    time.sleep(0.5)
    check("onRemoved", session.run("return self.log"), [{"removed": {"permissions": ["history"], "origins": []}}])
    check("contains after removing", session.run_async("chrome.permissions.contains({permissions: ['history']})"), {"ok": False})
    check("history is refused again", "error" in session.run_async("chrome.history.search({text: ''})"), True)
finally:
    session.close()
check("saved grants after removing", json.load(open(state_file())).get("granted", {}).get(extension_id),
      {"permissions": [], "origins": ["https://www.example.org/*"]})

print("FAILED: " + ", ".join(failures) if failures else "ALL PASSED")
sys.exit(1 if failures else 0)
