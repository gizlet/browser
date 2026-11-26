# Gizlet Browser

A single-page browser for Gizlet handhelds. It uses WPE WebKit for web content and the DRM/KMS backend for full-screen display on the R36S.

## Controls

The pointer stays visible while browsing. Move it with the left stick or nudge it with the D-pad, then press A to click. Clicking a text field opens the keyboard. The right stick scrolls in all four directions and accelerates as you hold it.

The top progress bar shows when a page loads. It briefly stays visible after fast page changes, including when you go back.

| Button | On a website | With the keyboard open |
| --- | --- | --- |
| D-pad | Nudge the pointer (hold to repeat) | Move between keys |
| A | Click at the pointer | Press the key |
| B | Back | Delete |
| X | Address screen | Symbols |
| Y | Reload | Space |
| L1 / R1 | Page up / down | Move the caret |
| L2 / R2 | Zoom out / in | Shift (L2) |
| Select | Reset zoom | Close the keyboard (back, on the address screen) |
| Start | Forward | Go |
| Left stick | Pointer | |
| Right stick | Scroll | |
| Hold FN | Return to Launcher | Return to Launcher |

The keyboard has a row of `https://`, `www.`, `.com`, `.cn`, `/` and `:`, a symbol layer with the rest of ASCII, and a preview of the field being edited. Text that is not an address searches Bing. The app follows the Launcher's A/B setting and language. A physical keyboard can type directly and use `Ctrl+L`, `Alt+Left`, `Alt+Right` and `F5`.

## Build and install

On ARM64 Linux, install `mmdebstrap`, `squashfs-tools`, `debian-archive-keyring` and Python 3.11 or newer. Run `sudo ./scripts/package.sh` to build, run the headless smoke test, and write the `.gzl` under `dist/`. The build uses a dated Debian snapshot for WPE 2.54.3 and the application runtime. It generates the package's `manifest.json` from [`gizlet.toml`](gizlet.toml), following the [Gizlet format](https://github.com/gizlet/gizlet/blob/main/docs/gizlet-toml.md).

Copy the package to the device, then install it with `gizlet install /tmp/io.gizlet.browser-0.1.0-aarch64-unknown-linux-gnu-lite.gzl`. Open Browser in the Launcher.

The package carries its own read-only runtime with WPE, EGL/GBM and fonts. The entry mounts that runtime in a private mount namespace and starts the browser with an unprivileged UID. WebKit uses an ephemeral network session and Debian's bubblewrap sandbox. The address screen is bundled locally, and the shell denies permission requests. The first release has one tab, with media and WebRTC disabled.

Browser sends an iPhone Safari user agent so sites can serve their mobile layout.

The application code is in `src/`, the address screen is in `ui/`, and the ARM64 packaging and launch scripts are in `scripts/`. The overlay in `ui/overlay.js` runs in an isolated script world and draws the keyboard, pointer and load progress. Pages paint on the CPU (`WEBKIT_SKIA_ENABLE_CPU_RENDERING`), which held frame times steadier than Panfrost on the R36S, and web processes are capped at 420 MB. The headless test checks pointer movement and clicks, right-stick scrolling in four directions, typing into page and frame fields, and navigation across origins. The R36S display, handoff to Launcher and physical controls need device validation.

## Release

Set `version` in `gizlet.toml` and push the matching `v<version>` tag. CI builds
and smoke-tests the ARM64 package, publishes it to the GitHub Release, and
waits for the Gizlet Store checks. Select the tag when dispatching CI to
retry the release.

## Redistribution

The runtime includes Debian package copyright notices under `usr/share/doc`. Provide matching Debian sources from the snapshot in `scripts/package.sh` when distributing `.gzl` builds.
