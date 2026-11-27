const closedList = document.getElementById("closed");
const eventList = document.getElementById("events");

async function showClosed() {
    const sessions = await chrome.sessions.getRecentlyClosed({ maxResults: 8 });
    closedList.replaceChildren();
    if (sessions.length === 0)
        closedList.append(Object.assign(document.createElement("li"), { textContent: "Nothing." }));
    for (const session of sessions) {
        const tab = session.tab ?? session.window.tabs[0];
        const id = session.tab?.sessionId ?? session.window.sessionId;
        const item = document.createElement("li");
        const label = document.createElement("span");
        label.textContent = (session.window ? "[window] " : "") + (tab.title || tab.url);
        label.title = tab.url;
        const button = document.createElement("button");
        button.textContent = "Restore";
        button.onclick = () => chrome.sessions.restore(id).then(showClosed);
        item.append(label, button);
        closedList.append(item);
    }
}

function addEvent(line, atTop = true) {
    const item = Object.assign(document.createElement("li"), { textContent: line });
    if (atTop) eventList.prepend(item);
    else eventList.append(item);
}

// The events come from the background page over a port.
const port = chrome.runtime.connect({ name: "popup" });
port.onMessage.addListener(message => {
    document.getElementById("status").textContent = "(live, over a port)";
    if (message.history) message.history.forEach(line => addEvent(line, false));
    if (message.event) addEvent(message.event);
});
port.onDisconnect.addListener(() => {
    document.getElementById("status").textContent = `(port closed: ${chrome.runtime.lastError?.message ?? "no error"})`;
});
chrome.sessions.onChanged.addListener(showClosed);
showClosed();
