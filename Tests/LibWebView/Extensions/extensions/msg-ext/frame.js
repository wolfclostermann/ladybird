// An extension page in a frame of a web page, which hears the extension's messages and ports.
const log = text => console.log("MSG " + text);

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    log(`frame got ${message.kind} from ${sender.url?.startsWith("chrome-extension:") ? "extension page" : "content script"}`);
    if (message.kind === "ask-frame")
        sendResponse("frame-answer");
});

chrome.runtime.onConnect.addListener(port => {
    log(`frame onConnect ${port.name}`);
    port.onMessage.addListener(message => {
        log(`frame got ${JSON.stringify(message)} on ${port.name}`);
        port.postMessage(`frame-echo ${message}`);
        if (port.name === "cs-port")
            setTimeout(() => { log("frame lets go of cs-port"); port.disconnect(); }, 1500);
    });
    port.onDisconnect.addListener(() => log(`frame onDisconnect ${port.name} lastError=${chrome.runtime.lastError?.message}`));
});

window.addEventListener("message", async event => {
    if (event.data !== "connect")
        return;
    const port = chrome.runtime.connect({ name: "frame-port" });
    port.onMessage.addListener(message => log(`frame-port got ${JSON.stringify(message)}`));
    port.postMessage("from frame");
    log(`frame sendMessage ask-bg response=${await chrome.runtime.sendMessage({ kind: "ask-bg" })}`);
});

chrome.runtime.sendMessage({ kind: "frame-ready" }).then(response => log(`frame-ready response=${response}`));
