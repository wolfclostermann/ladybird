import base64, json, os, subprocess, sys, time, urllib.request
S = sys.argv[1]
import extension_fixtures
BIN = extension_fixtures.BIN
extension_fixtures.prepare(S)
PORT = 8790
def req(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", data=data, method=method, headers={"Content-Type": "application/json"})
    return json.loads(urllib.request.urlopen(r, timeout=60).read())["value"]
os.makedirs(os.path.join(S, "nothing-ext"), exist_ok=True)
open(os.path.join(S, "nothing-ext", "manifest.json"), "w").write('{"manifest_version": 3, "name": "Quiet Helper", "version": "2.3.1", "description": "Does nothing at all, quietly."}')
open(os.path.join(S, "broken-ext", "manifest.json"), "w").write("{ broken")
log = open(os.path.join(S, "webdriver-shot.log"), "w")
args = [os.path.join(BIN, "WebDriver"), "--headless", f"--port={PORT}", f"--profile-path={os.path.join(S, 'profile')}"]
for e in ["mark-ext", "nothing-ext", "broken-ext"]: args.append(f"--load-extension={os.path.join(S, e)}")
wd = subprocess.Popen(args, stdout=log, stderr=log); time.sleep(2)
try:
    sid = req("POST", "/session", {"capabilities": {}})["sessionId"]; time.sleep(2)
    req("POST", f"/session/{sid}/window/rect", {"width": 900, "height": 700})
    req("POST", f"/session/{sid}/url", {"url": "about:extensions"}); time.sleep(1.5)
    req("POST", f"/session/{sid}/execute/sync", {"script": "[...document.querySelectorAll('.extension')].find(r => r.textContent.includes('Quiet')).querySelector('input[switch]').click()", "args": []}); time.sleep(1.5)
    open(os.path.join(S, "list.png"), "wb").write(base64.b64decode(req("GET", f"/session/{sid}/screenshot")))
    req("POST", f"/session/{sid}/execute/sync", {"script": "document.querySelector('.extension button:nth-of-type(3)').click()", "args": []}); time.sleep(1)
    open(os.path.join(S, "dialog.png"), "wb").write(base64.b64decode(req("GET", f"/session/{sid}/screenshot")))
    req("DELETE", f"/session/{sid}")
finally:
    wd.terminate()
