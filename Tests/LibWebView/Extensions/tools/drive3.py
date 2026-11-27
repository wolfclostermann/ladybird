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

BIN = extension_fixtures.BIN
extension_fixtures.prepare(S)
PROFILE = os.path.join(S, "profile")
PORT = 8790
SITE = "http://127.0.0.1:8791/index.html"
EXT = os.path.join(S, "mark-ext")
BROKEN = os.path.join(S, "broken-ext")


def req(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(
        f"http://127.0.0.1:{PORT}{path}", data=data, method=method, headers={"Content-Type": "application/json"}
    )
    try:
        return json.loads(urllib.request.urlopen(r, timeout=60).read())["value"]
    except urllib.error.HTTPError as e:
        raise RuntimeError(e.read().decode()) from e


def run(script):
    return req("POST", f"/session/{sid}/execute/sync", {"script": script, "args": []})


def go(url):
    req("POST", f"/session/{sid}/url", {"url": url})
    time.sleep(1.5)


def marker():
    go(SITE)
    return run("return document.documentElement.getAttribute('data-marker')")


def view():
    return run(
        "return {rows: [...document.querySelectorAll('.extension')].map(r => [r.querySelector('.extension-name').textContent, [...r.querySelectorAll('button')].map(b => b.textContent), r.querySelector('.extension-failure')?.textContent ?? null]), errors: [...document.querySelectorAll('.load-error')].map(e => e.textContent)}"
    )


def click_button(row_selector, text):
    run(f"[...document.querySelectorAll('{row_selector} button')].find(b => b.textContent === '{text}').click()")
    time.sleep(2)


# A second extension whose manifest is broken at first.
shutil.rmtree(BROKEN, ignore_errors=True)
shutil.copytree(EXT, BROKEN)
good_manifest = open(os.path.join(BROKEN, "manifest.json")).read().replace('"Marker"', '"Second"')
open(os.path.join(BROKEN, "manifest.json"), "w").write("{ not json")
cs = os.path.join(EXT, "cs.js")
original_cs = open(cs).read()
manifest = os.path.join(EXT, "manifest.json")
original_manifest = open(manifest).read()

server = subprocess.Popen(
    [sys.executable, "-I", "-m", "http.server", "8791", "--bind", "127.0.0.1", "--directory", os.path.join(S, "site")],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
log = open(os.path.join(S, "webdriver-3.log"), "w")
wd = subprocess.Popen(
    [
        os.path.join(BIN, "WebDriver"),
        "--headless",
        f"--port={PORT}",
        f"--profile-path={PROFILE}",
        f"--load-extension={EXT}",
        f"--load-extension={BROKEN}",
    ],
    stdout=log,
    stderr=log,
)
time.sleep(2)
try:
    sid = req("POST", "/session", {"capabilities": {}})["sessionId"]
    time.sleep(2)
    print("1 marker:", marker())
    open(cs, "w").write(original_cs.replace('"on"', '"v2"'))
    go("about:extensions")
    print("2 view:", view())
    click_button(".extension", "Reload")
    print("3 after reload:", view())
    print("  marker:", marker())

    open(manifest, "w").write("{ broken")
    go("about:extensions")
    click_button(".extension", "Reload")
    print("4 reload with broken manifest:", view())
    print("  marker (old still runs):", marker())
    open(manifest, "w").write(original_manifest)

    go("about:extensions")
    click_button(".load-error", "Try Again")
    print("5 retry while still broken:", view())
    open(os.path.join(BROKEN, "manifest.json"), "w").write(good_manifest)
    click_button(".load-error", "Try Again")
    print("6 retry after fixing:", view())
    req("DELETE", f"/session/{sid}")
finally:
    wd.terminate()
    server.terminate()
    open(cs, "w").write(original_cs)
    open(manifest, "w").write(original_manifest)
