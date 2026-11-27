# Extensions to-do

Work on the `web-extensions` branch, which lets Ladybird load unpacked Chrome extensions with
`--load-extension=<dir>`, starting with Vimium. Most useful items first.

## Bugs found in interactive testing

- [x] **Tab order:** report tab positions in the tab strip's order. `tab_views()` in
      `Libraries/LibWebView/ExtensionHost.cpp` walks an unordered `HashMap` of views, so `tab.index` is
      arbitrary and Vimium's `J`/`K` (previous/next tab) appear reversed.
- [x] **`F` (open link in new tab):** Vimium simulates a click with `metaKey` (macOS) or `ctrlKey` held.
      Make that synthetic click from the extension's isolated world open the link in a new tab.
- [x] **`b` (bookmarks):** not a bug. Vimium's bookmark completer lists nothing until you type a query (as in
      Chrome); typing matches bookmarks, including those in folders.
- [x] **`yy` (copy URL):** an extension's focused page may now write to the clipboard without a gesture, as in
      Chromium. The pages of loaded extensions may read the clipboard too, for Vimium's `p`/`P`.
- [x] **`/` (find):** the find box loses focus after each letter, probably when the page's selection
      changes to highlight a match.
- [x] **`o` (Vomnibar):** remove the blue outline drawn around the Vomnibar frame, which Chrome doesn't draw.
- [x] **Debugging:** `console.log` from content scripts doesn't reach the terminal; only thrown errors do.

- [x] **Strict CSP sites (ai.google):** `f` threw on Trusted Types and Vimium's styles were blocked. Content scripts
      are now exempt from the page's Trusted Types and inline-style rules, as in Chromium.

## Missing APIs Vimium uses

- [x] `tabs.move`, for `<<`/`>>`. Moves within a window only; `tabs.onMoved` isn't fired yet.
- [x] `sessions.restore`, for `X` (reopen a closed tab). Only the most recent; no session IDs or `getRecentlyClosed` yet.
- [x] `search.query`, for searching the web from `o`. Fails when no search engine is chosen (Ladybird's default).
- [x] The `chrome-extension://<id>/_favicon/` endpoint, for favicons in the Vomnibar. No default icon for pages without one.
- [x] `tabs.update({pinned})`, for Vimium's pin toggle. Fires `tabs.onUpdated` with `pinned`; `tabs.query({pinned})` works.
- [x] `tabs.onMoved`, also for moves the user makes (worked out from the tab strip order the UI reports).
- [ ] **Parked:** moving tabs between windows (`tabs.move` with `windowId`, `tabs.onAttached`/`onDetached`); there is one window, id 1.
      Needs real windows first, all checked in the Qt UI: the UI reports each window's tab order and focus (today
      `tab_order_changed` is one flat list), ExtensionHost keeps view -> window and gives `tab.windowId`/`index` per
      window, `windows.get/getAll/getCurrent/getLastFocused` list real windows, `currentWindow`/`lastFocusedWindow`
      queries and `tabs.onActivated` use them, `windows.onCreated/onRemoved/onFocusChanged` fire. Then `tabs.move` with
      `windowId` asks the UI to move the tab (Qt can already drag a tab to another window), and a tab whose window
      changed in a report fires `onDetached` then `onAttached`.
- [x] `sessions.getRecentlyClosed`, `sessions.restore` by session ID, `sessions.onChanged`. A closed window lists only
      the tab that was active in it, and `restore` resolves with the session as it was listed (no new tab IDs).
- [x] `history.onVisited` and `history.onVisitRemoved`. Vimium reads the history once and then follows these, so
      without them pages visited since it started never reached the Vomnibar (`o`) and removed ones stayed there.
- [ ] **Parked:** `windows.create`, for `W` (move tab to a new window); part of the multi-window work above, which
      needs substantial changes to Ladybird's window handling.
