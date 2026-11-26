// The badge says whether the extension can script the active tab right now. Clicking the button grants it activeTab
// access to the tab; navigating the tab to another origin should take that away again.
async function probe() {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    if (!tab) return;
    let text = "no";
    try {
        await chrome.scripting.executeScript({ target: { tabId: tab.id }, func: () => true });
        text = tab.url ? "yes" : "yes?";
    } catch (e) {
        text = tab.url ? "url" : "no";
    }
    await chrome.action.setBadgeText({ text });
}

chrome.action.onClicked.addListener(async (tab) => {
    console.log("activeTab probe clicked; tab.url =", tab.url);
    try {
        await chrome.scripting.executeScript({
            target: { tabId: tab.id },
            func: () => {
                const banner = document.createElement("div");
                banner.textContent = "Injected by the Active Tab probe";
                banner.style = "position:fixed;bottom:0;left:0;right:0;background:#cfc;padding:8px;font:16px sans-serif";
                document.body.append(banner);
            },
        });
    } catch (e) {
        console.log("activeTab probe could not inject:", e.message);
    }
    probe();
});

setInterval(probe, 1000);
