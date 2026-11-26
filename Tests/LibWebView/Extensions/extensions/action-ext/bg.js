const log = (...a) => console.log("ACTIONTEST", ...a);
(async () => {
    try {
        await chrome.action.setBadgeText({ text: "42" });
        await chrome.action.setBadgeBackgroundColor({ color: "#d00" });
        log(
            "badge",
            await chrome.action.getBadgeText({}),
            JSON.stringify(await chrome.action.getBadgeBackgroundColor({}))
        );
        log("title", await chrome.action.getTitle({}));
        await chrome.action.setTitle({ title: "Changed title" });
        log("title2", await chrome.action.getTitle({}));
        const canvas = new OffscreenCanvas(16, 16);
        const ctx = canvas.getContext("2d");
        ctx.fillStyle = "rgb(0, 160, 0)";
        ctx.fillRect(0, 0, 16, 16);
        await chrome.action.setIcon({ imageData: ctx.getImageData(0, 0, 16, 16) });
        log("imageData icon set");
        await chrome.action.setIcon({ path: "icon16.png" });
        log("path icon set");
        try {
            await chrome.action.setIcon({ path: "../../../etc/passwd" });
            log("ESCAPE ALLOWED");
        } catch (e) {
            log("escape refused", e.message);
        }
        log("popup", JSON.stringify(await chrome.action.getPopup({})), "enabled", await chrome.action.isEnabled());
        const [tab] = await chrome.tabs.query({ active: true });
        if (tab) {
            await chrome.action.setBadgeText({ text: "tab", tabId: tab.id });
            log(
                "tab badge",
                await chrome.action.getBadgeText({ tabId: tab.id }),
                "global",
                await chrome.action.getBadgeText({})
            );
            await chrome.action.setBadgeText({ text: null, tabId: tab.id });
            log("tab badge reset", await chrome.action.getBadgeText({ tabId: tab.id }));
        }
        log("done");
    } catch (e) {
        log("FAIL", e.message);
    }
})();
chrome.action.onClicked.addListener(async tab => {
    log("clicked", tab.id, tab.url);
    await chrome.action.setBadgeText({ text: "ok", tabId: tab.id });
    await chrome.action.setPopup({ popup: "popup.html" });
});
