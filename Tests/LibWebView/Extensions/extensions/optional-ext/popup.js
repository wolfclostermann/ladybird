// For trying permissions.request in the browser: the first button should bring up the browser's permission prompt.
const wanted = { permissions: ["history"], origins: ["https://www.example.org/*"] };
const state = document.getElementById("state");

async function show(note) {
    const all = await chrome.permissions.getAll();
    state.textContent = `${note}\n\nHeld now: ${JSON.stringify(all, null, 1)}`;
}

document.getElementById("ask").addEventListener("click", () => {
    chrome.permissions.request(wanted).then(
        (granted) => show(granted ? "Granted" : "Declined"),
        (error) => show(`Error: ${error.message}`));
});
document.getElementById("remove").addEventListener("click", () => {
    chrome.permissions.remove(wanted).then(() => show("Removed"), (error) => show(`Error: ${error.message}`));
});
show("");
