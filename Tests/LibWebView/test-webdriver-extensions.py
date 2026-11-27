#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# Loads two of the extensions in Extensions/extensions/ through WebDriver and checks what they do: content scripts run
# and stop when the extension is turned off on about:extensions, the browser opens the extension's own pages while a
# web page can't, and messages and ports reach every page of the extension.

import argparse
import http.server
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


def extension_ids(session):
    session.go("about:extensions")
    rows = session.wait_for(
        "return [...document.querySelectorAll('.extension')].map(row => [row.querySelector('.extension-name').textContent, row.dataset.id])",
        lambda rows: len(rows) == 2,
        "both extensions on about:extensions",
    )
    ids = {}
    for text, extension_id in rows:
        ids["marker" if "Marker" in text else "messages"] = extension_id
    assert set(ids) == {"marker", "messages"}, rows
    return ids


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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    webdriver_binary = parser.parse_args().webdriver_binary

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), WebPageHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    web_page = f"http://127.0.0.1:{server.server_address[1]}/"

    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        for name in ("mark-ext", "msg-ext"):
            shutil.copytree(EXTENSIONS / name, work / name)

        process, port = helpers["launch_webdriver"](
            webdriver_binary,
            "--headless",
            f"--profile-path={work / 'profile'}",
            f"--load-extension={work / 'mark-ext'}",
            f"--load-extension={work / 'msg-ext'}",
        )
        session = None
        try:
            session = Session(port, helpers["create_session"](port))
            ids = extension_ids(session)
            check_content_scripts(session, web_page, ids["marker"])
            check_extension_pages(session, web_page, ids["marker"])
            check_messaging(session, ids["messages"])
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
                server.shutdown()


if __name__ == "__main__":
    main()
