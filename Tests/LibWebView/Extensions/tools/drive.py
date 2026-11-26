import json, os, subprocess, sys, time, urllib.request, urllib.error

S = sys.argv[1]
import extension_fixtures
BIN = extension_fixtures.BIN
extension_fixtures.prepare(S)
PROFILE = os.path.join(S, "profile")
PORT = 8790
SITE = "http://127.0.0.1:8791/index.html"

def req(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", data=data, method=method, headers={"Content-Type": "application/json"})
    try:
        return json.loads(urllib.request.urlopen(r, timeout=60).read())["value"]
    except urllib.error.HTTPError as e:
        raise RuntimeError(e.read().decode())

def run(script, *args):
    return req("POST", f"/session/{sid}/execute/sync", {"script": script, "args": list(args)})

def go(url):
    req("POST", f"/session/{sid}/url", {"url": url})
    time.sleep(1.5)

def page_state():
    go(SITE)
    return run("return [document.documentElement.getAttribute('data-marker'), getComputedStyle(document.documentElement).scrollMarginTop]")

def ext_rows():
    return run("return [...document.querySelectorAll('.extension')].map(r => ({id: r.dataset.id, cls: r.className, checked: r.querySelector('input[switch]').checked, failure: r.querySelector('.extension-failure')?.textContent ?? null}))")

def state_file():
    p = os.path.join(PROFILE, "data", "Extensions", "extensions.json")
    for root, dirs, files in os.walk(PROFILE):
        if "extensions.json" in files:
            return open(os.path.join(root, "extensions.json")).read()
    return None

def data_dirs():
    out = []
    for root, dirs, files in os.walk(PROFILE):
        if os.path.basename(root) == "Extensions":
            out += dirs
    return out

server = subprocess.Popen([sys.executable, "-I", "-m", "http.server", "8791", "--bind", "127.0.0.1", "--directory", os.path.join(S, "site")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
log = open(os.path.join(S, "webdriver.log"), "w")
wd = subprocess.Popen([os.path.join(BIN, "WebDriver"), "--headless", f"--port={PORT}", f"--profile-path={PROFILE}", f"--load-extension={os.path.join(S, 'mark-ext')}"], stdout=log, stderr=log)
time.sleep(2)
try:
    sid = req("POST", "/session", {"capabilities": {}})["sessionId"]
    time.sleep(2)
    print("1 page with extension on:", page_state())

    go("about:extensions")
    rows = ext_rows(); print("2 about:extensions rows:", rows)
    ext_id = rows[0]["id"]
    print("  data dirs before:", data_dirs())

    # Open the options page in a second tab, which turning the extension off should close.
    main = req("GET", f"/session/{sid}/window")
    new = req("POST", f"/session/{sid}/window/new", {"type": "tab"})["handle"]
    req("POST", f"/session/{sid}/window", {"handle": new})
    go(f"chrome-extension://{ext_id}/options.html")
    print("3 options tab title:", req("GET", f"/session/{sid}/title"), "handles:", len(req("GET", f"/session/{sid}/window/handles")))
    req("POST", f"/session/{sid}/window", {"handle": main})

    run("document.querySelector('.extension input[switch]').click()")
    time.sleep(1.5)
    print("4 after turning off:", ext_rows(), "handles:", len(req("GET", f"/session/{sid}/window/handles")))
    print("  state file:", state_file())
    print("5 page with extension off:", page_state())

    go("about:extensions")
    run("document.querySelector('.extension input[switch]').click()")
    time.sleep(2)
    print("6 after turning on:", ext_rows(), "state:", state_file())
    print("7 page with extension on again:", page_state())

    go("about:extensions")
    run("document.querySelector('.extension button.secondary-button:last-of-type').click()")
    time.sleep(0.5)
    print("8 dialog open:", run("return document.querySelector('#remove-dialog').open"), run("return document.querySelector('#remove-dialog .dialog-title').textContent"))
    run("document.querySelector('#remove-dialog button[value=remove]').click()")
    time.sleep(1.5)
    print("9 after removal rows:", ext_rows(), "empty shown:", run("return !document.querySelector('#empty').classList.contains('hidden')"))
    print("  data dirs after:", data_dirs(), "state:", state_file())
    print("10 page after removal:", page_state())
    req("DELETE", f"/session/{sid}")
finally:
    wd.terminate(); server.terminate()
