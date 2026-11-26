setTimeout(async () => {
    try {
        await chrome.action.openPopup();
        console.log("PROBE2 openPopup ok");
    } catch (e) {
        console.log("PROBE2 openPopup error " + e.message);
    }
}, 5000);
