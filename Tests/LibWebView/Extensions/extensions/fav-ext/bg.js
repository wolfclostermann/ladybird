const log = s => console.log("FAV " + s);
function probe(pageUrl) {
    const url = new URL(chrome.runtime.getURL("/_favicon/"));
    url.searchParams.set("pageUrl", pageUrl);
    url.searchParams.set("size", "16");
    const img = new Image();
    img.onload = () => log(`loaded ${pageUrl} ${img.naturalWidth}x${img.naturalHeight}`);
    img.onerror = () => log(`error ${pageUrl}`);
    img.src = url.toString();
}
setTimeout(
    () =>
        chrome.tabs.query({}, tabs => {
            for (const t of tabs) probe(t.url);
            probe("https://never.example/");
        }),
    5000
);
