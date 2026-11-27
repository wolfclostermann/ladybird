// Checks that messages and ports reach every page of the extension, not only its background page, logging "MSG ..."
// console lines. content.js on a 127.0.0.1 page adds frame.html (an extension page) to the page and drives the steps.
const log = text => console.log("MSG " + text);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.kind === "do")
        return void run(message.step, sender).then(sendResponse, error => sendResponse("error " + error.message)), true;
    log(`bg got ${message.kind} from ${sender.url?.startsWith("chrome-extension:") ? "frame" : "content script"}`);
    if (message.kind === "ask-bg")
        sendResponse("bg-answer");
});

chrome.runtime.onConnect.addListener(port => {
    log(`bg onConnect ${port.name}`);
    port.onMessage.addListener(message => {
        log(`bg got ${JSON.stringify(message)} on ${port.name}`);
        port.postMessage(`bg-echo ${message}`);
    });
    port.onDisconnect.addListener(() => log(`bg onDisconnect ${port.name} lastError=${chrome.runtime.lastError?.message}`));
    if (port.name === "cs-port")
        setTimeout(() => { log("bg lets go of cs-port"); port.disconnect(); }, 1000);
});

async function run(step, sender) {
    if (step === "send-to-frame") {
        const response = await chrome.runtime.sendMessage({ kind: "ask-frame" });
        log(`bg sendMessage response=${response}`);
        return;
    }
    if (step === "connect-to-frame") {
        const port = chrome.runtime.connect({ name: "bg-port" });
        port.onMessage.addListener(message => {
            log(`bg-port got ${JSON.stringify(message)}`);
            port.disconnect();
            log("bg-port disconnected");
        });
        port.onDisconnect.addListener(() => log(`bg-port onDisconnect lastError=${chrome.runtime.lastError?.message}`));
        port.postMessage("hello frame");
        await sleep(1000);
        return;
    }
    if (step === "no-listener") {
        try {
            const response = await chrome.tabs.sendMessage(sender.tab.id, { kind: "nobody" });
            log(`bg tabs.sendMessage resolved ${response}`);
        } catch (error) {
            log(`bg tabs.sendMessage rejected ${error.message}`);
        }
        return;
    }
    if (step === "port-to-gone-frame") {
        const port = chrome.runtime.connect({ name: "nav-port" });
        port.onDisconnect.addListener(() => log(`nav-port onDisconnect lastError=${chrome.runtime.lastError?.message}`));
        await sleep(500);
        return;
    }
}
