# Anthy in the simulator's AppImage

The keyboard itself loads the system's Anthy at run time and bundles none. Only the desktop simulator's AppImage (`scripts/simulator-appimage.sh`, [simulator](../../docs/simulator.md#appimage)) can carry a copy, so that Japanese conversion works without installing anything. No Anthy source is kept here.

What it carries is Arch Linux's `anthy 9100h-7` package for x86-64, unchanged: `libanthy.so.0`, `libanthydic.so.0` and the dictionary `anthy.dic`.

- Package: `anthy-9100h-7-x86_64.pkg.tar.zst`, SHA-256 `72bd530708e5a7e799d442a24d6a998dac91723be19ccf8745cf2b175728b3ec`, signed by its Arch packager.
- Source: `anthy-9100h.tar.gz`, SHA-512 `315ebd9e1af208f3ecaeaa13620213b35f004e47edb54e60c3e02c2997444cd2326cc58b725dd2d0bd6a723130884d946ea193ea3d92418082256b59ecc8d88b` (SHA-256 `d256f075f018b4a3cb0d165ed6151fda4ba7db1621727e0eb54569b6e2275547`). It was published on the now-closed OSDN; Arch keeps it with the package's build script in `https://sources.archlinux.org/sources/packages/anthy-9100h-7.src.tar.gz` (SHA-256 `d7b53a8b729b369527ad21057138ec461151895f6917d1ec2f5985bb6673a30e`).

Licenses, as Anthy's `COPYING` states: the library is under the LGPL 2.1 (`COPYING`, which begins with that statement). The dictionary's sources, `alt-cannadic/*` and `mkworddic/*.t` apart from `base.t` and `zipcode.t`, are under the GPL 2 (`COPYING.dictionary`, from `alt-cannadic/COPYING`), so the built `anthy.dic` is too. Anyone given the AppImage must also be able to get that source, for example by publishing the source archive above beside it.
