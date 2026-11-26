function dump(reason) {
  chrome.tabs.query({}, tabs => {
    console.log("ORDER " + reason + " " + tabs.sort((a, b) => a.index - b.index).map(t => t.index + ":" + t.title + (t.active ? "*" : "")).join(" "));
  });
}
setInterval(() => dump("tick"), 2000);
// After the test page closes itself, restore it as Vimium's X does.
let restored = false;
chrome.tabs.onRemoved.addListener(() => {
  if (restored) return;
  restored = true;
  setTimeout(() => chrome.sessions.restore(null).then(r => console.log("ORDER restore resolved " + JSON.stringify(r)), e => console.log("ORDER restore failed " + e)), 1500);
});
