chrome.notifications.onClicked.addListener(id => console.log(`NOTIFY clicked ${id}`));
chrome.notifications.onClosed.addListener((id, byUser) => console.log(`NOTIFY closed ${id} byUser=${byUser}`));
