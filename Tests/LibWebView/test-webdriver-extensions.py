#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# Loads some of the extensions in Extensions/extensions/ through WebDriver and checks what they do: content scripts run
# and stop when the extension is turned off on about:extensions, the browser opens the extension's own pages while a
# web page can't, messages and ports reach every page of the extension, and keyboard shortcuts run an extension's
# commands and can be changed on about:extensions. It also drives the rest of about:extensions: Reload, Remove, Try
# Again for a folder that couldn't be loaded, and folders installed with Load Unpacked…. Options, Load Unpacked… and
# Install from File… open a tab or ask for a file, which a browser without a UI can't, so they are tried by hand.

import argparse
import contextlib
import http.server
import json
import runpy
import shutil
import subprocess
import tempfile
import threading
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))
EXTENSIONS = Path(__file__).parent / "Extensions" / "extensions"

WEB_PAGE = b"<!doctype html><title>web page</title><p>A page for content scripts"


class WebPageHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(WEB_PAGE)))
        self.end_headers()
        self.wfile.write(WEB_PAGE)

    def log_message(self, format, *args):
        pass


class Session:
    def __init__(self, port, session_id):
        self.port = port
        self.id = session_id

    def request(self, method, path, body=None):
        status, payload, raw = helpers["request"](self.port, method, f"/session/{self.id}{path}", body)
        assert status == 200, f"{method} {path} failed with HTTP {status}: {raw}"
        return payload["value"]

    def run(self, script, *args):
        return self.request("POST", "/execute/sync", {"script": script, "args": list(args)})

    # Runs script with a callback as its last argument, which it calls with the result.
    def run_async(self, script, *args):
        return self.request("POST", "/execute/async", {"script": script, "args": list(args)})

    def go(self, url):
        self.request("POST", "/url", {"url": url})

    def new_tab(self):
        handle = self.request("POST", "/window/new", {"type": "tab"})["handle"]
        self.request("POST", "/window", {"handle": handle})
        return handle

    def switch_to(self, handle):
        self.request("POST", "/window", {"handle": handle})

    # Clicks an element as the user would, which lets the page do what needs a click, such as opening a tab.
    def click(self, css):
        element = self.request("POST", "/element", {"using": "css selector", "value": css})
        self.request("POST", f"/element/{next(iter(element.values()))}/click", {})

    def handles(self):
        return self.request("GET", "/window/handles")

    # Waits for script to return what is_done accepts, and returns that.
    def wait_for(self, script, is_done, description):
        deadline = time.monotonic() + helpers["EVENT_TIMEOUT_SECONDS"]
        while True:
            value = self.run(script)
            if is_done(value):
                return value
            if time.monotonic() >= deadline:
                raise AssertionError(f"Timed out waiting for {description}; last saw {value!r}")
            time.sleep(0.1)


# Messages an extension page sends, answered with their response or "error <message>".
SEND_MESSAGE = """
const [message, done] = arguments;
chrome.runtime.sendMessage(message).then(
    response => done(response === undefined ? "undefined" : response),
    error => done("error " + error.message));
"""

# Opens a port from an extension page, posts a message on it, and collects the replies for a while.
CONNECT_AND_POST = """
const [name, message, done] = arguments;
const port = chrome.runtime.connect({ name });
const replies = [];
port.onMessage.addListener(reply => replies.push(reply));
port.onDisconnect.addListener(() => replies.push("disconnected " + chrome.runtime.lastError?.message));
port.postMessage(message);
setTimeout(() => { port.disconnect(); done(replies.sort()); }, 1000);
"""


# The IDs of the loaded extensions, by the first word of each one's name.
def extension_ids(session, count):
    session.go("about:extensions")
    rows = session.wait_for(
        "return [...document.querySelectorAll('.extension')].map(row => [row.querySelector('.extension-name').firstChild.textContent, row.dataset.id])",
        lambda rows: len(rows) == count,
        f"{count} extensions on about:extensions",
    )
    return {name.split()[0].lower(): extension_id for name, extension_id in rows}


