/* SPDX-License-Identifier: Apache-2.0
 * 中文：readll 原创官网的可访问预览与当前构建说明；不触发串口操作。
 * English: Accessible previews and current-build notes for the original readll site; no serial I/O.
 */
(() => {
  const views = {
    shelf: {asset: "shelf-acrylic", title: "书架", caption: "封面排好，收藏与阅读进度跟着书走。"},
    reader: {asset: "reader", title: "正文", caption: "阅读正文、目录与工具栏，长篇也有自己的节奏。"},
    home: {asset: "home", title: "首页", caption: "上次读到的书和阅读记录，下次打开接着读。"}
  };
  const tabs = [...document.querySelectorAll("[data-view]")];
  const image = document.getElementById("preview-image");
  const dialog = document.getElementById("preview-dialog");
  const zoom = document.getElementById("preview-zoom");
  function select(tab) {
    const view = views[tab.dataset.view];
    for (const item of tabs) {
      item.setAttribute("aria-selected", String(item === tab));
      item.tabIndex = item === tab ? 0 : -1;
    }
    image.src = `./assets/screens/${view.asset}.png`;
    image.alt = `readll ${view.title}界面`;
    zoom.setAttribute("aria-label", `放大${view.title}界面`);
    document.getElementById("preview-panel").setAttribute("aria-labelledby", tab.id);
    document.getElementById("preview-caption").textContent = view.caption;
  }
  for (const tab of tabs) {
    tab.addEventListener("click", () => select(tab));
    tab.addEventListener("keydown", event => {
      const index = tabs.indexOf(tab);
      const target = {
        ArrowDown: (index + 1) % tabs.length, ArrowRight: (index + 1) % tabs.length,
        ArrowUp: (index + tabs.length - 1) % tabs.length,
        ArrowLeft: (index + tabs.length - 1) % tabs.length,
        Home: 0, End: tabs.length - 1
      }[event.key];
      if (target === undefined) return;
      event.preventDefault();
      select(tabs[target]);
      tabs[target].focus();
    });
  }
  zoom.addEventListener("click", () => {
    document.getElementById("preview-full").src = image.src;
    document.getElementById("preview-full").alt = image.alt;
    document.getElementById("preview-dialog-title").textContent = image.alt;
    dialog.showModal();
  });
  document.getElementById("preview-close").addEventListener("click", () => dialog.close());
  dialog.addEventListener("click", event => {
    const box = dialog.getBoundingClientRect();
    if (event.target === dialog && (event.clientX < box.left || event.clientX > box.right ||
        event.clientY < box.top || event.clientY > box.bottom)) dialog.close();
  });

  const title = document.getElementById("release-notes-title");
  const list = document.getElementById("release-notes-list");
  function notes(lines) {
    list.replaceChildren(...lines.map(textContent =>
      Object.assign(document.createElement("li"), {textContent})));
  }
  document.addEventListener("readll:release-loading", () => {
    title.textContent = "构建说明";
    notes(["正在读取构建信息…"]);
  });
  document.addEventListener("readll:release-unavailable", () => {
    title.textContent = "构建暂不可用";
    notes(["固件信息未能加载。请刷新页面；加载成功前不会启用安装或下载。"]);
  });
  document.addEventListener("readll:release", event => {
    const {manifest} = event.detail;
    title.textContent = `${manifest.version} / 构建说明`;
    notes([
      "WebUSB 完整安装与 TF 应用升级来自同一次构建；下载后可比对 SHA-256。",
      "设备内联网 OTA 尚未配置。网页安装与 TF 本地升级不受影响。"
    ]);
  });
})();
