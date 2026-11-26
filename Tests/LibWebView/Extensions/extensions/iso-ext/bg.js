chrome.runtime.onMessage.addListener((m, s, reply) => {
    reply("bg-got:" + m);
});
(async () => {
    const r = await fetch("secret.txt")
        .then(r => r.text())
        .catch(e => "ERR " + e);
    console.log("ISOTEST bg reads secret: " + r.trim());
    const f = document.createElement("iframe");
    f.src = "http://127.0.0.1:8781/iso-site/frame.html?id=" + chrome.runtime.id;
    document.documentElement.appendChild(f);
})();