def check_content_scripts(session, web_page, marker_id):
    session.go(web_page)
    session.wait_for(
        "return document.documentElement.getAttribute('data-marker')",
        lambda value: value == "on",
        "Marker's content script",
    )

    # Turning the extension off keeps its content scripts out of pages loaded afterwards.
    session.go("about:extensions")
    session.run(
        "document.querySelector(`.extension[data-id='${arguments[0]}'] input[switch]`).click()",
        marker_id,
    )
    session.wait_for(
        f"return document.querySelector(`.extension[data-id='{marker_id}']`)?.classList.contains('disabled')",
        lambda value: value is True,
        "Marker to be turned off",
    )
    session.go(web_page)
    time.sleep(0.5)
    marker = session.run("return document.documentElement.getAttribute('data-marker')")
    assert marker is None, f"Marker's content script ran while it was off: {marker!r}"

    session.go("about:extensions")
    session.run(
        "document.querySelector(`.extension[data-id='${arguments[0]}'] input[switch]`).click()",
        marker_id,
    )
    session.go(web_page)
    session.wait_for(
        "return document.documentElement.getAttribute('data-marker')",
        lambda value: value == "on",
        "Marker's content script after turning it on again",
    )
    print("PASS: content scripts run, and stop while the extension is off")


def check_extension_pages(session, web_page, marker_id):
    options = f"chrome-extension://{marker_id}/options.html"

    # WebDriver, like the browser's UI, may open an extension's page.
    session.go(options)
    title = session.wait_for("return document.title", lambda title: title == "Marker options", "the options page")
    assert title == "Marker options", title

    # A web page may not: the options page isn't one of the files the extension makes accessible to the web.
    session.go(web_page)
    session.run("location.href = arguments[0]", options)
    session.wait_for("return location.href", lambda url: url != web_page, "the web page to navigate")
    title = session.run("return document.title")
    assert title != "Marker options", "A web page navigated to an extension's page"

    session.go(web_page)
    session.run(
        "const frame = document.createElement('iframe'); frame.src = arguments[0]; document.body.append(frame)", options
    )
    time.sleep(1)
    session.request("POST", "/frame", {"id": 0})
    title = session.run("return document.title")
    session.request("POST", "/frame/parent", {})
    assert title != "Marker options", "A web page framed an extension's page"
    print("PASS: WebDriver opens an extension's page, and a web page can't")


def check_messaging(session, messages_id):
    page = f"chrome-extension://{messages_id}/frame.html"
    first = session.new_tab()
    session.go(page)
    session.wait_for("return typeof chrome?.runtime?.sendMessage", lambda value: value == "function", "chrome.runtime")

    # The background page listens for ask-frame without answering, and the page that sends it doesn't receive it.
    response = session.run_async(SEND_MESSAGE, {"kind": "ask-frame"})
    assert response == "undefined", f"ask-frame with one page: {response!r}"
    response = session.run_async(SEND_MESSAGE, {"kind": "ask-bg"})
    assert response == "bg-answer", f"ask-bg: {response!r}"

    # A second page of the extension, in another tab, answers ask-frame.
    second = session.new_tab()
    session.go(page)
    session.wait_for("return typeof chrome?.runtime?.sendMessage", lambda value: value == "function", "chrome.runtime")
    session.switch_to(first)
    response = session.run_async(SEND_MESSAGE, {"kind": "ask-frame"})
    assert response == "frame-answer", f"ask-frame with two pages: {response!r}"

    # A port connects to the background page and to the other page, which both echo what it is sent.
    replies = session.run_async(CONNECT_AND_POST, "test-port", "hi")
    assert replies == ["bg-echo hi", "frame-echo hi"], f"replies on a port: {replies!r}"

    # Once the other page is gone, only the background page is left to take the port.
    session.switch_to(second)
    session.request("DELETE", "/window")
    session.switch_to(first)
    replies = session.run_async(CONNECT_AND_POST, "test-port", "hello")
    assert replies == ["bg-echo hello"], f"replies once the other page closed: {replies!r}"
    print("PASS: messages and ports reach every page of the extension but the sender")


