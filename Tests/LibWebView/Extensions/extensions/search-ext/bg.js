const log = (s) => console.log("SEARCH " + s);
const dump = (reason) => chrome.tabs.query({}, tabs => log(reason + " " + tabs.sort((a, b) => a.index - b.index).map(t => t.index + ":" + t.url + (t.active ? "*" : "")).join(" ")));
const call = (q) => chrome.search.query(q).then(() => log("ok " + JSON.stringify(q)), e => log("error " + JSON.stringify(q) + " " + e.message));
setTimeout(() => call({ text: "ladybird current" }), 2000);
setTimeout(() => dump("after current"), 4000);
setTimeout(() => call({ text: "ladybird newtab", disposition: "NEW_TAB" }), 4500);
setTimeout(() => dump("after newtab"), 6500);
setTimeout(() => call({ text: "x", disposition: "NEW_TAB", tabId: 1 }), 7000);
