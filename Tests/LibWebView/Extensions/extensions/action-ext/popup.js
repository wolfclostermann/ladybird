chrome.tabs.query({ active: true, currentWindow: true }).then(([tab]) => {
  document.getElementById("tab").textContent = tab ? `Tab ${tab.id}: ${tab.url}` : "no tab";
  console.log("ACTIONTEST popup tab", tab && tab.id, tab && tab.url);
});
document.getElementById("close").onclick = () => window.close();