def set_enabled(session, extension_id, enabled):
    session.go("about:extensions")
    row = f".extension[data-id='{extension_id}']"
    # The page asks for the list of extensions once it has loaded, and draws it when the answer comes.
    session.wait_for(
        f"return document.querySelector(`{row}`) !== null", lambda found: found, "the extension on about:extensions"
    )
    if session.run(f"return document.querySelector(`{row} input[switch]`).checked") != enabled:
        session.run(f"document.querySelector(`{row} input[switch]`).click()")
    session.wait_for(
        f"return document.querySelector(`{row}`).classList.contains('disabled')",
        lambda disabled: disabled is not enabled,
        f"the extension to be turned {'on' if enabled else 'off'}",
    )


def marker_on(session, web_page):
    session.go(web_page)
    # The content script runs at document_end, which has passed once a script can run.
    return session.run("return document.documentElement.getAttribute('data-marker')")


def click_row_button(session, extension_id, text):
    session.wait_for(
        f"return [...document.querySelectorAll(`.extension[data-id='{extension_id}'] button`)].some(b => b.textContent === arguments[0])".replace(
            "arguments[0]", repr(text)
        ),
        lambda found: found,
        f"the {text} button on about:extensions",
    )
    session.run(
        f"[...document.querySelectorAll(`.extension[data-id='{extension_id}'] button`)].find(b => b.textContent === arguments[0]).click()",
        text,
    )


def check_state_after_restart(session, web_page, marker_id):
    session.go("about:extensions")
    disabled = session.wait_for(
        f"return document.querySelector(`.extension[data-id='{marker_id}']`)?.classList.contains('disabled')",
        lambda value: value is not None,
        "Marker on about:extensions",
    )
    assert disabled is True, "Marker was on again after a restart"
    assert marker_on(session, web_page) is None, "Marker's content script ran after a restart while it was off"
    set_enabled(session, marker_id, True)
    print("PASS: an extension turned off stays off after a restart")


def check_reload_and_remove(session, web_page, work, marker_id):
    folder = work / "mark-ext"
    content_script = folder / "cs.js"
    manifest = folder / "manifest.json"

    # Reload picks up the extension's changed files.
    content_script.write_text(content_script.read_text().replace('"on"', '"v2"'))
    session.go("about:extensions")
    click_row_button(session, marker_id, "Reload")
    session.wait_for(
        f"return document.querySelector(`.extension[data-id='{marker_id}']`) !== null",
        lambda value: value,
        "Marker on about:extensions after reloading",
    )
    deadline = time.monotonic() + helpers["EVENT_TIMEOUT_SECONDS"]
    while (marker := marker_on(session, web_page)) != "v2" and time.monotonic() < deadline:
        time.sleep(0.2)
    assert marker == "v2", f"The reloaded content script set {marker!r}"

    # A reload that fails keeps the version that was loaded, and says why.
    original_manifest = manifest.read_text()
    manifest.write_text("{ broken")
    session.go("about:extensions")
    click_row_button(session, marker_id, "Reload")
    failure = session.wait_for(
        f"return document.querySelector(`.extension[data-id='{marker_id}'] .extension-failure`)?.textContent ?? null",
        lambda value: value is not None,
        "the reload to fail",
    )
    assert "Could not reload" in failure, failure
    assert marker_on(session, web_page) == "v2", "A failed reload took away the extension"
    manifest.write_text(original_manifest)

    # Removing it takes its content scripts out of pages and deletes what it stored, but not its folder.
    storage = [path for path in (work / "profile").rglob(marker_id) if path.parent.name == "Extensions"]
    assert storage, "Marker stored nothing, so its removal can't be checked"
    session.go("about:extensions")
    click_row_button(session, marker_id, "Remove")
    assert session.run("return document.querySelector('#remove-dialog').open") is True, "No dialog asked to confirm"
    session.run("document.querySelector('#remove-dialog button[value=remove]').click()")
    session.wait_for(
        f"return document.querySelector(`.extension[data-id='{marker_id}']`) === null",
        lambda gone: gone,
        "Marker to leave about:extensions",
    )
    assert marker_on(session, web_page) is None, "A removed extension's content script still ran"
    assert not any(path.exists() for path in storage), f"What Marker stored is still there: {storage}"
    assert folder.exists(), "Removing an extension deleted the folder it was loaded from"
    print("PASS: reloading and removing an extension")


