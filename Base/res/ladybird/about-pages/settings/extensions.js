const SHOW_EXTENSIONS_BUTTON = "ui.toolbar.show_extensions_button_without_extensions";

const showExtensionsButtonGroup = document.querySelector("#show-extensions-button-group");
const showExtensionsButton = document.querySelector("#show-extensions-button");

function loadSettings(settings) {
    const variable = settings.configVariableDefinitions?.find(candidate => candidate.name === SHOW_EXTENSIONS_BUTTON);
    if (variable) {
        showExtensionsButton.checked = variable.value;
        showExtensionsButtonGroup.classList.remove("hidden");
    }
}

showExtensionsButton.addEventListener("change", () => {
    ladybird.sendMessage("setConfigVariable", {
        name: SHOW_EXTENSIONS_BUTTON,
        value: showExtensionsButton.checked,
    });
});

document.querySelector("#manage-extensions").addEventListener("click", () => {
    location.href = "about:extensions";
});

document.addEventListener("WebUIMessage", event => {
    const { name, data } = event.detail;
    if (name === "loadSettings") {
        loadSettings(data);
    }
});
