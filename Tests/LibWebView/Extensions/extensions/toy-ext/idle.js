toyLog.push(`idle: readyState=${document.readyState}`);
toyLog.push(`document.body.dataset.page = ${document.body.dataset.page}`);
const show = () => { console.log("TOY " + toyLog[toyLog.length-1]); document.getElementById("toy-log").textContent = toyLog.join("\n"); };
show();
chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    toyLog.push(`tab got ${JSON.stringify(message)} from ${sender.id === chrome.runtime.id ? "own extension" : sender.id}`);
    show();
    sendResponse({ pong: true });
});
chrome.storage.onChanged.addListener((changes, area) => { toyLog.push(`onChanged ${area} ${JSON.stringify(changes)}`); show(); });
(async () => {
    try {
        toyLog.push(`sync: ${JSON.stringify(await chrome.runtime.sendMessage({ kind: "sync", text: "hi" }))}`);
        toyLog.push(`async: ${JSON.stringify(await chrome.runtime.sendMessage({ kind: "async" }))}`);
        toyLog.push(`promise: ${JSON.stringify(await chrome.runtime.sendMessage({ kind: "promise" }))}`);
        chrome.runtime.sendMessage({ kind: "sync", text: "callback" }, (r) => { toyLog.push(`callback: ${JSON.stringify(r)}`); show(); });
        try { await chrome.tabs.create({}); } catch (e) { toyLog.push(`expected error: ${e.message}`); }
    } catch (e) {
        toyLog.push(`ERROR ${e}`);
    }
    show();
})();
setTimeout(async () => {
    history.pushState({}, "", "#pushed");
    location.hash = "frag";
    await new Promise((r) => setTimeout(r, 300));
    const report = await chrome.runtime.sendMessage({ kind: "phase3" });
    for (const line of report) { toyLog.push(line); console.log("PHASE3 " + line); }
    show();
}, 300);