def check_options_tab_closes(session, marker_id):
    # Turning an extension off closes the tabs that show its pages. (Options opens one in a new tab, which a browser
    # without a UI can't do, so WebDriver opens it here.)
    main_tab = session.request("GET", "/window")
    options_tab = session.new_tab()
    session.go(f"chrome-extension://{marker_id}/options.html")
    session.wait_for("return document.title", lambda title: title == "Marker options", "the options page")
    session.switch_to(main_tab)

    set_enabled(session, marker_id, False)
    deadline = time.monotonic() + helpers["EVENT_TIMEOUT_SECONDS"]
    while options_tab in session.handles():
        assert time.monotonic() < deadline, "The options page stayed open after its extension was turned off"
        time.sleep(0.1)
    set_enabled(session, marker_id, True)
    print("PASS: turning an extension off closes its pages")


def load_errors(session):
    return session.run(
        "return [...document.querySelectorAll('.load-error')].map(row => row.querySelector('.load-error-directory').textContent)"
    )


def click_load_error_button(session, directory, text):
    session.run(
        "const row = [...document.querySelectorAll('.load-error')].find(row => row.querySelector('.load-error-directory').textContent === arguments[0]);"
        "[...row.querySelectorAll('button')].find(button => button.textContent === arguments[1]).click()",
        directory,
        text,
    )


def check_try_again(session, broken, good_manifest):
    # A folder that couldn't be loaded is listed with why, and Try Again loads it once it is fixed.
    session.go("about:extensions")
    directory = session.wait_for(
        "return [...document.querySelectorAll('.load-error .load-error-directory')].map(e => e.textContent)",
        lambda directories: any(d.endswith(broken.name) for d in directories),
        "the broken folder on about:extensions",
    )
    directory = next(d for d in directory if d.endswith(broken.name))
    click_load_error_button(session, directory, "Try Again")
    time.sleep(0.5)
    assert directory in load_errors(session), "A folder still broken left the list of failures"

    (broken / "manifest.json").write_text(good_manifest)
    click_load_error_button(session, directory, "Try Again")
    session.wait_for(
        "return [...document.querySelectorAll('.extension .extension-name')].map(e => e.firstChild.textContent)",
        lambda names: "Second" in names,
        "the fixed folder to load",
    )
    assert directory not in load_errors(session), "A folder that loaded is still listed as a failure"
    print("PASS: Try Again loads a folder once it is fixed")


def extensions_state(work):
    (path,) = [path for path in (work / "profile").rglob("extensions.json") if path.parent.name == "Extensions"]
    return path


