(async () => {
  try {
    const tree = await chrome.bookmarks.getTree();
    console.log("BMTEST isArray=" + Array.isArray(tree) + " " + JSON.stringify(tree).slice(0, 400));
    const flat = [];
    const walk = n => { if (n.url) flat.push(n.title + "=" + n.url); (n.children || []).forEach(walk); };
    tree.forEach(walk);
    console.log("BMTEST count=" + flat.length);
    const found = await chrome.bookmarks.search("lady");
    console.log("BMTEST search=" + found.length);
  } catch (e) { console.log("BMTEST error " + e); }
})();
