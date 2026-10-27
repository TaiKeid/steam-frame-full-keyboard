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

All lists except `zh-pinyin.tsv` are also blended with film-subtitle word
frequencies, so everyday chat words that news text rarely uses ("dziękuję",
"okay", "спасибо") rank as they are spoken. The subtitle lists come from
FrequencyWords, https://github.com/hermitdave/FrequencyWords, commit
`525f9b560de45753a5ea01069454e72e9aa541c6`, built from OpenSubtitles 2018
(https://opus.nlpl.eu/OpenSubtitles2018.php), whose content is licensed under
CC BY-SA 4.0, https://creativecommons.org/licenses/by-sa/4.0/. **The blended
lists are therefore licensed under CC BY-SA 4.0**, as adaptations of both
sources; each file's header names its sources.

| Subtitle list (`content/2018/…`) | Used for | SHA-256 |
| --- | --- | --- |
| `en/en_50k.txt` | `en-us.tsv`, `en-gb.tsv` | `5351ff405b1126ef555791dd4d9798a48e3e9a501a9fc481a9da957752cfb458` |
| `de/de_50k.txt` | `de-de.tsv` | `d9e50546fd7e8b6fe6542a2b33c51d1331092b2a3916ec09f80d97856068705b` |
| `es/es_50k.txt` | `es-es.tsv` | `dcff3ad4316192f4dc4ff7d26e637c6ff314ef1ca0f3f720c5649018a71056c0` |
| `fr/fr_50k.txt` | `fr-fr.tsv` | `f81f7c570b6433764da99aa30f4dfb08d81c5926301377b709af23a13b1f9596` |
| `it/it_50k.txt` | `it-it.tsv` | `bb96cdcb56d28342c1e909db6b2525448b7767136b7e86a2ccc649a1be66fc19` |
| `pt_br/pt_br_50k.txt` | `pt-br.tsv` | `a61d6f2ede97c5daad5fb3907b72a228f0aff12668be6d24da590d20804fa611` |
| `ru/ru_50k.txt` | `ru-ru.tsv` | `6095f507cc167488ec66ada5a85ac50433503a08ad24a07c6eabdf54352c4e7f` |
| `uk/uk_50k.txt` | `uk-ua.tsv` | `881686fe66a168b79f6a9ee2f6cbbf944cfb12bfb7040913ef535cb29bd86ec9` |
| `pl/pl_50k.txt` | `pl-pl.tsv` | `50bf8d30d1ed4f5cedceb3e40e74d793ec6bec0f16d9f3dd0a2c9e313402e10f` |
| `nl/nl_50k.txt` | `nl-nl.tsv` | `099bd0c27b514d54284360ad9ce2b4ef7b6903b22e848b67ab10f6aa2adb4baa` |
| `sv/sv_50k.txt` | `sv-se.tsv` | `566d0614a54f3c243bca4097c9bb824d46275abff31582157e73c166b7089500` |

Blending (`--format tsv --blend`): subtitle counts are put on the same 0–255
log scale, shifted by the median difference on the 5,000 most frequent subtitle
words both lists share; a listed word rises to its subtitle frequency when that
is higher and keeps its spelling (German "Zeit"); a subtitle word not listed is
added in lowercase; the 100,000 most frequent are kept.

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
| `pl-pl.tsv` | `wordlists/main_pl__experimental.combined` | `001e472839633f93380cac656149b5d395c68b0d6c45313caef2ce7019124499` |
| `nl-nl.tsv` | `wordlists/main_nl__experimental.combined` | `ff3200da0b873cb70b84a15b34c5af522f72803905f3af2c9a1c27837d977068` |
| `sv-se.tsv` | `wordlists/main_sv__experimental.combined` | `b690f71aeaf6b1edb277af30ffa3e99c723e59c6c16fa894416592e8c38d2772` |

Changes from the source: bigram and shortcut entries removed; only words of at
least two letters (apostrophes allowed) kept; the 100,000 most frequent kept;
converted to TSV by `tools/build_swipe_lexicon.py`.

For `pl-pl.tsv`, `nl-nl.tsv` and `sv-se.tsv`, a capitalised word is also folded
into its lowercase form when both exist and the capitalised one is at most 20
steps (about 4.6 times) more frequent, so sentence-initial capitals such as
Polish "Po" do not replace "po"; names such as "Warszawa" stay capitalised
(`--fold-initial-capitals`).

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
