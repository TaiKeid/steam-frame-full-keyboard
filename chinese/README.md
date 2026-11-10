# Chinese character ranks

`char-ranks.txt` holds the 17,560 CJK ideographs of Big5-HKSCS, the repertoire Taiwan and Hong Kong input methods offer, one a line, commonest first. Cangjie and Quick offer the characters of a code in this order (docs/languages.md, Chinese and Korean composition): the IBus tables the simulator bundles give nearly every character the same frequency, so without it a code's characters come in table order, rare ones first. A character missing from the list (Simplified-only and Japanese forms, most of Extensions A and B) counts as rare and is offered after the others.

15,893 of them are ranked by usage. After one empty line, the other 1,667 follow by code point: characters neither source uses, which are offered after a code's completions, as rare ones are. A ranked character's place is the better of two:

- **Taiwan usage:** its best single-character score in McBopomofo's phrase data, over all its readings.
- **Hong Kong usage:** its count in rime-cantonese's table of Cantonese word frequencies; characters that table lacks (𨋢, 冧, 冚) follow, by how many words of rime-cantonese's Cantonese dictionary hold them, if at least two do.

Big5-HKSCS is Python's `big5hkscs` codec. Ties go by code point. Sources, licences and attribution are in `third_party/chinese-ranks/`.

Rebuild it from those files with:

```sh
python3 -I tools/build_chinese_ranks.py <fcitx5-mcbopomofo>/data/data.txt <rime-cantonese>/essay-cantonese.txt <rime-cantonese>/jyut6ping3.words.dict.yaml chinese/char-ranks.txt
```

The script checks each source against its pinned hash:

| File | Commit | SHA-256 |
| --- | --- | --- |
| `data/data.txt` (fcitx5-mcbopomofo 3.1.1) | `a84bdd5c741e1696b270159ebda675946dc73d49` | `0deae7b7c1dcde1d7a30d139e7068543e0c0e7112e944b63eb52947bca1db7ac` |
| `essay-cantonese.txt` (rime-cantonese) | `259f0e48bba840c3a2e0d117539e96937f3d89bc` | `d08836175f598219f43c2f2f9e12e12711212dbefe08d57b2eddb7a9d9f22a5d` |
| `jyut6ping3.words.dict.yaml` (rime-cantonese) | `259f0e48bba840c3a2e0d117539e96937f3d89bc` | `54d174ad2bb997e4a678b7b076b84e4dc5914481b32467cdea3b43e88b7d5474` |
| `char-ranks.txt` (output, Python 3.14) | | `5c7455e83b936bb9f033b53a13af9afecdd4be7f3fe8ece6844d3c525289f0bb` |
