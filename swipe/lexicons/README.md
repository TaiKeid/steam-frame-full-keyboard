# Swipe word lists

Each `<id>.tsv` holds up to 100,000 words as `word<TAB>log-frequency`
(0–255), most frequent first. A language profile selects one with
`"swipe": {"lexicon": "<id>"}`. A user file with the same name in
`~/.config/framekeyboard/swipe/lexicons/` overrides the bundled one.

The bundled lists are derived from HeliBoard's `aosp-dictionaries` repository,
https://codeberg.org/Helium314/aosp-dictionaries, commit
`ff0cd52be8d9530200fefe8b296c53d4f662091e`, which builds them from the Leipzig
Corpora Collection (https://wortschatz.uni-leipzig.de/en/download/). The source
lists are licensed under CC BY 4.0,
https://creativecommons.org/licenses/by/4.0/.

| File | Source list | Source SHA-256 |
| --- | --- | --- |
| `en-us.tsv` | `wordlists/main_en_US__experimental.combined` | `466c2eb4737fa1a9453e5f4a33337db8dd703f75012b70c2534490fd189c9487` |
| `de-de.tsv` | `wordlists/main_de.combined` | `873f0c90e104c574609d8da6a938e4a7c951d53e20cddc1b66301d4d37752b6d` |
| `es-es.tsv` | `wordlists/main_es__experimental.combined` | `786717cf9ca7a69f6b0bf297f8dbdc24a71e21820462f014e11aa2c8a62d3a97` |
| `en-gb.tsv` | `wordlists/main_en_GB__experimental.combined` | `2c724db60d54a67f25d401b5b5ac264484ded1be4338ea2ab2ed10434f5abb30` |
| `fr-fr.tsv` | `wordlists/main_fr__experimental.combined` | `296d5ae8b1b697092e941af284de454695db1f949c589af8b7ffa71936e3d004` |
| `it-it.tsv` | `wordlists/main_it__experimental.combined` | `50e0c819848e5d7b527dfcb0d21ca85fef3e90d99bd0f5f1d2435cacfa48301d` |
| `uk-ua.tsv` | `wordlists/main_uk__experimental.combined` | `300b928f4ffff6f1d0c5ac74084a5c4d64b63ef194dd20e9bc93a57af6b178df` |
| `ru-ru.tsv` | `wordlists/main_ru__experimental.combined` | `cc95ccd487c8f169924c4f979c0caa5a12ad4a79aa63e967c62359f4bd2dc3c4` |

Changes from the source: bigram and shortcut entries removed; only words of at
least two letters (apostrophes allowed) kept; the 100,000 most frequent kept;
converted to TSV by `tools/build_swipe_lexicon.py`.

`pt-br.tsv` is built directly from the Leipzig Corpora Collection corpus
`por-br_newscrawl_2011_1M`
(https://downloads.wortschatz-leipzig.de/corpora/por-br_newscrawl_2011_1M.tar.gz),
whose downloadable corpora are offered under CC BY
(https://wortschatz.uni-leipzig.de/en/usage). Its words file (SHA-256
`05e9e8c403088b585a33ac55bf36e58332cc4a9e0a18e62d867b3f48f0cf7109`) was reduced
to letter words seen at least three times, case variants merged under their most
frequent spelling, counts converted to the 0–255 log scale, and the 100,000 most
frequent kept (`--format leipzig`).

`zh-pinyin.tsv`, used by both Chinese Pinyin profiles, holds toneless Pinyin
(`v` for ü, apostrophes before vowel-initial syllables) rather than characters:
the composer then offers characters. It is derived from jieba's `dict.txt`
(https://github.com/fxsjy/jieba commit `67fa2e36e72f69d9134b8a1037b83fbb070b9775`,
SHA-256 `7197c3211ddd98962b036cdf40324d1ea2bfaa12bd028e68faa70111a88e12a8`,
MIT, `LICENSE.jieba`) with readings from pypinyin 0.55.0 (MIT,
`LICENSE.pypinyin`). Words of one to four characters were converted, counts of
identical Pinyin summed, and the 100,000 most frequent kept
(`--format jieba-pinyin`).
