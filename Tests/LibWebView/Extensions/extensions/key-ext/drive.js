const sleep = ms => new Promise(r => setTimeout(r, ms));
(async () => {
    await sleep(3500);
    internals.sendTextThroughUIProcess("yy");
    await sleep(2000);
    internals.sendTextThroughUIProcess("P");
    await sleep(2000);
    console.log("KEYTEST done");
})();