- [x] `chrome.notifications` (`create`, `update`, `clear`, `getAll`, `getPermissionLevel`, `onClicked`, `onClosed`), which
      Vimium uses only to announce a major upgrade. Shown through the platform's notifications as the extension's; no
      icons, images or buttons (a list's items and a progress value are shown as text).

## Next phase: UI

- [x] An `about:extensions` page listing loaded extensions (and the ones that failed to load).
- [x] The toolbar button for extensions (`chrome.action`), with badges, per-tab state, `onClicked` and popups.
- [x] Enable, disable, reload and remove extensions from `about:extensions`.
- [x] Install an unpacked folder from the browser (Load Unpacked… on `about:extensions`).
- [x] Install packed `.crx` / `.zip` files from the browser (Install from File… on `about:extensions`). CRX3 only;
      signatures are checked, but there is no store or update channel.
- [x] A context menu on toolbar buttons: Options, Turn Off, Remove from Ladybird… (with a confirmation), Manage
      Extensions. Open about:extensions pages update when an extension changes elsewhere.
- [x] An extensions menu (puzzle-piece button) listing every extension with a toolbar button, with pinning, as in
      Chromium. Buttons are unpinned by default; pins and their order persist. `action.openPopup` and
      `action.getUserSettings().isOnToolbar` work. Pinned buttons can be dragged into another order.
- [x] An unpinned extension's button shows while its popup is open, and the popup hangs from it, as in Chromium.
- [x] Reach `about:extensions` without the puzzle-piece button: an Extensions item in the Ladybird and File menus, and
      an Extensions tab in `about:settings`, whose switch (on by default) keeps the button while no extensions load.
- [x] Keyboard commands: the manifest's `commands` and their suggested keys, `commands.onCommand` (granting `activeTab`),
      `_execute_action`/`_execute_browser_action` (a click on the toolbar button), `commands.getAll`, and a shortcuts view
      at `about:extensions#shortcuts` to change or remove them, kept in `extensions.json` (`"shortcuts"`). Ladybird's own
      reserved shortcuts stay fixed. Shortcuts only work while a tab's page has focus, not in the address bar or a popup.

## Other APIs

- [x] `runtime.connect`, `tabs.connect` and ports.
- [x] `runtime.sendMessage` and `runtime.connect` reach every page of the extension (popup, pages in tabs, its frames
      in web pages), not only the background page. A port connects to every frame that takes it, as in Chromium.

## Housekeeping

- [x] Keep the test extensions and pages (a toy extension, a Vimium test page) in the repo or another permanent
      place. They are in `Tests/LibWebView/Extensions/` (see its README), with `tools/fetch-vimium.sh` for Vimium.
- [x] WebDriver's Navigate To opens an extension's own pages. `Tests/LibWebView/test-webdriver-extensions.py` checks
      content scripts, extension pages and messaging through WebDriver under CTest.
- [x] It also checks reload (and a failed one), remove, and that an extension turned off stays off after a restart.
- [x] Move the rest of the `tools/drive*.py` checks into it: Try Again, installed folders (and forgetting a missing
      one), turning an extension off closing its pages. Options, Load Unpacked… and Install from File… need a UI to open
      a tab or choose a file, so they stay manual checks.
- [x] Format the new code with clang-format (Xcode's 21 on the changed lines), prettier and ruff.
- [x] Close the security gaps before everyday use:
  - [x] an extension's frame could share a process with the web page around it;
  - [x] the browser process trusted a renderer's claim about which extension is calling an API;
  - [x] the `matches` of an extension's `web_accessible_resources` weren't checked;
  - [x] the page's CSP was bypassed for every `chrome-extension:` URL, not only web-accessible resources;
  - [x] permissions and host permissions weren't enforced;
  - [x] no tests showed that extension file paths can't leave the extension's directory (they found an escape from
        the web-accessible files, now fixed);
  - [x] `activeTab` (granted by clicking the toolbar button) and optional permissions (`permissions.request`, with a
        prompt) are supported. about:extensions lists granted optional permissions and can take them back.
- [ ] Do an adversarial security review of all the extensions code added on this fork (IPC entry points, the
      `chrome-extension:` scheme, path handling, permission checks, process isolation, CRX parsing), looking for ways
      a web page or a malicious extension could get past the fixes above.

## Out of scope

- `declarativeNetRequest` / `webRequest`: only ad blockers need them, and ad blocking isn't planned.
