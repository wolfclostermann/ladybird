const log = s => console.log("NOPERM " + s);
log(
    `namespaces: storage=${typeof chrome.storage} history=${typeof chrome.history} scripting=${typeof chrome.scripting} webNavigation=${typeof chrome.webNavigation} tabs=${typeof chrome.tabs}`
);
let done = false;
chrome.tabs.onUpdated.addListener(async (tabId, change, tab) => {
    log(`onUpdated change=${JSON.stringify(change)} tab.url=${tab.url} tab.title=${tab.title}`);
    if (change.status !== "complete" || done) return;
    done = true;
    const tabs = await chrome.tabs.query({});
    log(`query: ${JSON.stringify(tabs.map(t => [t.id, t.url, t.title]))}`);
    log(`query by url: ${(await chrome.tabs.query({ url: "<all_urls>" })).length}`);
    log(`get: url=${(await chrome.tabs.get(tabId)).url}`);
    log(`contains tabs: ${await chrome.permissions.contains({ permissions: ["tabs"] })}`);
    log(`contains origin: ${await chrome.permissions.contains({ origins: ["<all_urls>"] })}`);
    log(`getAll: ${JSON.stringify(await chrome.permissions.getAll())}`);
});
