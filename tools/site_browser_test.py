"""中文：在本机浏览器回归 readll 官网，不连接或刷写设备。
English: Exercise the readll site in a local browser without connecting or flashing hardware.

Requires Python Playwright and its Chromium browser. Optional screenshots stay in build/.
"""
from __future__ import annotations

import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread

from playwright.sync_api import sync_playwright


ROOT = Path(__file__).resolve().parents[1]


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, _format: str, *_args: object) -> None:
        pass


def check(site: Path, screenshots: Path | None) -> None:
    server = ThreadingHTTPServer(("127.0.0.1", 0),
                                partial(QuietHandler, directory=str(site)))
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base = f"http://127.0.0.1:{server.server_port}"
    try:
        with sync_playwright() as playwright:
            browser = playwright.chromium.launch()
            page = browser.new_page(viewport={"width": 1440, "height": 1000})
            errors: list[str] = []
            page.on("pageerror", lambda error: errors.append(str(error)))
            response = page.goto(base, wait_until="networkidle")
            assert response and response.ok
            page.wait_for_function(
                "!document.getElementById('bin-download').hidden")
            assert page.locator("h1").count() == 1
            assert page.locator("#firmware-version").inner_text().startswith("0.")
            assert page.locator("#bin-download").get_attribute("download") == "readll-update.bin"
            assert page.locator("#flash-installer").get_attribute("manifest").endswith("/manifest.json")
            assert page.locator("#release-notes-list li").count() == 2
            assert not page.locator("#flash-installer").is_hidden()
            assert page.locator("#flash-help").is_hidden()
            page.locator("#view-reader").click()
            assert page.locator("#preview-image").get_attribute("src").endswith("/reader.png")
            page.locator("#view-reader").press("ArrowDown")
            assert page.locator("#view-home").get_attribute("aria-selected") == "true"
            assert page.locator("#view-home").evaluate("(node) => node === document.activeElement")
            page.locator("#preview-zoom").click()
            assert page.locator("#preview-dialog").is_visible()
            assert page.locator("#preview-full").get_attribute("src").endswith("/home.png")
            page.keyboard.press("Escape")
            assert not page.locator("#preview-dialog").is_visible()
            page.locator("#view-shelf").click()

            for width in (1440, 1024, 768, 720, 390, 320):
                page.set_viewport_size({"width": width, "height": 1000})
                assert page.evaluate(
                    "document.documentElement.scrollWidth <= innerWidth"), f"overflow at {width}px"
                assert page.locator("img").evaluate_all(
                    "(nodes) => nodes.filter(n => n.id !== 'preview-full').every(n => n.complete && n.naturalWidth > 0)"
                ), f"missing image at {width}px"
                if screenshots:
                    screenshots.mkdir(parents=True, exist_ok=True)
                    page.screenshot(path=str(screenshots / f"readll-{width}.png"), full_page=True)

            page.set_viewport_size({"width": 1440, "height": 1000})
            page.locator("[data-ask-manual]").click()
            assert page.locator(".manual-helper").is_visible()
            page.keyboard.press("Escape")
            assert not page.locator(".manual-helper").is_visible()
            links = page.locator('a[href*="manual/#"]').evaluate_all(
                "(nodes) => nodes.map(n => n.getAttribute('href'))")
            manual_response = page.request.get(base + "/manual/")
            assert manual_response.ok
            manual = manual_response.text()
            for link in links:
                assert f'id="{link.split("#")[1]}"' in manual, f"missing manual anchor: {link}"
            assert not errors, errors

            page.route("**/manifest.json", lambda route: route.fulfill(status=503, body="unavailable"))
            page.reload(wait_until="networkidle")
            assert page.locator("#bin-download").is_hidden()
            assert page.locator("#bin-download").get_attribute("href") is None
            assert page.locator("#flash-installer").is_hidden()
            assert "暂不可用" in page.locator("#release-notes-title").inner_text()
            assert not errors, errors

            unsupported = browser.new_context()
            unsupported.add_init_script(
                "Object.defineProperty(Navigator.prototype, 'serial', {value: undefined});"
                "delete Navigator.prototype.serial;")
            other = unsupported.new_page()
            other.goto(base, wait_until="networkidle")
            other.wait_for_function("!document.getElementById('bin-download').hidden")
            assert other.locator("#flash-installer").is_hidden()
            assert "无法连接串口" in other.locator("#flash-help-title").inner_text()
            unsupported.close()
            browser.close()
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
    print("readll browser PASS: responsive layouts, previews, keyboard/dialog, manual, release, unavailable and unsupported-browser states")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, default=ROOT / "flash")
    parser.add_argument("--screenshots", type=Path)
    args = parser.parse_args()
    check(args.site.resolve(), args.screenshots)
