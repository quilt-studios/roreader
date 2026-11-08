<p align="center">
  <img src="assets/roreader-logo.png" alt="ROREADER" width="420">
</p>

# ROREADER

**An EPUB reader with built-in tools, a sketchpad and offline games for Kobo and compatible tolino devices.**

ROREADER is based on Kobalt and uses its Qt interface and CREngine renderer. Read DRM-free EPUBs, draw, edit text files and run shell commands directly on your reader. Protected books, PDFs and other unsupported formats continue to open in the device's built-in reader.

## Languages

Reader menus, tools, games and the website installer support **English, German, French, Spanish, Italian, Portuguese, Dutch, Polish, Czech, Slovak, Hungarian, Romanian, Turkish, Ukrainian, Russian, Swedish, Danish, Norwegian, Finnish, Indonesian and Kurdish (Kurmancî)**.

On the reader, open **Settings → System → Language**, apply your settings and reopen the book. The device language is used unless you choose another language; unsupported languages fall back to English. The website's language selector follows your browser language and remembers your choice locally. Switching languages reloads the page, so select your reader's folder again afterwards. Language changes are disabled during installation.

Language catalogs are included locally. Reading and installation do not use an online translation service. Translations are machine-generated drafts and still need native-speaker review. Technical system messages, filenames, book content and license notices keep their original language. See [Localization](doc/localization.md) for details and validation commands.

## Tools on your reader

Tap the **tools icon** in the reading menu, or open **Settings → System → Terminal · Sketchpad · Text editor**. The day/night shortcut in the reading menu switches themes directly.

| Tool | Features |
| --- | --- |
| Terminal | Command history, a working directory that follows `cd`, input to running programs, UTF-8 output and text export. Commands have a 60-second limit and bounded output. Full-screen applications such as `vi` require a PTY and are not supported. |
| Sketchpad | Smoothed strokes, adjustable pen width, black/gray ink, eraser, undo and redo. Handles touch and reported pen pressure. Open PNGs and export drawings. Small repaint regions are batched; actual latency depends on the device and E-Ink driver. |
| Text editor | Open and atomically save UTF-8 files, including a BOM; search, on-screen keyboard, new lines and undo. Files are limited to 1 MiB. Externally changed files and existing files that have not been opened are not silently overwritten. |
| Files | Open, rename, calculate SHA-256 checksums or delete files after confirmation in the ROREADER documents folder. PNGs open in Sketchpad; text files open in the editor. |
| Device / Mods | Firmware, Qt, system, battery and storage information, diagnostic export, local diagnostic commands and ROREADER document backups. Does not automatically patch firmware. |
| Games | Tic-Tac-Toe against the reader or another player, Sudoku with hints and conflict checking, and Memory. Games work offline; their state stays in memory while the tools are open. |

Text files and drawings are stored in **`/mnt/onboard/ROREADER/`**, the **`ROREADER`** folder on the reader's USB drive. On Linux desktop they use `Documents/ROREADER`. Unsaved drafts survive switching between tools; closing the tools asks whether to discard them. Save files to keep them permanently. Shell commands run with the reader process's permissions, so review commands before running them.

**Back up documents** creates a new `backup-…` folder containing copies and a SHA-256 manifest. This covers files in the ROREADER documents folder, not the reading database, other books or firmware. The file manager lists files in that documents folder; backup subfolders can be opened over USB. Command history and game state are not automatically written to the SD card.

## Install online

The GitHub Pages site uses the same installer code as the Windows bundle. Open it on your computer in a current version of **Chrome or Edge**, connect your reader over USB and choose its root folder using **Select reader drive**. No BAT file or additional application is required. The supplied Qt6 package is downloaded and verified automatically; device and book data are not uploaded.

The [ROREADER website](.github/workflows/pages.yml) workflow publishes the site through GitHub Pages. Enable **Settings → Pages → Source → GitHub Actions** once, then run the workflow. Its successful Pages deployment provides the published URL.

## Windows: install using Chrome

