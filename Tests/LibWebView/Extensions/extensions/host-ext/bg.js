const log = (s) => console.log("HOSTPROBE " + s);
const attempt = async (label, fn) => { try { log(`${label}: ${JSON.stringify(await fn())}`); } catch (e) { log(`${label}: ERROR ${e.message}`); } };
let done = false;
chrome.tabs.onUpdated.addListener(async (tabId, change, tab) => {
    if (change.status !== "complete" || done) return;
    done = true;
    log(`tab url visible: ${tab.url}`);
    const frames = await chrome.webNavigation.getAllFrames({ tabId });
    log(`frames: ${JSON.stringify(frames.map((f) => [f.frameId, f.url]))}`);
    const sub = frames.find((f) => f.frameId !== 0);
    await attempt("top", () => chrome.scripting.executeScript({ target: { tabId }, func: () => location.host }));
    await attempt("allFrames", () => chrome.scripting.executeScript({ target: { tabId, allFrames: true }, func: () => location.host }));
    await attempt("subframe by id", () => chrome.scripting.executeScript({ target: { tabId, frameIds: [sub.frameId] }, func: () => location.host }));
    await attempt("insertCSS sub", () => chrome.scripting.insertCSS({ target: { tabId, frameIds: [sub.frameId] }, css: "body{}" }));
    await attempt("contains localhost", () => chrome.permissions.contains({ origins: ["http://localhost/*"] }));
    await attempt("contains tabs", () => chrome.permissions.contains({ permissions: ["tabs"] }));
});
