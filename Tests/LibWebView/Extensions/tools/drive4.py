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
PROFILE = os.path.join(S, "profile4")
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


def view():
    return run(
        "return {rows: [...document.querySelectorAll('.extension')].map(r => r.querySelector('.extension-name').textContent), errors: [...document.querySelectorAll('.load-error')].map(e => e.querySelector('.load-error-directory').textContent.split('/').pop()), empty: document.querySelector('#empty').textContent.trim()}"
    )


def state():
    for root, _dirs, files in os.walk(PROFILE):
        if "extensions.json" in files and root.endswith("Extensions"):
            return open(os.path.join(root, "extensions.json")).read()


# Make the profile's data directory by launching once, then write the state.
shutil.rmtree(PROFILE, ignore_errors=True)


def start(extra=()):
    log = open(os.path.join(S, "webdriver-4.log"), "a")
    return subprocess.Popen(
        [os.path.join(BIN, "WebDriver"), "--headless", f"--port={PORT}", f"--profile-path={PROFILE}"] + list(extra),
        stdout=log,
        stderr=log,
    )


server = subprocess.Popen(
    [sys.executable, "-I", "-m", "http.server", "8791", "--bind", "127.0.0.1", "--directory", os.path.join(S, "site")],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
wd = None
try:
    wd = start()
    time.sleep(2)
    sid = req("POST", "/session", {"capabilities": {}})["sessionId"]
    time.sleep(1)
    req("DELETE", f"/session/{sid}")
    wd.terminate()
    wd.wait()
    time.sleep(1)
    data_dir = None
    for root, _dirs, files in os.walk(PROFILE):
        if os.path.basename(root) == "data" or "History.db" in files:
            data_dir = root
            break
    assert data_dir is not None, "the profile has no data directory"
    print("data dir:", os.path.relpath(data_dir, PROFILE))
    os.makedirs(os.path.join(data_dir, "Extensions"), exist_ok=True)
    json.dump(
        {"disabled": [], "unpacked": [os.path.realpath(os.path.join(S, "mark-ext")), os.path.join(S, "gone-ext")]},
        open(os.path.join(data_dir, "Extensions", "extensions.json"), "w"),
    )

    wd = start()
    time.sleep(2)
    sid = req("POST", "/session", {"capabilities": {}})["sessionId"]
    time.sleep(2)
    go(SITE)
    print(
        "1 installed ext runs without the option:", run("return document.documentElement.getAttribute('data-marker')")
    )
    go("about:extensions")
    print("2", view())
    run("document.querySelector('.extension button:nth-last-of-type(1)').click()")
    time.sleep(0.5)
    print(
        "3 dialog command-line note hidden:", run("return document.querySelector('#remove-dialog-command-line').hidden")
    )
    run("document.querySelector('#remove-dialog button[value=remove]').click()")
    time.sleep(1.5)
    print("4 after remove:", view(), state())
    run("[...document.querySelectorAll('.load-error button')].find(b => b.textContent === 'Remove').click()")
    time.sleep(1.5)
    print("5 after forgetting missing folder:", view(), state())
    req("DELETE", f"/session/{sid}")
    wd.terminate()
    wd.wait()
    time.sleep(1)

    wd = start([f"--load-extension={os.path.join(S, 'mark-ext')}"])
    time.sleep(2)
    sid = req("POST", "/session", {"capabilities": {}})["sessionId"]
    time.sleep(2)
    go("about:extensions")
    print("6 restart with option:", view())
    run("document.querySelector('.extension button:nth-last-of-type(1)').click()")
    time.sleep(0.5)
    print(
        "7 dialog command-line note hidden:", run("return document.querySelector('#remove-dialog-command-line').hidden")
    )
    run("document.querySelector('#remove-dialog button[value=cancel]').click()")
    run("document.querySelector('#load-unpacked').click()")
    time.sleep(1)
    print("8 load unpacked without a UI does nothing:", view())
    req("DELETE", f"/session/{sid}")
finally:
    if wd:
        wd.terminate()
    server.terminate()
