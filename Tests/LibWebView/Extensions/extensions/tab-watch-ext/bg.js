// Keeps the latest tab events and streams them over a port to the popup while it is open. Each event is also logged
// as a "WATCH ..." console line.
const events = [];
const popups = new Set();

async function note(text) {
    const line = `${new Date().toLocaleTimeString()} ${text}`;
    console.log("WATCH " + text);
    events.unshift(line);
    events.length = Math.min(events.length, 30);
    for (const port of popups) port.postMessage({ event: line });
}

const title = async id => {
    try {
        return (await chrome.tabs.get(id)).title || `tab ${id}`;
    } catch {
        return `tab ${id}`;
    }
};

chrome.tabs.onMoved.addListener(async (id, info) =>
    note(`moved "${await title(id)}" ${info.fromIndex} → ${info.toIndex}`)
);
chrome.tabs.onUpdated.addListener((id, change, tab) => {
    if ("pinned" in change) note(`${change.pinned ? "pinned" : "unpinned"} "${tab.title}"`);
});
chrome.sessions.onChanged.addListener(() => note("recently closed list changed"));

chrome.runtime.onConnect.addListener(port => {
    if (port.name !== "popup") return;
    popups.add(port);
    port.postMessage({ history: events });
    port.onDisconnect.addListener(() => popups.delete(port));
});
