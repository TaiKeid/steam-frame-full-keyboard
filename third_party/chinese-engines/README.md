# Chinese conversion engines

Licences of the engines that `tools/build-chinese.sh` builds for Chinese
conversion, and of everything built into them. The sources are not vendored.
`tools/fetch-chinese.sh` fetches them at the revisions pinned in
`tools/chinese-pins.sh`.

| Component | Pin | Built as | Licence | File here |
|---|---|---|---|---|
| libime | 1.1.17 | shared libraries | LGPL-2.1-or-later | `LGPL-2.1-or-later.txt` |
| libime dictionary and language model (`dict-20260907`, `lm_sc.arpa-20260629`) | SHA-256 in libime's `data/CMakeLists.txt` | data files | LGPL-2.1-or-later | `LGPL-2.1-or-later.txt` |
| KenLM, inside libime | libime's submodule | part of libime | LGPL-2.1-or-later, BSD-3-Clause for double-conversion | `kenlm-LICENSE`, `kenlm-double-conversion-LICENSE` |
| fcitx5 utility library (`Fcitx5Utils` only) | 5.1.23 | shared library | LGPL-2.1-or-later | `LGPL-2.1-or-later.txt` |
| McBopomofo engine and data (fcitx5-mcbopomofo `src/Engine`, `data/`) | 3.1.1 | static libraries, data files | MIT | `mcbopomofo-LICENSE` |
| OpenCC and its dictionaries | 1.4.2 | static library, data files | Apache-2.0 | `opencc-LICENSE` |
| marisa-trie, darts-clone, RapidJSON, inside OpenCC | OpenCC's `deps/` | part of OpenCC | BSD-2-Clause (marisa also LGPL-2.1-or-later), BSD-2-Clause, MIT | `marisa-COPYING.md`, `darts-clone-COPYING.md`, `rapidjson-license.txt` |
| zstd | 1.5.7 | static, inside libime | BSD-3-Clause (or GPL-2.0) | `zstd-LICENSE` |
| Boost (headers and Iostreams) | 1.92.0 | static, inside libime | BSL-1.0 | `boost-LICENSE_1_0.txt` |

The LGPL components stay shared libraries, so they can be replaced with a
rebuilt copy. Only the build uses KDE's extra-cmake-modules, so nothing from
it is shipped.
