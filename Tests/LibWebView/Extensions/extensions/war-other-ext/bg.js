console.log("WAR other-ext origin " + location.origin);
for (const f of ["allowed.js", "for-extensions.js", "secret.js"]) {
    const s = document.createElement("script");
    s.src = "chrome-extension://dcemjojdbakopijleedjjpbedbgegehd/" + f;
    s.onerror = () => console.log("WAR other-ext " + f + ": refused");
    document.head.append(s);
}
