// Opens two tabs and closes them, then lists and restores them through chrome.sessions, as "SESSIONS ..." console lines.
const log = text => console.log("SESSIONS " + text);
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

chrome.sessions.onChanged.addListener(() => log("onChanged"));

const describe = sessions =>
    sessions
        .map(s =>
            s.tab ? `tab ${s.tab.sessionId} ${s.tab.url}` : `window ${s.window.sessionId} (${s.window.tabs.length})`
        )
        .join(" | ");

async function open_tabs() {
    const tabs = await chrome.tabs.query({});
    return tabs
        .map(t => t.url)
        .sort()
        .join(" ");
}

async function run() {
    await sleep(3000);
    const one = await chrome.tabs.create({ url: "https://example.com/#one", active: false });
    const two = await chrome.tabs.create({ url: "https://example.org/#two", active: false });
    await sleep(2500);
    await chrome.tabs.remove(one.id);
    await sleep(500);
    await chrome.tabs.remove(two.id);
    await sleep(1000);

    const closed = await chrome.sessions.getRecentlyClosed();
    log(`recently closed: ${describe(closed)}`);
    log(`maxResults 1: ${describe(await chrome.sessions.getRecentlyClosed({ maxResults: 1 }))}`);
    log(`lastModified is recent: ${Math.abs(closed[0].lastModified - Date.now() / 1000) < 60}`);
    log(`devices: ${JSON.stringify(await chrome.sessions.getDevices())}`);

    // The older one by its ID, so not what restore(null) would bring back.
    const older = closed.find(s => s.tab && s.tab.url.includes("#one"));
    const restored = await chrome.sessions.restore(older.tab.sessionId);
    log(`restored by id: ${restored.tab.url}`);
    await sleep(1500);
    log(`open tabs: ${await open_tabs()}`);
    log(`recently closed now: ${describe(await chrome.sessions.getRecentlyClosed())}`);

    const latest = await chrome.sessions.restore();
    log(`restored latest: ${latest.tab.url}`);
    await sleep(1500);
    log(`open tabs: ${await open_tabs()}`);

    await chrome.sessions.restore("999999").then(
        () => log("bad id: resolved"),
        e => log(`bad id: ${e.message}`)
    );
    log("done");
}

run().catch(e => log("error " + e));