1. Download [ROREADER-Windows-Installer.zip](ziprelease/ROREADER-Windows-Installer.zip) and extract it completely on your Windows PC. Its [SHA-256 checksum](ziprelease/ROREADER-Windows-Installer.zip.sha256) is provided alongside it.
2. Connect the reader over USB and confirm the connection on the device. Back up your books and reading data.
3. Run **`Install-ROREADER.bat`**. It opens the local installer in **Google Chrome**. Chrome must be installed; Python and administrator privileges are not required.
4. Click **Select reader drive** and choose the device's **root directory `/`**, for example `E:\`. Do not select `.kobo` or `.adds` as the starting folder.
5. Allow Chrome to access the folder. The bundled Qt6 package and checksum load automatically. Check the detected firmware and confirm that you have backed up your data.
6. Click **Copy ROREADER to the reader**. After the copy has been verified, safely eject the drive, disconnect USB and wait for installation on the reader.

Keep the console window open while using the installer: it runs the local file server, which stops when the window closes. Files are written directly through Chrome's folder permission. No upload or sign-in is required. Existing ROREADER/Kobalt installations are detected: **Update ROREADER** replaces program files and stages the new device package. Books, reading data, settings and personal documents are preserved. Any update archive already on the device must be processed on the reader first.

When starting the BAT from the source checkout, you can select your own built **`ROREADER-qt6.zip`** and the checksum from **`ROREADER-qt6.zip.sha256`**. The interface requires a real device package and does not substitute another project's release.

## Firmware and models

The USB installer targets **Kobo and tolino devices running Kobo-based firmware 5.x or 6.x**, using the Qt6 package. It detects the firmware family from `.kobo/version`; the application retains its checks for required firmware symbols and object layouts.

**Support for every model and firmware version has not been verified on physical devices.** Detecting a version number does not confirm device compatibility or successful installation. Older Android-based tolino devices are outside this USB installation path. Any required mod manager must also support the installed firmware. Firmware ABI changes must not bypass compatibility checks.

The existing Qt5 and Android build paths remain available in the source. The Windows installer accepts only the regular Qt6 device package, not Qt5 or evaluation packages.

## Build and test

See [Building](doc/building.md) for prerequisites and platform commands. Linux development uses a prepared **Qt 6.11+ SDK** with Core, Gui, Widgets, Network and Sql. Tests additionally require Test, matching GuiPrivate headers and the Minimal platform plugin. ROREADER does not build Qt as part of its application build.

```sh
cd platforms/desktop
cmake --preset linux -DCMAKE_PREFIX_PATH=/path/to/qt-sdk
cmake --build --preset linux
cmake --build --preset linux-light-tests
ctest --preset linux-light
```

For compatibility, the internal desktop build target and executable are still named `Kobalt`; the interface and device packages are named ROREADER. Existing `.adds/kobalt` paths, profiles, databases and plugin identities are preserved so that reading data remains usable. Source namespaces and CMake options starting with `KOBALT_` do not identify a separate application.

The firmware-5/6 package requires the pinned, prepared `kobuild` Qt6 image:

```sh
docker pull "$(sh platforms/sdk-image.sh qt6)"
bash platforms/kobo/build.sh qt6
cmake -DROOT="$PWD" \
  -DPACKAGE_FILE="$PWD/build/kobo-qt6/ROREADER-qt6.zip" \
  -P platforms/installer/package.cmake
```

The device package is written to `build/kobo-qt6/`. The Windows bundle, including the BAT, web interface, device package and checksum, is **`build/installer/ROREADER-Windows-Installer.zip`**. A successful build and archive check do not replace installation testing on a real device.

Run the web installer's ZIP and firmware checks with Node.js:

```sh
node --test tests/installer.test.mjs
```

The native light suite includes functional checks for the device tools. Generate headless screenshots with `linux-light-screenshots`; [Native validation](doc/validation.md) describes the complete gallery and other test groups. Run `./start.sh` to open the developer launcher, or `./start.sh --check` to check its prerequisites without opening a window.

## Origin and license

ROREADER is a fork of Kobalt, not an official Rakuten Kobo or tolino product. Original copyright notices and [Acknowledgments](doc/acknowledgements.md) are preserved. CREngine comes from the [KOReader](https://koreader.rocks/) ecosystem. The new logo artwork was generated for ROREADER.

The application code uses the [MIT license](LICENSE). Third-party libraries and combined packages remain subject to their respective licenses, including GPL and AGPL. The combined package is distributed under GNU AGPL v3 without warranty. Included license texts are available in `assets/notices/`.
