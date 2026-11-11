# Installer

A window that installs, reinstalls or removes Full Keyboard without a terminal, and sets it up on a first install. It is a draft: the screens and the simulated flow are done, while the real install path still needs its first run on the Frame.

The installer carries no package. It lists the stable releases from 0.16.0 on, the first that understands every wizard choice, newest first and the newest chosen. It downloads the chosen one with `update_helper.py --installer-fetch`, which checks it the way the [in-app updater](updates.md#verification-and-installation) does: HTTPS to the project's GitHub releases only, the exact `SHA256SUMS` entry and GitHub's digest, a safe unpack and the package's own checks. The release's `install.sh` then installs it, and `uninstall.sh` removes it.

## Screens

- **Home:** the keyboard on a lighter backdrop, its name, **Install** with a version list beside it, and the chosen version below. With a version installed, it says which, the button reads **Reinstall**, and **Uninstall** sits below. Uninstall asks first, with **Keep my settings** on by default. The list shows the eight newest versions.
- **Setup:** the keyboard slides up and fades while the progress bar fills above a card. A first install shows five pages there while it downloads: Replace the native keyboard, swiping, typing suggestions, Pull for Shift, and Use the numpad. Each has a picture of its feature on the left, playing in a loop, and its question on the right, Yes or No, Yes by default. Answered No, the picture dims; the numpad's shows the keyboard with or without it instead. Choosing doesn't advance; Back and Next do, and pages slide and fade like a carousel, with a dot for each along the bottom.
- **Finish:** lists the five answers and waits, with a spinner on Finish and a ring that fills, until the install is done. On a first install it saves the choices with the keyboard's own settings code, the same code Settings uses. With replacement on, it also sets up Start with SteamOS, starts the service and shows the keyboard. Back works until Finish is pressed. Reinstalls skip the wizard, keep settings as they are, and close on Finish.

A keyboard that is running, also as the Start with SteamOS service, is stopped before installing, as `uninstall.sh` stops it. Steam's keyboard takes over meanwhile, and the service starts again afterwards. The interface follows the system language, as the keyboard does; its text is in the `installer.*` keys of `locales/`. The look comes from the keyboard's own drawing of cards, buttons and wells (`include/framekeyboard/paint.hpp`) in the Graphite theme, and every other colour is mixed from that theme's. The pictures use the keyboard's own picture and keys drawn like its keys; Pull for Shift's moves the legends as the keyboard does. Buttons go down under a press as keys do. The window has no close button of its own: KDE's title bar and Gamescope's app bar close it.

## Simulation

`--simulate` downloads, writes and starts nothing: each task only takes time. Host builds have it in `build/<preset>/framekeyboard-installer`:

```sh
build/simulator/framekeyboard-installer --simulate --releases 0.17.0,0.16.0
build/simulator/framekeyboard-installer --simulate --installed 0.16.0 --fail download
build/simulator/framekeyboard-installer --simulate --language ja --tour /tmp/installer-tour
```

`--installed` pretends a version is installed, `--fail` fails one task (`releases`, `download`, `install`, `uninstall` or `finish`), `--speed` runs tasks faster, and `--tour DIR` saves every screen as PNG and exits: each page's picture at a telling moment, a page mid-turn, and controls with the laser on them and held down. A tour takes about twenty seconds, as the install is slowed to let the pictures play.

The window repaints only when its picture changes: every frame through a transition and while a wizard page's picture plays, thirty times a second for a spinner, and not at all on a screen where nothing moves. The card, the stage and Home's keyboard with its shadow and reflection are drawn once and reused.

## AppImage

`scripts/installer-appimage.sh` builds `out/installer/Full_Keyboard_installer-VERSION-aarch64.AppImage` with a `SHA256SUMS` file, from the `frame-arm64-release` preset and the stable sysroot. It needs `FRAMEKEYBOARD_SYSROOT`, `FRAMEKEYBOARD_APPIMAGETOOL` (appimagetool 1.9.1, as for the [simulator](simulator.md#appimage)) and `FRAMEKEYBOARD_APPIMAGE_RUNTIME`, the aarch64 runtime of [AppImage/type2-runtime](https://github.com/AppImage/type2-runtime) (release 20251108, SHA-256 `00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444`). That runtime needs only FUSE 3, which SteamOS has; older runtimes need `libfuse.so.2`, which it lacks. The image carries our `LICENSE` and the runtime's ([third-party sources](../third_party/README.md#appimage-runtime)).

On the Frame, a downloaded AppImage doesn't open from the browser's download list. Open its folder in Dolphin, which also works in Game Mode, and double-click it. The first time, KDE asks whether to launch it and whether to trust it, and then makes it executable.
