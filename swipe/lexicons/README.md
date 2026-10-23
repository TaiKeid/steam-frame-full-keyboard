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

Changes from the source: bigram and shortcut entries removed; only words of at
least two letters (apostrophes allowed) kept; the 100,000 most frequent kept;
converted to TSV by `tools/build_swipe_lexicon.py`.
