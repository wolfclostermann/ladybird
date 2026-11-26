// Checks runtime.connect and tabs.connect, logging "PORT ..." console lines. Content scripts on example.com and
// example.org connect here; page.html connects from an extension page.
const log = text => console.log("PORT " + text);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

chrome.runtime.onConnect.addListener(port => {
    log(`bg onConnect ${port.name} sender.url=${port.sender?.url} tab=${port.sender?.tab?.id} frame=${port.sender?.frameId}`);
    port.onMessage.addListener((message, receivingPort) => {
        log(`bg got ${JSON.stringify(message)} on ${port.name} same port=${receivingPort === port}`);
        if (message === "close-me") port.disconnect();
        else port.postMessage({ echo: message });
    });
    port.onDisconnect.addListener(p => log(`bg onDisconnect ${p.name} lastError=${chrome.runtime.lastError?.message}`));
});

async function run() {
    await sleep(4000);
    const [tab] = await chrome.tabs.query({ url: "https://example.com/*" });

    const toTab = chrome.tabs.connect(tab.id, { name: "to-tab" });
    toTab.onMessage.addListener(message => {
        log(`bg got from tab ${JSON.stringify(message)}`);
        toTab.disconnect();
        log("bg disconnected to-tab");
    });
    toTab.postMessage("ping");
    await sleep(1000);

    const [welcome] = await chrome.tabs.query({ url: "about:*" });
    const nobody = chrome.tabs.connect(welcome?.id ?? 999999, { name: "nobody" });
    nobody.onDisconnect.addListener(() => log(`bg nobody onDisconnect lastError=${chrome.runtime.lastError?.message}`));
    await sleep(1000);

    log("-- navigating the tab");
    await chrome.tabs.update(tab.id, { url: "https://example.org/" });
    await sleep(3000);

    log("-- opening page.html");
    await chrome.tabs.create({ url: "page.html", active: false });
    await sleep(2000);

    log("-- closing the tab");
    await chrome.tabs.remove(tab.id);
    await sleep(1000);
    log("done");
}

run().catch(e => log("error " + e));
