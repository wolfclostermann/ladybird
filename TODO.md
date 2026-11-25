# Extensions to-do

Work on the `web-extensions` branch, which lets Ladybird load unpacked Chrome extensions with
`--load-extension=<dir>`, starting with Vimium. Most useful items first.

## Bugs found in interactive testing

- [ ] **Tab order:** report tab positions in the tab strip's order. `tab_views()` in
      `Libraries/LibWebView/ExtensionHost.cpp` walks an unordered `HashMap` of views, so `tab.index` is
      arbitrary and Vimium's `J`/`K` (previous/next tab) appear reversed.
- [ ] **`F` (open link in new tab):** Vimium simulates a click with `metaKey` (macOS) or `ctrlKey` held.
      Make that synthetic click from the extension's isolated world open the link in a new tab.
- [ ] **`b` (bookmarks):** no bookmarks show. Check the path of `Bookmarks.json` that the UI uses, and the
      shape Vimium's bookmark completer expects from `chrome.bookmarks.getTree()`.
- [ ] **`yy` (copy URL):** Vimium reports a copy, but nothing reaches the clipboard. It copies with
      `navigator.clipboard.writeText` inside its HUD frame.
- [ ] **`/` (find):** the find box loses focus after each letter, probably when the page's selection
      changes to highlight a match.
- [ ] **`o` (Vomnibar):** remove the blue outline drawn around the Vomnibar frame, which Chrome doesn't draw.
- [ ] **Debugging:** `console.log` from content scripts doesn't reach the terminal; only thrown errors do.

## Missing APIs Vimium uses

- [ ] `tabs.move`, for `<<`/`>>`. Needs the tab-order fix first.
- [ ] `sessions.restore`, for `X` (reopen a closed tab).
- [ ] `search.query`, for searching the web from `o`.
- [ ] The `chrome-extension://<id>/_favicon/` endpoint, for favicons in the Vomnibar.

## Next phase: UI

- [ ] An `about:extensions` page listing loaded extensions.
- [ ] The toolbar button for extensions (`chrome.action`). The `action.set*` calls are no-ops for now.

## Housekeeping

- [ ] Keep the test extensions and pages (a toy extension, a Vimium test page) in the repo or another permanent
      place.
- [ ] Format the new code with clang-format.
- [ ] Close the security gaps before everyday use:
  - an extension's frame can share a process with the web page around it;
  - the browser process trusts a renderer's claim about which extension is calling an API;
  - the `matches` of an extension's `web_accessible_resources` aren't checked.
