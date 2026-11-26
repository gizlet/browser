# Gizlet Browser

A single-page browser for Gizlet handhelds. It uses WPE WebKit for web content and the DRM/KMS backend for full-screen display on the R36S.

## Controls

| Button | On the address screen | On a website |
| --- | --- | --- |
| D-pad | Move around the address keyboard | Scroll up or down and move between links with left or right |
| A / B | Select / delete a character | Open a focused link / go back |
| X | Open the address screen | Open the address screen |
| Y | Reload | Reload |
| L / R | Back / forward | Back / forward |
| Hold FN | Return to Launcher | Return to Launcher |

The app follows the Launcher's A/B setting. A physical keyboard can type into the address field and use `Ctrl+L`, `Alt+Left`, `Alt+Right` and `F5`.

## Build and install

On ARM64 Linux, install `mmdebstrap`, `squashfs-tools`, `debian-archive-keyring` and Python 3. Run `sudo ./scripts/package.sh` to build, run the headless smoke test, and write the `.gzl` under `dist/`. The build uses a dated Debian snapshot for WPE 2.54.3 and the application runtime.

Copy the package to the device, then install it with `gizlet install /tmp/io.gizlet.browser-0.1.0-arm64.gzl`. Open Browser in the Launcher.

The package carries its own read-only runtime with WPE, EGL/GBM and fonts. The entry mounts that runtime in a private mount namespace and starts the browser with an unprivileged UID. WebKit uses an ephemeral network session and Debian's bubblewrap sandbox. The address screen is bundled locally, and the shell denies permission requests. The first release has one tab, with media and WebRTC disabled.

The application code is in `src/`, the address screen is in `ui/`, and the ARM64 packaging and launch scripts are in `scripts/`. The headless test opens the address screen in WebKit and navigates between two local HTTP origins. The R36S display, handoff to Launcher and physical controls need device validation.

## Redistribution

The runtime includes Debian package copyright notices under `usr/share/doc`. Provide matching Debian sources from the snapshot in `scripts/package.sh` when distributing `.gzl` builds.
