# IBus Cangjie and Quick tables (simulator only)

Cangjie and Quick read the system's IBus tables,
`/usr/share/ibus-table/tables/cangjie5.db` and `quick5.db`; SteamOS ships
them, and none of ibus-table-chinese is bundled with the keyboard.

The desktop simulator's AppImage carries those two tables, unchanged, from
Arch Linux's `ibus-table-chinese` 1.8.14-1 package, so Cangjie and Quick work
on desktops without them. They are licensed under the GNU General Public
License, version 3 (`COPYING`); their source is that of the named package
version, https://github.com/mike-fabian/ibus-table-chinese/releases/tag/1.8.14.
