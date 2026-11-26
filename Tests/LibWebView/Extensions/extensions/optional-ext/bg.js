// tools/optional_permissions.py drives this background page. What happens is kept in self.log for it to read.
// Clicking the button requests the permissions in self.wanted, with the user activation permissions.request needs.
self.log = [];
self.wanted = {};
chrome.permissions.onAdded.addListener(permissions => self.log.push({ added: permissions }));
chrome.permissions.onRemoved.addListener(permissions => self.log.push({ removed: permissions }));

const button = document.createElement("button");
button.id = "request";
button.textContent = "Request";
button.addEventListener("click", () => {
    const wanted = self.wanted;
    chrome.permissions.request(wanted).then(
        granted => self.log.push({ request: wanted, granted }),
        error => self.log.push({ request: wanted, error: error.message })
    );
});
(document.body ?? document.documentElement).append(button);
