// Logs the events of chrome.history as "HISTORY ..." console lines: each visit the history records, and what is
// removed from it (about:history's Remove and Forget, or clearing the history in about:settings).
const log = text => console.log("HISTORY " + text);

chrome.history.onVisited.addListener(item =>
    log(
        `onVisited ${item.url} title=${JSON.stringify(item.title)} visits=${item.visitCount} ` +
            `typed=${item.typedCount} recent=${Math.abs(Date.now() - item.lastVisitTime) < 60000}`
    )
);
chrome.history.onVisitRemoved.addListener(removed =>
    log(`onVisitRemoved allHistory=${removed.allHistory} urls=${JSON.stringify(removed.urls)}`)
);
log("ready");
