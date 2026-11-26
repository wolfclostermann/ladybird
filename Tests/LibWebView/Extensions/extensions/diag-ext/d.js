let href;
try {
    href = document.location.href;
} catch (e) {
    href = "THROWS " + e.message.slice(0, 30);
}
throw new Error(
    `DIAG url=${document.URL.slice(0, 60)} top=${window === window.top} readyState=${document.readyState} origin=${origin} href=${href.slice(0, 60)}`
);
