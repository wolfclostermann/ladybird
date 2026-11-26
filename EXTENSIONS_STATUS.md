# WebExtensions in this Ladybird fork: status, remaining work and security review

Branch: `web-extensions`, merged into this fork's `master` on 2026-11-25 (22 commits, about 4,250 lines added
across 67 files). Upstream Ladybird is the `upstream` remote; none of this has been offered upstream.

Goal: load unpacked Chrome extensions with `--load-extension=<dir>`, starting with Vimium, by implementing the real
`chrome.*` APIs rather than a custom clone.

---

## 1. What has been changed

### Commits so far

| Commit | What it does |
|---|---|
| `39bce1ff4bc` | Runs extensions' content scripts in isolated JS worlds (their own globals, shared DOM). |
| `33b5ac657c1` | Loads unpacked extensions from `--load-extension`, reads `manifest.json`, works out extension IDs. |
| `dc442f2dd5d` | Adds the `chrome-extension://` scheme, with its own origin per extension, and hosts background pages in hidden views. |
| `5a5a8306c0d` | Messaging between content scripts and the background page (`runtime.sendMessage`, `tabs.sendMessage`), and `storage.local` / `storage.sync` saved to disk. |
| `a5ddee06707` | Wider API surface: `tabs.*`, `scripting.*`, `webNavigation.*`, `history.search`, read-only `bookmarks.*`, `runtime.onInstalled` / `onStartup`. |
| `4c2890b4ea1` | Stops content scripts running inside the documents of SVG images. |
| `66109c94250` | Lets `chrome-extension:` URLs bypass the page's Content Security Policy, so Vimium's HUD and Vomnibar frames load. |
| `3e271cdbcea` | Adds `TODO.md`. |
| `24e4b3bcf0f` | Gives tabs their tab-strip order (`tab.index`), which fixes Vimium's reversed `J` / `K`. |
| `c836168a572` | A script's ctrl/cmd click on a link opens it in a new tab, if the page has transient user activation (Vimium `F`). |
| `44d371f6570`, `3bc8a269fb5`, `d2c495f7e07` | Extensions' pages may read and write the clipboard (Vimium `yy`, `p`, `P`). |
| `3d8b5e05318` | Focusing a frame's window keeps the frame's focused element (Vimium's find bar kept losing focus). |
| `09724bdb333` | No focus ring around a focused iframe (the blue outline around the Vomnibar). |
| `80f6a92b8eb` | Content scripts' `console.log` reaches the terminal. |
| `567672df201` | `tabs.move` (Vimium `<<` / `>>`), within a window; tabs report `pinned`. |
| `927566db3de` | `sessions.restore()` reopens the most recently closed tab (Vimium `X`). |
| `4205c821878` | `search.query`, with the search engine chosen in settings (searching from Vimium's `o`). |
| `2363946e694`, `7f78b453e1c` | The `_favicon/` endpoint, scaled to the requested size (icons in Vimium's `T`). |
| `f9a3a3748a7`, `c795f44bd0a` | The browser process checks which extension a page may speak for, and limits content scripts to messaging and storage. |
| `617357367ee` | Extensions are held to the permissions and host permissions their manifests ask for. |
| `9764428c80b` | A page's CSP no longer applies Trusted Types or inline-style rules to content scripts' DOM changes (Vimium on strict sites such as ai.google). |
| `ee07bc3403f` | An `about:extensions` page: loaded extensions with icon, version, details and an Options button, and the ones that failed to load. |
| `30dd514e73f` | Pages of an extension load when the browser itself opens them (options page, `tabs.create`, popups); before, only pages already hosting the extension could read its files. |
| `2fd7010643e` | Toolbar buttons for extensions (`chrome.action`): icon, title, badge per tab, `onClicked`, and popups sized to their page. |
| `7fd30c09527` | `about:extensions` turns extensions on and off (kept in `<profile data>/Extensions/extensions.json`) and removes them, deleting what they stored. |
| `3ed85fc4beb` | `about:extensions` reloads an extension from its folder, and tries again to load a folder that failed. |
| `a49aaa5cd4d` | `WebDriver --load-extension=<dir>` passes the option on to the browser, for scripted tests. |

### Where the code lives

**New code**

- `Libraries/LibWeb/Extensions/ContentScripts.{h,cpp}`: decides which content scripts and CSS match a document, runs
  them at `document_start` / `document_end` / `document_idle`, and sets up extension pages.
- `Libraries/LibWeb/Extensions/ExtensionContext.{h,cpp}`: the `chrome.*` JavaScript surface given to each context and
  the `nativeCall` bridge that sends API calls to the browser process over IPC.
- `Libraries/LibWebView/Extensions.{h,cpp}`: loading, IDs, and access to extension files.
- `Libraries/LibWebView/ExtensionHost.{h,cpp}` (about 1,400 lines): carries out every API call in the browser
  process and dispatches events.
- `Libraries/LibWebCommon/Extensions/MatchPattern.{h,cpp}` and `FrameId.h`, plus `Tests/LibWeb/TestExtensionMatchPatterns.cpp`.

**Changes to existing engine code** (these are the parts that touch upstream behaviour)

- `LibURL/Rust/src/origin.rs`: `chrome-extension://<id>` gets a tuple origin of its own.
- `LibWebCommon/SecureContexts/AbstractOperations.cpp`: `chrome-extension:` origins count as potentially trustworthy
  (secure contexts).
- `LibWebCommon/Fetch/Infrastructure/URL.h`, `LibWeb/Fetch/Fetching/Fetching.cpp`, `LibWeb/Loader/ResourceLoader.*`:
  `chrome-extension:` is a fetch scheme whose files are opened by the browser process and handed to the renderer.
- `LibWeb/ContentSecurityPolicy/*`: requests, responses and navigations to `chrome-extension:` URLs skip the page's CSP.
- `LibWeb/DOM/Document.*`, `LibWeb/HTML/LocalNavigable.cpp`: hooks that run content scripts and hold each
  extension's isolated world per document.
- `LibWeb/CSS/StyleScope.cpp`: content-script CSS joins the user style sheet.
- `Services/WebContent/*.ipc`, `ConnectionFromClient.*`, `PageClient.*`: new IPC messages for API calls, events,
  script injection and file requests.
- `LibWebView/Application.*`, `WebContentPage.*`, `CanonicalNavigable.cpp`, `CanonicalTraversable.cpp`,
  `ViewImplementation.cpp`, `Services/Hub/*`, `UI/Qt/*`: lifecycle hooks, background views and tab order.

Every hook returns early when no extension is loaded, so a browser started without `--load-extension` should behave
as before.

### What works

Tested in the Qt UI with Vimium 2.4.2 and real input: `j`/`k`/`d`/`u`/`gg`/`G` scrolling, `f` and `F` link hints,
`t`, `x`, `X`, `^`, `yt`, `yy`, `p`/`P`, `r`, `H`/`L`, `o` (URLs and searches), `b`, `T` (with favicons), `gi`,
`i`/`Esc`, `J`/`K`, `<<`/`>>`, `/` find, zoom (`zi`/`zo`/`z0`), help (`?`).

---

## 2. What else is needed

### Bugs from interactive testing

All fixed. `TODO.md` is the live checklist.

### Gaps in what is implemented

- `tabs.move` can't move a tab to another window, and `tabs.onMoved` is never fired.
- `sessions.restore` reopens only the most recent tab; there is no `sessions.getRecentlyClosed` or session ID.
- `search.query` fails until the user chooses a search engine (Ladybird has none by default).
- `_favicon/` serves no default icon, nothing larger than 1x for HiDPI, and a favicon from history only if it is
  already small enough (decoding happens in another process).
- `tabs.update({pinned})` does nothing, so Vimium's pin toggle doesn't work.

### APIs still missing

- `runtime.connect` and ports (many extensions other than Vimium need these).
- `runtime.sendMessage` reaches only the background page, not other extension pages.
- Writing bookmarks, `notifications`, `alarms`, `contextMenus`.
- `declarativeNetRequest` / `webRequest`: needed by any ad blocker. Not started.
- Manifest V3 service workers. Background pages are run as MV2-style pages; MV3 extensions that rely on a service
  worker lifecycle won't behave correctly.

### UI

- `about:extensions` turns extensions on and off, reloads and removes them. Content scripts already running in a
  page keep running until it reloads, as in Chromium. Removing one loaded with `--load-extension` lasts until
  Ladybird next starts with that option, since that is the only way to install one so far.
- Every extension with an action gets a toolbar button; there is no overflow menu or pinning, so many extensions
  crowd the toolbar. No context menu on the button, `action.openPopup` is not implemented, and the button is not
  shown in the sidebar layouts' collapsed rail.
- A way to install extensions from the browser: a Load Unpacked button (needs a folder picker from the UI) and
  packed extensions (`.crx` / `.zip`, which needs a zip reader; LibCompress has only deflate).
- Only one window exists as far as extensions are concerned (window id 1).

### Housekeeping

- Run `clang-format` on the new code (it has never been formatted).
- Move the test extensions and test pages out of the session scratchpad into the repo, and add automated tests
  (currently only match patterns have unit tests).
- The security items in section 4.

---

## 3. Choices the upstream Ladybird project has not made

Upstream Ladybird has **no extension support** at all. I found no written policy on extensions in this checkout
(`CONTRIBUTING.md`, `Documentation/`); the closest thing upstream ships is the built-in **content blocker**
(`Document::content_blocker_style_sheet`), which covers the most popular reason people install extensions without
running third-party code in the browser. So this branch takes a direction upstream hasn't. Specifically:

1. **Adopting the Chrome extension model at all.** WebExtensions is not a web standard in the sense that the HTML,
   DOM and Fetch specs are: the W3C WebExtensions Community Group has no finished spec, and behaviour is defined by
   Chromium's and Firefox's implementations. Ladybird's code style is built around following specs step by step;
   most of this branch is unavoidably `AD-HOC:` / `INTEROP:` code with links to Chrome's developer docs instead of
   spec steps.
2. **Naming the scheme `chrome-extension:`** and copying Chromium's behaviour (IDs, origins, API shapes) so that
   existing extensions work unchanged. Firefox uses `moz-extension:` and random per-install UUIDs; there is no
   `browser.*` alias yet.
3. **Special cases inside core engine code.** Origin computation, secure-context checks, CSP, Fetch, the resource
   loader, `Document` and style cascading now all know about extensions. Upstream tends to keep such things out of
   the core algorithms.
4. **A large browser-process component** (`ExtensionHost`, about 1,400 lines) that accepts calls from renderers and
   can open tabs, navigate them, inject script into them, and read history and bookmarks. That widens what a
   compromised renderer can ask the browser process to do (see below).
5. **Content-script CSS cascades as a user style sheet**, not an author one as in Chrome (a known FIXME; it changes
   how `!important` and specificity interact with the page).
6. **Background pages run in hidden `HeadlessWebView`s**, rather than in a dedicated extension process type.
7. **Script ctrl/cmd-clicks open new tabs** (`c836168a572`). Upstream deliberately leaves the ctrl/cmd click alone in
   the renderer so that only the browser process acts on the user's real clicks. The change lets any page script do
   this too, but only while the page has transient user activation, which is what Chromium does.

If the intent is ever to upstream this, expect resistance mainly on points 1, 3 and 4. Keeping it as a fork-only
feature is the realistic assumption.

---

## 4. Does any of it make the browser less secure?

**Short answer:** with no extension loaded, very little changes. Almost every new path checks `has_extensions()` or
returns early. **With an extension loaded, there are real gaps**, several of which matter even if you trust the
extension, because they let a *compromised web content process* or a *malicious web page* take advantage of the
extension's privileges. The three high-severity items below are fixed, and the first two medium ones; the rest remain.

### High

1. **Fixed: the browser process trusted the renderer about which extension is calling.**
   `ExtensionHost::handle_api_call` now works out who is calling from what the page hosts (`ExtensionHost::caller_for`),
   not from the `extension_id` in the IPC message. A page hosting a document of the extension's own origin gets the
   full API. A page whose documents the extension's content scripts match, or that the extension injected a script
   into (`scripting.executeScript`, isolated world), counts as a content script: it may only message the extension and
   use storage (session storage once `storage.session.setAccessLevel` allows it), and its frame ID and URL are taken
   from the browser's own records. Any other page is refused. Answers to events are also accepted only from the page
   the event went to. What remains is item 2: a web page that shares a process with an extension frame can still act
   as that extension.

2. **Fixed: extension frames could share a process with web content.** Site isolation (on by default) already put
   cross-origin `chrome-extension:` frames, such as Vimium's HUD and Vomnibar, in processes of their own, but what a
   process may do for an extension was decided by what it reported hosting, and file reads trusted every page of a
   background view, so a web iframe inside a background page or popup could read all of the extension's files. Now the
   UI process records which extension's documents it gave each process (`WebContentClient::note_hosted_extension`),
   and `CanonicalNavigable::process_to_host` keeps them apart as it does WebUIs: a process that hosted an extension's
   documents hosts only that extension's documents and about:blank documents nothing else can reach, and the
   extension's documents go to no other process. Full API access and file reads go by
   `WebContentClient::may_speak_for_extension`. Remaining: with site isolation off (only possible when debugging
   WebContent), extension frames stay in the page's process, which may then act as the extension.

3. **Fixed: no permission enforcement.** API namespaces that need a permission (`storage`, `history`, `bookmarks`,
   `sessions`, `search`, `scripting`, `webNavigation`, ...) exist only with it, and the browser process refuses calls
   without it. `scripting.*` needs host permissions for every frame it touches (`allFrames` skips the rest). Tab URLs,
   titles and favicons are only shown with `tabs` or host access, in results, `sender.tab` and `tabs` events;
   `webNavigation` events need their permission; `permissions.contains`/`getAll` answer from the manifest. Not yet:
   `activeTab` (no toolbar button or commands to grant it), optional permissions (`permissions.request`), and the
   per-site "allow access to file URLs" switch Chromium has.

### Medium

4. **Fixed: `web_accessible_resources` `matches` were not checked.** A WebContent process now says which origin made each
   `chrome-extension:` request (`LoadRequest::origin`), and the browser process believes it only for the site whose
   documents it gave that process (`WebContentClient::may_make_requests_for_origin`). A Manifest V3 entry's resources
   then load only for origins its `matches` cover, or for the extensions its `extension_ids` name (`*` for all);
   Manifest V2 entries stay open to every page, as in Chromium. Remaining: `use_dynamic_url` is ignored; an opaque
   origin (a sandboxed frame) matches nothing, where Chromium goes by the origin it came from; and a process that has
   hosted documents of several sites (only without site isolation) is believed for any origin.

5. **Fixed: CSP bypass for all `chrome-extension:` URLs.** A page's Content Security Policy now leaves alone only
   requests for files of a loaded extension that it makes web accessible to the request's origin
   (`Extensions::request_bypasses_content_security_policy`), by the same rule the UI process applies
   (`Web::Extensions::is_web_accessible_resource` in LibWebCommon). Chromium and Firefox exempt extension resources in
   the same way. The policy governs every other `chrome-extension:` load.

6. **`chrome-extension:` origins are treated as secure contexts.** This is correct and matches other browsers, but
   it means anything running at that origin (including in a shared process, item 2) gets the powerful APIs that
   require secure contexts.

### Low or acceptable

7. **Isolated worlds.** Content scripts get their own JS globals, so the page cannot directly reach `chrome.*` or
   `nativeCall`. While their script runs, the page's Trusted Types and inline-style rules don't apply to what they do
   to the DOM (`Extensions::is_running_in_an_isolated_world`), as in Chromium; inline scripts they insert are still
   subject to the page's policy. This matches Chrome's model. The `MAIN` world option of `scripting.executeScript` deliberately runs
   in the page's world, as in Chrome; such code must not be given privileged objects.

8. **Script ctrl/cmd-click opening tabs.** Needs transient user activation, so a page cannot spam new
   tabs; Chromium behaves the same. Small increase in what pages can do; acceptable.

9. **Files on disk.** Extension files are read only by the browser process and passed to renderers as file
   descriptors. Paths come from parsed URLs under a known extension's directory, and `Tests/LibWebView/TestExtensions.cpp`
   checks that `..` segments, percent-encoded or not, absolute paths and symbolic links cannot leave it. The tests
   found that a web page could get past the web-accessible check with an encoded slash: with `sub/*` web accessible,
   `sub/..%2fpage.html` opened `page.html`. The decoded path is now resolved before it is checked or opened.

10. **Storage, history and bookmarks.** `storage.json` is plain JSON in the profile directory, which is the same as
    Chrome's unencrypted extension storage. The Hub opens its own connection to the history database, which adds
    another process that reads browsing history.

11. **Loading is command-line only and unpacked.** There is no signature checking, store or update channel. That
    is fine for a developer build, but a user can be talked into running a malicious unpacked extension by changing
    how the browser is launched.

### Suggested order of fixes

1. ~~Track, in the browser process, which WebContent page hosts which extension's contexts, and reject API calls that
   don't match (item 1).~~ Done. File reads already decide by what the page hosts.
2. ~~Run extension frames and background pages in their own WebContent processes (item 2).~~ Done. This also
   tightens items 5 and 6.
3. ~~Enforce `permissions` and `host_permissions` (item 3).~~ Done.
4. ~~Check `web_accessible_resources` `matches`, and limit the CSP bypass to web-accessible resources (items 4 and
   5).~~ Done.
5. ~~Add tests for path traversal in `open_extension_file` (item 9).~~ Done.

---

## 5. Picking the work up again

- Build: `ninja -C Build/release`. Run: `Build/release/bin/Ladybird.app/Contents/MacOS/Ladybird
  --load-extension=<unpacked extension dir> <url>` (repeat `--load-extension` for more; log stderr to a file).
- Tests: `Build/release/bin/TestLibWeb --suite TestExtensionMatchPatterns`, and
  `Build/release/bin/test-web --test-path Tests/LibWeb --filter "*navigation*"` as a regression check.
- WebDriver tests: `WebDriver --headless --port=<p> --profile-path=<abs dir> --load-extension=<dir>`, then drive
  `about:extensions` and test pages over HTTP (execute/sync, element click, screenshot). Background pages show up
  as extra window handles.
- Headless scripted tests: `--expose-internals-object --headless=screenshot` with a page that calls
  `internals.sendTextThroughUIProcess` (Vimium ignores untrusted events). That flag also fakes user activation in
  the UI process, so clipboard and new-tab behaviour must be confirmed with real input.
- The test extensions and pages (a Vimium 2.4.2 copy, probes for tab order, search, favicons, a toy messaging
  extension) have lived in temporary session folders and are not in the repo; keeping them is a `TODO.md` item.
- Code map: `Libraries/LibWeb/Extensions/` (content scripts, the `chrome.*` JS surface), `Libraries/LibWebView/
  ExtensionHost.cpp` (every API call, in the browser process), `Libraries/LibWebView/Extensions.cpp` (loading and
  file access). New Hub↔UI messages: `tab_order_changed` (UI→Hub), `view_move_tab` and
  `app_reopen_recently_closed_tab` (Hub→UI).
- Suggested next steps, in order: the remaining high-severity security item in section 4 (2); `runtime.connect`;
  `tabs.onMoved` and pinning; clang-format the new code. New Hub↔UI messages for the toolbar buttons:
  `app_extension_actions_changed`, `app_show_extension_popup`, `app_extension_popup_did_request_size` (Hub→UI) and
  `extension_action_clicked` (UI→Hub).
