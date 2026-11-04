# IBus emoji dictionary (simulator only)

The keyboard reads the system's IBus Japanese emoji dictionary,
`/usr/share/ibus/dicts/emoji-ja.dict`, through the system's libibus
(`libibus-1.0.so.5`); SteamOS ships both, and nothing of IBus is bundled
with the keyboard.

The desktop simulator's AppImage carries them, unchanged, from Arch Linux's
`libibus` 1.5.32-1 and `ibus` 1.5.32-1 packages, so emoji candidates work on
desktops without IBus. libibus is licensed under the GNU Lesser General Public
License, version 2.1 or later (`COPYING`); its source is that of the named
package version, https://github.com/ibus/ibus/releases/tag/1.5.32. The
dictionary is generated from the Unicode CLDR emoji annotations, under the
Unicode License (see `third_party/unicode`).
