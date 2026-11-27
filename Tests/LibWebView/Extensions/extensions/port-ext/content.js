const log = text => console.log("PORT " + text);

const port = chrome.runtime.connect({ name: "cs-" + location.hostname });
port.onMessage.addListener(message => log(`cs got ${JSON.stringify(message)}`));
port.onDisconnect.addListener(() => log(`cs onDisconnect lastError=${chrome.runtime.lastError?.message}`));
port.postMessage("hello");
port.postMessage({ n: 1 });

const closer = chrome.runtime.connect({ name: "closer-" + location.hostname });
closer.onDisconnect.addListener(() => {
    log(`cs closer disconnected by bg, lastError=${chrome.runtime.lastError?.message}`);
    try {
        closer.postMessage("again");
        log("cs post after close: no throw");
    } catch (error) {
        log("cs post after close threw: " + error.message);
    }
});
closer.postMessage("close-me");

chrome.runtime.onConnect.addListener(port => {
    log(`cs onConnect ${port.name} sender.id ok=${port.sender?.id === chrome.runtime.id}`);
    port.onMessage.addListener(message => port.postMessage("cs-echo:" + message));
    port.onDisconnect.addListener(() =>
        log(`cs ${port.name} onDisconnect lastError=${chrome.runtime.lastError?.message}`)
    );
});
