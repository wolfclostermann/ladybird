chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    sendResponse({ got: message, url: sender.url });
});
chrome.tabs.onUpdated.addListener(async (tabId, change) => {
    if (change.status !== "complete") return;
    const [{ result }] = await chrome.scripting.executeScript({
        target: { tabId },
        func: async () => {
            try {
                return JSON.stringify(await chrome.runtime.sendMessage("hello from injected"));
            } catch (e) {
                return "ERROR " + e.message;
            }
        },
    });
    console.log("INJECT result: " + result);
});
console.log("INJECT bg started");
setTimeout(async () => {
    for (const tab of await chrome.tabs.query({})) {
        const [{ result }] = await chrome.scripting.executeScript({
            target: { tabId: tab.id },
            func: async () => {
                try {
                    return JSON.stringify(await chrome.runtime.sendMessage("hello from injected (late)"));
                } catch (e) {
                    return "ERROR " + e.message;
                }
            },
        });
        console.log("INJECT late result: " + result);
    }
}, 2000);
