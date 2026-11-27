# Extension test fixtures

Extensions, pages and scripts used to try out WebExtensions support by hand and through WebDriver. None of this
runs in CI: `TestExtensions.cpp` holds the automated tests. The fixtures were made while building each feature and are
kept so that the checks can be repeated.

## Layout

- `extensions/`: small unpacked extensions, each probing one area. Load one with
  `--load-extension=<abs path>`.
  - `toy-ext`: content scripts at every `run_at`, the background service worker (as a module), storage,
    scripting, webNavigation and tabs. Its manifest has comments, which Chromium allows. Goes with `sites/toy-page`.
  - `mark-ext`: "Marker". A content script that sets `data-marker` on pages, plus a style sheet, an options page and a
    toolbar button. Used by the `tools/drive*.py` scripts for about:extensions (off/on, reload, remove, install).
  - `action-ext`, `popup-ext`, `nopopup-ext`: `chrome.action` (badges, per-tab state, `onClicked`, popups,
    `openPopup`).
  - `order-ext`: tab order and `tabs.move` (goes with `sites/pages`).
  - `pin-ext`: `tabs.update({pinned})`, `tabs.onUpdated` and `tabs.query({pinned})`. Pins the active tab 3 seconds
    after start and unpins it again, logging `PIN ...` lines. Needs the Qt UI (headless views can't be pinned).
  - `moved-ext`: `tabs.onMoved`. Opens three tabs, moves them with `tabs.move` (one and several) and by pinning,
    and logs `MOVED ...` lines with each event and the order after each step. Needs the Qt UI. Drag a tab afterwards
    to see the event for a user's move.
  - `sessions-ext`: `chrome.sessions`. Opens and closes two tabs, lists them, restores one by ID and one with no ID,
    logging `SESSIONS ...` lines. Needs the Qt UI.
  - `port-ext`: `runtime.connect`, `tabs.connect` and ports between content scripts on example.com/.org, the
    background page and `page.html`: messages both ways, disconnects from either end, no receiver, navigation and
    closing the tab. Logs `PORT ...` lines.
  - `msg-ext`: `runtime.sendMessage` and `runtime.connect` between the background page, a content script and an
    extension page in a frame of the web page: who receives, whose response wins, ports with several receivers, a
    port closing when its frame navigates, and `tabs.sendMessage` with no listener. Runs headless on any
    `http://127.0.0.1` page and logs `MSG ...` lines.
  - `tab-watch-ext`: for trying tab moves, pins and `chrome.sessions` by hand. Its popup lists recently closed tabs
    with Restore buttons and shows tab events live, streamed from the background page over a port.
  - `bm-ext`, `search-ext`, `fav-ext`: bookmarks, `search.query`, the `_favicon/` endpoint (with `sites/fav-site`).
  - `csp-ext`: content scripts on a page with a strict CSP and Trusted Types (`sites/csp-site`, served by
    `tools/csp-server.py`).
  - `host-ext`, `inject-ext`, `noperm-ext`: permission and host permission enforcement (`sites/perm-page`).
  - `iso-ext`, `war-ext`, `war-other-ext`: process isolation of extension frames and `web_accessible_resources`
    (`sites/iso-site`, `sites/war-site`, `sites/neg-site`).
  - `active-tab-ext`: `activeTab`. Its badge says every second whether it can script the active tab: "no" until its
    button is clicked, "yes" after, and "no" again once the tab goes to another origin.
  - `optional-ext`: optional permissions. Its popup asks for `history` and a host and gives them back;
    `tools/optional_permissions.py` drives the rest through WebDriver.
  - `key-ext`, `log-ext`, `diag-ext`: synthetic key events, content script `console.log`, general diagnostics.
  - `broken-ext` (invalid JSON), `bad-ext` (unsupported manifest version), `nothing-ext` (a manifest and nothing
    else): load failures and the about:extensions layout.
- `sites/`: the pages these extensions run on. Most expect to be served over HTTP from `sites/` as the root, for
  example `python3 -m http.server 8790 --bind 127.0.0.1 --directory sites`. `vimium-page` is the page for trying
  Vimium's keys (`j`/`k`, `f`, `o`, `/`, ...).
- `packages/`: packed versions of `mark-ext` and a minimal `tiny` extension for Install from File…: signed `.crx`
  files, one with a tampered payload, and `.zip` files with and without a top-level folder. The `.pem` files are
  throwaway keys made for these tests; they fix the extension IDs. `tiny.crx` is the one embedded in
  `TestExtensions.cpp`.
- `tools/`:
  - `drive.py`, `drive2.py` (run with `a`, then `b`), `drive3.py`, `drive4.py`, `shot.py`: WebDriver scripts for
    about:extensions. Run each as `python3 tools/drive.py <work dir>`. They copy the fixtures they change into the
    work directory, start a headless WebDriver from `Build/release` (or `$LADYBIRD_BIN_DIR`) and print what they
    see. `shot.py` saves screenshots into the work directory.
  - `optional_permissions.py <work dir>`: checks `chrome.permissions` headlessly and prints PASS/FAIL per check.
  - `fetch-vimium.sh [dir]`: clones Vimium at the commit that was tested (2.4.2).
  - `csp-server.py <root> <port>`: a static server that sends a strict CSP for `/csp-site/`.
  - `click.swift` (`swift tools/click.swift x y [right]`) and `press.applescript` (`osascript tools/press.applescript
    <button name>`): drive the Qt UI with real input on macOS. These take over the mouse and keyboard.

## Known gaps

- WebDriver's Navigate To cannot open an extension's own page (for example its options page): the browser only lets a
  view read an extension's files when it started the load itself, so the load fails with "Permission denied".
  Opening the page from about:extensions or the toolbar button's menu works.
