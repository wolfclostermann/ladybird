// Each command and click is logged in storage.local as "log", for page.html and the WebDriver test to read.
async function log(line) {
    console.log(`COMMANDS ${line}`);
    const { log = [] } = await chrome.storage.local.get("log");
    log.push(line);
    await chrome.storage.local.set({ log });
}

// Running a command gives the extension the tab through activeTab, so it may script the page without host permissions.
chrome.commands.onCommand.addListener(async (command, tab) => {
    let marked;
    try {
        await chrome.scripting.executeScript({
            target: { tabId: tab.id },
            func: name => {
                document.documentElement.dataset.command = name;
            },
            args: [command],
        });
        marked = "marked";
    } catch (error) {
        marked = `not marked: ${error.message}`;
    }
    await log(`command ${command} ${tab.url ?? "(no url)"} ${marked}`);
});

chrome.action.onClicked.addListener(tab => log(`action ${tab.url ?? "(no url)"}`));