def check_installed_folders(session, web_page, work, installed, gone):
    # A folder installed with Load Unpacked… loads without --load-extension.
    assert marker_on(session, web_page) == "on", "The installed extension's content script didn't run"

    session.go("about:extensions")
    (installed_id,) = session.wait_for(
        "return [...document.querySelectorAll('.extension')].filter(row => row.querySelector('.extension-name').firstChild.textContent === 'Installed').map(row => row.dataset.id)",
        lambda ids: len(ids) == 1,
        "the installed extension on about:extensions",
    )
    assert str(gone) in load_errors(session), load_errors(session)

    # Load Unpacked… does nothing without a UI to choose a folder in.
    names = session.run("return [...document.querySelectorAll('.extension')].map(row => row.dataset.id)")
    session.click("#load-unpacked")
    time.sleep(0.5)
    assert session.run("return [...document.querySelectorAll('.extension')].map(row => row.dataset.id)") == names

    # Removing it says nothing of --load-extension, and it isn't loaded from its folder again.
    click_row_button(session, installed_id, "Remove")
    assert session.run("return document.querySelector('#remove-dialog-command-line').hidden") is True
    session.run("document.querySelector('#remove-dialog button[value=remove]').click()")
    session.wait_for(
        f"return document.querySelector(`.extension[data-id='{installed_id}']`) === null",
        lambda removed: removed,
        "the installed extension to leave about:extensions",
    )
    assert str(installed) not in extensions_state(work).read_text(), "The removed folder is still installed"

    # Remove on a folder that couldn't be loaded stops loading it.
    click_load_error_button(session, str(gone), "Remove")
    session.wait_for(
        "return [...document.querySelectorAll('.load-error .load-error-directory')].map(e => e.textContent)",
        lambda directories: str(gone) not in directories,
        "the missing folder to leave about:extensions",
    )
    assert str(gone) not in extensions_state(work).read_text(), "The missing folder is still installed"
    print("PASS: installed folders load without the option, and are removed or forgotten")


# Alt and Shift, as internals.sendKeyThroughUIProcess takes modifiers.
ALT_SHIFT = 1 | 4

# Records the keydown events the page sees, by their codes.
RECORD_KEYS = """
window.keysSeen = [];
addEventListener("keydown", event => keysSeen.push(event.code), true);
"""


def press(session, key, modifiers=ALT_SHIFT):
    session.run("internals.sendKeyThroughUIProcess(arguments[0], arguments[1])", key, modifiers)


def commands_log(session, commands_id):
    session.go(f"chrome-extension://{commands_id}/page.html")
    return session.wait_for(
        "return document.querySelector('#log').textContent",
        lambda text: text.strip() != "",
        "the commands page",
    ).split("\n")


def shortcut_of(lines, command):
    for line in lines:
        if line.startswith(f"{command}: "):
            return line.split(": ", 1)[1]
    raise AssertionError(f"No command {command} in {lines!r}")


def is_alt_shift(shortcut, key):
    # As Chromium shows them on macOS and elsewhere.
    return shortcut in (f"⌥⇧{key}", f"Alt+Shift+{key}")


def check_commands(session, web_page, commands_id):
    # The manifest's suggested keys are the shortcuts; a command without one has none.
    lines = commands_log(session, commands_id)
    assert is_alt_shift(shortcut_of(lines, "mark-tab"), "K"), lines
    assert is_alt_shift(shortcut_of(lines, "_execute_action"), "J"), lines
    assert shortcut_of(lines, "unbound") == "(none)", lines

    # A command's shortcut runs it on the tab, which activeTab lets the extension script, and the page never sees the
    # key. Other keys reach the page.
    session.go(web_page)
    session.run(RECORD_KEYS)
    press(session, "K")
    session.wait_for(
        "return document.documentElement.dataset.command ?? null",
        lambda value: value == "mark-tab",
        "the command to mark the tab",
    )
    press(session, "L")
    keys = session.wait_for("return keysSeen", lambda keys: "KeyL" in keys, "a key without a command to reach the page")
    assert "KeyK" not in keys, f"The page saw the key of a command: {keys!r}"

    # The command that clicks the toolbar button does what a click does.
    press(session, "J")
    deadline = time.monotonic() + helpers["EVENT_TIMEOUT_SECONDS"]
    while True:
        lines = commands_log(session, commands_id)
        if any(line.startswith("action ") for line in lines):
            break
        assert time.monotonic() < deadline, f"The toolbar button's command didn't run: {lines!r}"
        time.sleep(0.2)
    assert f"command mark-tab {web_page} marked" in lines, lines
    assert f"action {web_page}" in lines, lines
    print("PASS: shortcuts run an extension's commands, and the page doesn't see their keys")


def shortcut_field(session, commands_id, command):
    return session.run(
        "return document.querySelector(`.shortcuts-extension[data-id='${arguments[0]}'] .shortcut-row[data-command='${arguments[1]}'] .shortcut-field`)?.textContent ?? null",
        commands_id,
        command,
    )


