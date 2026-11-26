(async () => {
  const secret = await fetch("secret.txt").then(r => r.text()).catch(e => "ERR " + e);
  await chrome.storage.local.set({ k: 42 });
  const got = await chrome.storage.local.get("k");
  const reply = await chrome.runtime.sendMessage("hi").catch(e => "ERR " + e.message);
  const tab = await chrome.tabs.getCurrent().catch(e => "ERR " + e.message);
  console.log(`ISOTEST ext page in tab: secret=${secret.trim()} storage=${got.k} reply=${reply} tab=${tab?.id}`);
})();
