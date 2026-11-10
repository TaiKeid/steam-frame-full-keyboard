# Chinese character ranks

`chinese/char-ranks.txt`, the order Cangjie and Quick offer the characters of a code in, is derived from two sources. Both are pinned in `tools/build_chinese_ranks.py`, and their hashes are in `chinese/README.md`:

| Source | Used | Licence | File here |
|---|---|---|---|
| McBopomofo's phrase data (`data/data.txt` of [fcitx5-mcbopomofo](https://github.com/openvanilla/fcitx5-mcbopomofo) 3.1.1, commit `a84bdd5c741e1696b270159ebda675946dc73d49`), © The McBopomofo Authors | Taiwan usage: each character's best single-character score | MIT | `mcbopomofo-LICENSE` |
| [rime-cantonese](https://github.com/rime/rime-cantonese)'s `essay-cantonese.txt` and `jyut6ping3.words.dict.yaml`, commit `259f0e48bba840c3a2e0d117539e96937f3d89bc`, by the Cantonese Computational Linguistics Infrastructure Development Workgroup (CanCLID) | Hong Kong usage: counts of single characters, then how many dictionary words hold a character the counts lack | CC BY 4.0 | `rime-cantonese-LICENSE-CC-BY` |

Changes: only single characters are kept, each source is turned into an order, and a character takes the better of its two places. The list holds no text from either source other than the characters themselves. rime-cantonese's `jyut6ping3.maps`, the one part under ODbL, is not used.