def record_shortcut(session, commands_id, command, key):
    session.run(
        """const field = document.querySelector(`.shortcuts-extension[data-id='${arguments[0]}'] .shortcut-row[data-command='${arguments[1]}'] .shortcut-field`);
        field.focus();
        field.click();""",
        commands_id,
        command,
    )
    press(session, key)


def check_changing_shortcuts(session, web_page, commands_id):
    session.go("about:extensions#shortcuts")
    session.wait_for(
        "return document.querySelector('#shortcuts-view').classList.contains('hidden')",
        lambda hidden: hidden is False,
        "the list of shortcuts",
    )

    # Pressing keys in a command's field gives it them; a command's own keys reach the page here, to be chosen.
    record_shortcut(session, commands_id, "mark-tab", "M")
    session.wait_for(
        f"return document.querySelector(`.shortcuts-extension[data-id='{commands_id}'] .shortcut-row[data-command='mark-tab'] .shortcut-field`)?.textContent",
        lambda text: is_alt_shift(text, "M"),
        "the new shortcut on about:extensions",
    )

    # A shortcut another command has is refused, saying who has it.
    record_shortcut(session, commands_id, "unbound", "J")
    failure = session.wait_for(
        f"return document.querySelector(`.shortcuts-extension[data-id='{commands_id}'] .shortcut-row[data-command='unbound'] .shortcut-failure`)?.textContent ?? null",
        lambda text: text is not None,
        "the shortcut in use to be refused",
    )
    assert "Commands probe" in failure, failure
    assert shortcut_field(session, commands_id, "unbound") == "Not set"

    # The old keys now reach the page, and the new ones run the command.
    session.go(web_page)
    session.run(RECORD_KEYS)
    press(session, "K")
    session.wait_for("return keysSeen", lambda keys: "KeyK" in keys, "the old shortcut's key to reach the page")
    press(session, "M")
    session.wait_for(
        "return document.documentElement.dataset.command ?? null",
        lambda value: value == "mark-tab",
        "the new shortcut to run the command",
    )

    # Removing a shortcut leaves the command without one.
    session.go("about:extensions#shortcuts")
    session.wait_for(
        f"return document.querySelector(`.shortcuts-extension[data-id='{commands_id}'] .shortcut-row[data-command='_execute_action'] .shortcut-clear`) !== null",
        lambda found: found,
        "the list of shortcuts",
    )
    session.run(
        f"document.querySelector(`.shortcuts-extension[data-id='{commands_id}'] .shortcut-row[data-command='_execute_action'] .shortcut-clear`).click()"
    )
    session.wait_for(
        f"return document.querySelector(`.shortcuts-extension[data-id='{commands_id}'] .shortcut-row[data-command='_execute_action'] .shortcut-field`)?.textContent",
        lambda text: text == "Not set",
        "the shortcut to be removed",
    )
    print("PASS: about:extensions changes and removes shortcuts, and refuses one in use")


NOTIFICATIONS_API = """
const [done] = arguments;
(async () => {
    const results = {};
    const outcome = promise => promise.then(value => value, error => "error " + error.message);
    results.missing = await outcome(chrome.notifications.create({ type: "basic", title: "No message" }));
    results.created = await outcome(chrome.notifications.create("named", { type: "basic", title: "T", message: "M" }));
    results.generated = await outcome(chrome.notifications.create({ type: "basic", title: "T", message: "M" }));
    results.level = await chrome.notifications.getPermissionLevel();
    // Without a UI, the platform shows nothing, so nothing is left to clear.
    results.cleared = await chrome.notifications.clear("named");
    results.updated = await chrome.notifications.update("named", { title: "U" });
    done(results);
})();
"""


