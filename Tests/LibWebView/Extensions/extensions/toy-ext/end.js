toyLog.push(`end: readyState=${document.readyState}`);
toyLog.push(`page variable visible to extension: ${typeof window.pageVariable !== "undefined"}`);
toyLog.push(`getURL: ${chrome.runtime.getURL("/icons/x.png")}`);
toyLog.push(`manifest name: ${chrome.runtime.getManifest().name}`);
const marker = document.createElement("div");
marker.id = "toy-marker";
marker.textContent = "toy extension was here";
document.body.prepend(marker);
