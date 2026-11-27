// Drives the steps of bg.js from a page, with an extension page (frame.html) in a frame of it.
const log = text => console.log("MSG " + text);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function main() {
    const frame = document.createElement("iframe");
    frame.src = chrome.runtime.getURL("frame.html");
    document.body.append(frame);
    await sleep(2000);

    log("-- content script sends ask-frame (bg listens but doesn't answer)");
    log(`cs response=${await chrome.runtime.sendMessage({ kind: "ask-frame" })}`);
    log("-- content script sends ask-bg");
    log(`cs response=${await chrome.runtime.sendMessage({ kind: "ask-bg" })}`);

    log("-- bg sends ask-frame");
    await chrome.runtime.sendMessage({ kind: "do", step: "send-to-frame" });
    log("-- bg connects to its pages");
    await chrome.runtime.sendMessage({ kind: "do", step: "connect-to-frame" });

    log("-- content script connects (bg and frame take the port)");
    const port = chrome.runtime.connect({ name: "cs-port" });
    port.onMessage.addListener(message => log(`cs-port got ${JSON.stringify(message)}`));
    port.onDisconnect.addListener(() => log(`cs-port onDisconnect lastError=${chrome.runtime.lastError?.message}`));
    port.postMessage("hi");
    await sleep(2500);

    log("-- frame connects (bg takes the port, the frame itself doesn't)");
    frame.contentWindow.postMessage("connect", "*");
    await sleep(1500);

    log("-- tabs.sendMessage to a tab without a listener");
    await chrome.runtime.sendMessage({ kind: "do", step: "no-listener" });

    log("-- the frame navigates away while bg has a port to it");
    await chrome.runtime.sendMessage({ kind: "do", step: "port-to-gone-frame" });
    frame.src = "about:blank";
    await sleep(1500);

    log("-- a new frame is removed while bg has a port to it");
    const second = document.createElement("iframe");
    second.src = chrome.runtime.getURL("frame.html");
    document.body.append(second);
    await sleep(1500);
    await chrome.runtime.sendMessage({ kind: "do", step: "port-to-gone-frame" });
    second.remove();
    await sleep(1500);
    log("done");
}

main().catch(error => log("error " + error));
