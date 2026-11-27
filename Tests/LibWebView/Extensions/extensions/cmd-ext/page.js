// Shows the commands with their shortcuts, and what the background page logged.
async function show() {
    const commands = await chrome.commands.getAll();
    const { log = [] } = await chrome.storage.local.get("log");
    document.querySelector("#log").textContent = [
        ...commands.map(command => `${command.name}: ${command.shortcut || "(none)"}`),
        "",
        ...log,
    ].join("\n");
}
show();
chrome.storage.onChanged.addListener(show);
