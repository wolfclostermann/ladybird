import { greeting } from "./bg_module.js";
console.log(`toy background: ${greeting} origin=${location.origin} secure=${isSecureContext} id=${chrome.runtime.id}`);

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.kind === "sync") {
        sendResponse({ echo: message.text, tab: sender.tab?.id, frame: sender.frameId, url: sender.url });
        return;
    }
    if (message.kind === "async") {
        (async () => {
            const tabs = await chrome.tabs.query({ active: true });
            const count = ((await chrome.storage.local.get("count")).count ?? 0) + 1;
            await chrome.storage.local.set({ count });
            sendResponse({ tabs: tabs.length, count });
            // Reply into the tab too.
            chrome.tabs.sendMessage(sender.tab.id, { kind: "ping" }, reply =>
                console.log(`toy background: tab replied ${JSON.stringify(reply)}`)
            );
        })();
        return true;
    }
    if (message.kind === "promise") return Promise.resolve({ promised: true });
});

const seenEvents = [];
for (const name of ["onCommitted", "onCompleted", "onHistoryStateUpdated", "onReferenceFragmentUpdated"])
    chrome.webNavigation[name].addListener(details =>
        seenEvents.push(`webNavigation.${name} frame=${details.frameId} ${details.url.split("/").pop()}`)
    );
for (const name of ["onCreated", "onUpdated", "onRemoved", "onActivated"])
    chrome.tabs[name].addListener((...args) =>
        seenEvents.push(`tabs.${name} ${JSON.stringify(args[1] ?? args[0]).slice(0, 60)}`)
    );

chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.kind !== "phase3") return;
    (async () => {
        const report = [];
        const tabId = sender.tab.id;
        const attempt = async (label, fn) => {
            try {
                report.push(`${label}: ${JSON.stringify(await fn())}`);
            } catch (e) {
                report.push(`${label}: ERROR ${e.message}`);
            }
        };
        await attempt("executeScript func isolated", () =>
            chrome.scripting.executeScript({
                target: { tabId },
                func: (a, b) => [a + b, typeof pageSecret, typeof chrome.runtime],
                args: [2, 3],
            })
        );
        await attempt("executeScript func MAIN", () =>
            chrome.scripting.executeScript({
                target: { tabId },
                world: "MAIN",
                func: () => [typeof pageSecret, typeof chrome],
            })
        );
        await attempt("executeScript promise", () =>
            chrome.scripting.executeScript({
                target: { tabId },
                func: () => new Promise(r => setTimeout(() => r("later"), 50)),
            })
        );
        await attempt("executeScript files", () =>
            chrome.scripting.executeScript({ target: { tabId }, files: ["injected.js"] })
        );
        await attempt("executeScript allFrames", () =>
            chrome.scripting.executeScript({
                target: { tabId, allFrames: true },
                func: () => location.href.split("/").pop(),
            })
        );
        await attempt("insertCSS", () =>
            chrome.scripting.insertCSS({ target: { tabId }, css: "#toy-log { color: #b00; }" })
        );
        await attempt("setZoom 1.25", () => chrome.tabs.setZoom(tabId, 1.25));
        await attempt("getZoom", () => chrome.tabs.getZoom(tabId));
        await attempt("setZoom reset", () => chrome.tabs.setZoom(tabId, 0));
        await attempt("query active", async () =>
            (await chrome.tabs.query({ active: true })).map(t => [t.id, t.status])
        );
        await attempt("create", async () => {
            const t = await chrome.tabs.create({ url: "options.html", active: false });
            return [typeof t.id, t.pendingUrl];
        });
        await attempt("move (unimplemented)", () => chrome.tabs.move(tabId, { index: 0 }));
        report.push(...seenEvents);
        sendResponse(report);
    })();
    return true;
});
