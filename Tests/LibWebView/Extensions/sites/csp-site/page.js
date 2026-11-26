const report = (who, label, fn) => { let r; try { r = fn(); } catch (e) { r = "THREW " + e.message; } console.log(`CSPTEST ${who} ${label}: ${r}`); };
function probe(who) {
    const d = document.createElement("div"); document.body.append(d);
    report(who, "innerHTML", () => { d.innerHTML = "<b>x</b>"; return d.querySelector("b") ? "ok" : "no element"; });
    const s = document.createElement("style"); s.textContent = `#t-${who} { color: rgb(1, 2, 3); }`; document.head.append(s);
    const t = document.createElement("span"); t.id = `t-${who}`; document.body.append(t);
    report(who, "style element", () => getComputedStyle(t).color === "rgb(1, 2, 3)" ? "applied" : "blocked");
    const a = document.createElement("span"); document.body.append(a); a.setAttribute("style", "color: rgb(4, 5, 6)");
    report(who, "style attribute", () => getComputedStyle(a).color === "rgb(4, 5, 6)" ? "applied" : "blocked");
}
addEventListener("load", () => setTimeout(() => probe("page"), 500));
