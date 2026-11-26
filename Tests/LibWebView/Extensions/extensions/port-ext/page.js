const port = chrome.runtime.connect({ name: "page" });
port.onMessage.addListener(message => console.log("PORT page got " + JSON.stringify(message)));
port.postMessage("from page");
