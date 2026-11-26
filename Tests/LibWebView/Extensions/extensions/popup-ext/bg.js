async function probe(tag) {
  console.log("PROBE " + tag + " settings " + JSON.stringify(await chrome.action.getUserSettings()));
}
setTimeout(async () => {
  await probe("before");
  try { await chrome.action.openPopup(); console.log("PROBE openPopup ok"); }
  catch (e) { console.log("PROBE openPopup error " + e.message); }
  try { await chrome.action.openPopup({ windowId: 7 }); console.log("PROBE openPopup(7) ok"); }
  catch (e) { console.log("PROBE openPopup(7) error " + e.message); }
}, 5000);
setInterval(() => probe("tick"), 5000);
