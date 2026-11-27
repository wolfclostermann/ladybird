// Click a button to show a notification, then click or dismiss it; the background page logs NOTIFY lines.
const log = line => (document.querySelector("#log").textContent += `${line}\n`);
// Chromium asks for an icon, which Ladybird doesn't show.
const show = (id, options) =>
    chrome.notifications.create(id, { iconUrl: "missing.png", ...options }).then(
        shownId => log(`created ${shownId}`),
        error => log(`error ${error.message}`)
    );

document.querySelector("#basic").onclick = () =>
    show("", { type: "basic", title: "Basic", message: "A basic notification", contextMessage: "From the probe" });
document.querySelector("#list").onclick = () =>
    show("list", {
        type: "list",
        title: "List",
        message: "Some items",
        items: [
            { title: "One", message: "first" },
            { title: "Two", message: "second" },
        ],
    });
document.querySelector("#progress").onclick = () =>
    show("progress", { type: "progress", title: "Progress", message: "Working", progress: 10 });
document.querySelector("#update").onclick = () =>
    chrome.notifications.update("progress", { progress: 80 }).then(updated => log(`updated ${updated}`));
document.querySelector("#clear").onclick = async () => {
    for (const id of Object.keys(await chrome.notifications.getAll()))
        log(`cleared ${id} ${await chrome.notifications.clear(id)}`);
};
