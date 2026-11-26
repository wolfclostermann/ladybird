// Pins the active tab, checks the result, the tabs.onUpdated event and tabs.query({pinned}), then unpins it again,
// as Vimium's pin toggle does. Results are logged as "PIN ..." console lines.
const log = text => console.log("PIN " + text);

chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
    if ("pinned" in changeInfo) log(`onUpdated ${tabId} pinned=${changeInfo.pinned} tab.pinned=${tab.pinned}`);
});

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

async function run() {
    await sleep(3000);
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    log(`start ${tab.id} pinned=${tab.pinned} index=${tab.index}`);

    const pinned = await chrome.tabs.update(tab.id, { pinned: !tab.pinned });
    log(`update result pinned=${pinned.pinned}`);
    await sleep(1000);
    log(`query pinned=true -> ${(await chrome.tabs.query({ pinned: true })).map(t => t.id).join(",")}`);
    log(`get pinned=${(await chrome.tabs.get(tab.id)).pinned}`);

    const unpinned = await chrome.tabs.update(tab.id, { pinned: tab.pinned });
    log(`update back result pinned=${unpinned.pinned}`);
    await sleep(1000);
    log(`get after toggle back pinned=${(await chrome.tabs.get(tab.id)).pinned}`);
    log("done");
}

run().catch(e => log("error " + e));
