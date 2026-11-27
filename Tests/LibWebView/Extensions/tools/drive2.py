import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

import extension_fixtures

S = sys.argv[1]
phase = sys.argv[2]

BIN = extension_fixtures.BIN
extension_fixtures.prepare(S, fresh_profile=(phase == "a"))
PROFILE = os.path.join(S, "profile")
PORT = 8790
SITE = "http://127.0.0.1:8791/index.html"


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


def rows():
    return run(
        "return [...document.querySelectorAll('.extension')].map(r => [r.className, r.querySelector('input[switch]').checked, [...r.querySelectorAll('button')].map(b => b.textContent)])"
    )


def click(css):
    el = req("POST", f"/session/{sid}/element", {"using": "css selector", "value": css})
    req("POST", f"/session/{sid}/element/{list(el.values())[0]}/click", {})


def titles():
    out = []
    cur = req("GET", f"/session/{sid}/window")
    for h in req("GET", f"/session/{sid}/window/handles"):
        req("POST", f"/session/{sid}/window", {"handle": h})
        out.append((req("GET", f"/session/{sid}/url"), req("GET", f"/session/{sid}/title")))
    req("POST", f"/session/{sid}/window", {"handle": cur})
    return out


server = subprocess.Popen(
    [sys.executable, "-I", "-m", "http.server", "8791", "--bind", "127.0.0.1", "--directory", os.path.join(S, "site")],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
log = open(os.path.join(S, f"webdriver-{phase}.log"), "w")
wd = subprocess.Popen(
    [
        os.path.join(BIN, "WebDriver"),
        "--headless",
        f"--port={PORT}",
        f"--profile-path={PROFILE}",
        f"--load-extension={os.path.join(S, 'mark-ext')}",
    ],
    stdout=log,
    stderr=log,
)
time.sleep(2)
try:
    sid = req("POST", "/session", {"capabilities": {}})["sessionId"]
    time.sleep(2)
    go("about:extensions")
    print(phase, "rows at start:", rows())
    if phase == "a":
        click(".extension input[switch]")
        time.sleep(1.5)
        print("a after off:", rows())
    else:
        go(SITE)
        print("b page while off:", run("return document.documentElement.getAttribute('data-marker')"))
        go("about:extensions")
        click(".extension input[switch]")
        time.sleep(2)
        print("b after on:", rows())
        click(".extension button.secondary-button")
        time.sleep(2)
        print("b windows with options open:", titles())
        click(".extension input[switch]")
        time.sleep(2)
        print("b windows after off:", titles())
    req("DELETE", f"/session/{sid}")
finally:
    wd.terminate()
    server.terminate()
