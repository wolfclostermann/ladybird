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
- [ ] Moving tabs between windows (`tabs.move` with `windowId`, `tabs.onAttached`/`onDetached`); there is one window, id 1.
- [x] `sessions.getRecentlyClosed`, `sessions.restore` by session ID, `sessions.onChanged`. A closed window lists only
      the tab that was active in it, and `restore` resolves with the session as it was listed (no new tab IDs).

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
      `action.getUserSettings().isOnToolbar` work. No drag-to-reorder of pinned buttons yet.

## Other APIs

- [x] `runtime.connect`, `tabs.connect` and ports. Like `runtime.sendMessage`, `runtime.connect` reaches only the
      background page.

## Housekeeping

- [x] Keep the test extensions and pages (a toy extension, a Vimium test page) in the repo or another permanent
      place. They are in `Tests/LibWebView/Extensions/` (see its README), with `tools/fetch-vimium.sh` for Vimium.
- [ ] WebDriver's Navigate To can't open an extension's own pages (they fail with "Permission denied"), because the
      browser didn't start the load. Turn the `tools/drive*.py` scripts into `ladybird_python_test`s once it can.
- [ ] Format the new code with clang-format.
- [ ] Close the security gaps before everyday use:
  - [x] an extension's frame could share a process with the web page around it;
  - [x] the browser process trusted a renderer's claim about which extension is calling an API;
  - [x] the `matches` of an extension's `web_accessible_resources` weren't checked;
  - [x] the page's CSP was bypassed for every `chrome-extension:` URL, not only web-accessible resources;
  - [x] permissions and host permissions weren't enforced;
  - [x] no tests showed that extension file paths can't leave the extension's directory (they found an escape from
        the web-accessible files, now fixed);
  - [x] `activeTab` (granted by clicking the toolbar button) and optional permissions (`permissions.request`, with a
        prompt) are supported. about:extensions lists granted optional permissions and can take them back.