def check_notifications_api(session, notify_id, commands_id):
    session.go(f"chrome-extension://{notify_id}/page.html")
    results = session.run_async(NOTIFICATIONS_API)
    assert results["missing"].startswith("error Some of the required properties are missing"), results
    assert results["created"] == "named", results
    assert len(results["generated"]) == 32, results
    assert results["level"] == "granted", results
    assert results["cleared"] is False and results["updated"] is False, results

    # An extension without the permission has no chrome.notifications.
    session.go(f"chrome-extension://{commands_id}/page.html")
    assert session.run("return typeof chrome.notifications") == "undefined"
    print("PASS: chrome.notifications checks what it is asked, and needs its permission")


def check_shortcuts_after_restart(session, commands_id):
    lines = commands_log(session, commands_id)
    assert is_alt_shift(shortcut_of(lines, "mark-tab"), "M"), lines
    assert shortcut_of(lines, "_execute_action") == "(none)", lines
    print("PASS: the shortcuts the user chose stay after a restart")


@contextlib.contextmanager
def browser(webdriver_binary, work, extensions=("mark-ext", "msg-ext"), *arguments):
    process, port = helpers["launch_webdriver"](
        webdriver_binary,
        "--headless",
        f"--profile-path={work / 'profile'}",
        *(f"--load-extension={work / name}" for name in extensions),
        *arguments,
    )
    session = None
    try:
        session = Session(port, helpers["create_session"](port))
        yield session
    finally:
        try:
            if session is not None:
                helpers["request"](port, "DELETE", f"/session/{session.id}")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    webdriver_binary = parser.parse_args().webdriver_binary

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), WebPageHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    web_page = f"http://127.0.0.1:{server.server_address[1]}/"

    try:
        with tempfile.TemporaryDirectory() as work:
            work = Path(work)
            for name in ("mark-ext", "msg-ext", "cmd-ext", "notify-ext"):
                shutil.copytree(EXTENSIONS / name, work / name)

            # A copy of Marker under another name, whose manifest is broken at first.
            broken = work / "second-ext"
            shutil.copytree(EXTENSIONS / "mark-ext", broken)
            good_manifest = (broken / "manifest.json").read_text().replace('"Marker"', '"Second"')
            (broken / "manifest.json").write_text("{ not json")

            with browser(webdriver_binary, work) as session:
                ids = extension_ids(session, 2)
                check_options_tab_closes(session, ids["marker"])
                check_content_scripts(session, web_page, ids["marker"])
                check_extension_pages(session, web_page, ids["marker"])
                check_messaging(session, ids["message"])
                set_enabled(session, ids["marker"], False)

            with browser(webdriver_binary, work, ("mark-ext", "msg-ext", "second-ext")) as session:
                check_state_after_restart(session, web_page, ids["marker"])
                check_reload_and_remove(session, web_page, work, ids["marker"])
                check_try_again(session, broken, good_manifest)

            # The internals object presses keys as the user would, through the browser process.
            with browser(webdriver_binary, work, ("cmd-ext", "notify-ext"), "--expose-internals-object") as session:
                ids = extension_ids(session, 2)
                commands_id = ids["commands"]
                check_notifications_api(session, ids["notifications"], commands_id)
                check_commands(session, web_page, commands_id)
                check_changing_shortcuts(session, web_page, commands_id)

            # Folders the user installed with Load Unpacked… are kept in the profile; one of them has since gone.
            installed = work / "installed-ext"
            shutil.copytree(EXTENSIONS / "mark-ext", installed)
            manifest = installed / "manifest.json"
            manifest.write_text(manifest.read_text().replace('"Marker"', '"Installed"'))
            gone = work / "gone-ext"
            state_path = extensions_state(work)
            state = json.loads(state_path.read_text())
            state["unpacked"] = [str(installed.resolve()), str(gone)]
            state_path.write_text(json.dumps(state))

            with browser(webdriver_binary, work, ("cmd-ext",)) as session:
                check_shortcuts_after_restart(session, commands_id)
                check_installed_folders(session, web_page, work, installed.resolve(), gone)
    finally:
        server.shutdown()


if __name__ == "__main__":
    main()
