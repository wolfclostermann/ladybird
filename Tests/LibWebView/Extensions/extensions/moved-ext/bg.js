// Opens three more tabs, moves tabs in several ways and logs each tabs.onMoved event and the order after each step, as
// "MOVED ..." console lines. Drag a tab in the Qt UI afterwards to see the event for a move the user makes.
const log = text => console.log("MOVED " + text);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

const names = new Map();
const name = id => names.get(id) ?? "?" + id;

chrome.tabs.onMoved.addListener((tabId, info) =>
    log(`onMoved ${name(tabId)} ${info.fromIndex}->${info.toIndex} window=${info.windowId}`)
);

async function order(step) {
    const tabs = await chrome.tabs.query({});
    tabs.sort((a, b) => a.index - b.index);
    log(`order after ${step}: ${tabs.map(t => name(t.id) + (t.pinned ? "^" : "")).join(" ")}`);
}

async function run() {
    await sleep(3000);
    const [first] = await chrome.tabs.query({ active: true });
    names.set(first.id, "A");
    for (const n of ["B", "C", "D"]) {
        const tab = await chrome.tabs.create({ url: "about:blank#" + n, active: false });
        names.set(tab.id, n);
    }
    await sleep(1500);
    await order("create");

    const id = n => [...names].find(([, v]) => v === n)[0];
    const steps = [
        ["move A to 2", () => chrome.tabs.move(id("A"), { index: 2 })],
        ["move D to 0", () => chrome.tabs.move(id("D"), { index: 0 })],
        ["move [D, B] to end", () => chrome.tabs.move([id("D"), id("B")], { index: -1 })],
        ["pin C", () => chrome.tabs.update(id("C"), { pinned: true })],
        ["unpin C", () => chrome.tabs.update(id("C"), { pinned: false })],
        ["move B to its own place", () => chrome.tabs.move(id("B"), { index: 3 })],
    ];
    for (const [step, action] of steps) {
        log(`-- ${step}`);
        await action();
        await sleep(1000);
        await order(step);
    }
    log("done");
}

run().catch(e => log("error " + e));
