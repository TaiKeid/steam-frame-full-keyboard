# Swipe word lists

Each `<id>.tsv` holds up to 100,000 words as `word<TAB>log-frequency`
(0–255), most frequent first; the lists added from Czech on (2026-11-08) hold
up to 50,000, about 0.6 MB each. A language profile selects one with
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
| `cs/cs_50k.txt` | `cs-cz.tsv` | `1fb0576ef07cf5a543789d394cde1168573fc9e7adf427fd143249fd5547458d` |
| `sk/sk_50k.txt` | `sk-sk.tsv` | `10eddbcd086ff26a814a140e1d5b20a1ba0ee8b0870be1a9a09268ba7aeaa094` |
| `da/da_50k.txt` | `da-dk.tsv` | `6a83e4deb5e38d873b1ccfb4be4fb25903878c07c8522af708027ac8b540c6b1` |
| `no/no_50k.txt` | `nb-no.tsv` | `d5808d8ec04603b765c09c06a03f0273ccf36bb35a3902287e56b7e7d872ca92` |
| `fi/fi_50k.txt` | `fi-fi.tsv` | `8bc1d43dd54ea0c4432b1f6ae497759a332e984224819cf8a4f2337681c803b4` |
| `et/et_50k.txt` | `et-ee.tsv` | `ede602b6d0569606692e91bdf8fd5fe66cf281547d21640d92b2ed2ef73c4270` |
| `lv/lv_50k.txt` | `lv-lv.tsv` | `22ed7023c0d49d1a79a1ff81ccaec8aeca224cb75f49b43bfb8f89b162f93ad0` |
| `lt/lt_50k.txt` | `lt-lt.tsv` | `e953d15442ceb0a2eefd2910ca1e01e1bbf5cb261dc8ac905c918c8ae3edfe31` |
| `hu/hu_50k.txt` | `hu-hu.tsv` | `b765780a1277c3b1e326205c972b1ea1631732ec54ab489d13d21618ceab4fec` |
| `ro/ro_50k.txt` | `ro-ro.tsv` | `af4d7d1fc980ee7f988a5114dd34478dd5c7abd1e66a45c25a5da330f06faba5` |
| `hr/hr_50k.txt` | `hr-hr.tsv` | `725f57e0bd122f46d8e358c3542a4e1d126e468075c5e4d6644fd0d21c47bbc3` |
| `sr/sr_50k.txt` | `sr-rs.tsv` | `c84eaf5e324fe5deb0aa491935bff5e9338b6cd687f6f7aacc56988594a39859` |
| `sl/sl_50k.txt` | `sl-si.tsv` | `00d9dc7a50726e99ab4d5bb50ce8087ce709a1416afad65a9dec0821c03ba442` |
| `bg/bg_50k.txt` | `bg-bg.tsv` | `b0faf13173216e0006ba548d57a621ae1df0fb3e888738df1b9ec09d210903c9` |
| `mk/mk_50k.txt` | `mk-mk.tsv` | `85febe3efafed0d6e3b99287a1268815a938ff9fba3b940c6e9605b393a0f760` |
| `el/el_50k.txt` | `el-gr.tsv` | `7ec8f6de52c38f36a04add5d1613b116aed576e10c5e3b40d0001e8572de978c` |
| `tr/tr_50k.txt` | `tr-tr.tsv` | `480192844e7fdfe9591bbf0b9cfbb96ed5751cfbba7024fd45d8cb7508e07d5c` |
| `ca/ca_50k.txt` | `ca-es.tsv` | `3140bdce43ae1cd40b2dfa2ec1291f4917f477ff7d0573e4b77c6f1bc1d3ffad` |
| `is/is_50k.txt` | `is-is.tsv` | `8c47a7795970edb55178a84d16a568422186c2299024b12a2fc4a0df85ef4e27` |
| `id/id_50k.txt` | `id-id.tsv` | `f2eea0da9735b7040efbfa813f5f875ec931de5a0ec70a6f28213ea60a87bb38` |
| `vi/vi_50k.txt` | `vi-vn.tsv` | `3f5b112e4324136c2ba82d3151038607e3dd1dfa0911c226929d7544cc11adf3` |
| `ar/ar_50k.txt` | `ar.tsv` | `bbe98b4b92902b392bdefa2e555a108fdb42a5dd79d261674be5ab666229e19f` |
| `he/he_full.txt` | `he-il.tsv` (its only source) | `a69a3f390eb53183bf191c4eac18282592992aef9ff184c5dcf8919f5ea0f773` |

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
| `cs-cz.tsv` | `wordlists/main_cs__experimental.combined` | `bc025923025137e2088407561c2cdf83c0c566888478fb843f471bbc27fb317a` |
| `da-dk.tsv` | `wordlists/main_da__experimental.combined` | `76b0283468767e4409b761c23e5c13ce7f9fccbdb226456c0df97735316c4074` |
| `nb-no.tsv` | `wordlists/main_nb__experimental.combined` | `b09123cb85c1b46d73d7297f577704d61bddf0198b8858e85937a839fe2420f4` |
| `fi-fi.tsv` | `wordlists/main_fi__experimental.combined` | `2b2b0ac15511b3cc306a34ac6747a82d5baf2c728c47a80b6abf64bfce347078` |
| `et-ee.tsv` | `wordlists/main_et__experimental.combined` | `10b9b8039c8f83de50ea1b26af6de21c3c0a7850e44532e68c061d3251e9c9e5` |
| `lv-lv.tsv` | `wordlists/main_lv__experimental.combined` | `7b5855db19bd1eec317199bcc8dd1c18c23aa598c9c65930a75715b3ae21a6ec` |
| `lt-lt.tsv` | `wordlists/main_lt__experimental.combined` | `790684b44b0b7110f58ce4070624a9ca780b60b94103f3524c1a459466326f2f` |
| `ro-ro.tsv` | `wordlists/main_ro__experimental.combined` | `31a537e9b4ad411fa8c0c6d157b169ed81a3d349baa729f80b086b7873203c95` |
| `hr-hr.tsv` | `wordlists/main_hr__experimental.combined` | `9d70676b1675fde23794fcd55ea021e6d8367d0e13bbc7c5023da596a4a0c2e2` |
| `sl-si.tsv` | `wordlists/main_sl__experimental.combined` | `5c0f6d36e8436b10c9bd511d4b0ed6ab40b028a89c6032e59e726382e1aeded4` |
| `tr-tr.tsv` | `wordlists/main_tr__experimental.combined` | `d0bd81827eb48b878aadda1d2aa61c08e263ecd7faae6fcf7aaf6a2c774bd272` |
| `is-is.tsv` | `wordlists/main_is__experimental.combined` | `58c3778547d48fb564ddf2cb98cd5b6569a66fd1eb20155aba2ea182cf93ee93` |
| `id-id.tsv` | `wordlists/main_id__experimental.combined` | `392c5302a5e536cc78e78896cd82ad391b0116d4d8aa5bcb42102cedacb32d6e` |
| `vi-vn.tsv` | `wordlists/main_vi__experimental.combined` | `7fda1a6068a5756ec228db0603db3f5b0b5daff111dace223d97e0cc5135ca08` |
| `fil-ph.tsv` | `wordlists/main_tl__experimental.combined` | `78239349374d24a7214e26ec08b3a937abbec7ecb25842b597d9f403c67a0675` |
| `az-az.tsv` | `wordlists/main_az.combined` | `38ce388162db65d7f7ff751f9deda98377cc768319ea5e9a34f9f620645d91a3` |
| `ar.tsv` | `wordlists/main_ar__experimental.combined` | `d5fd8e726331fccfc9bb3818b2414e048760bd4ae41fc01c57afe9df11abaf07` |

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

The lists added on 2026-11-08 are built the same way, with these differences:

- Bulgarian, Catalan, Hungarian and Slovak come straight from a Leipzig corpus
  (`--format leipzig`, like `pt-br.tsv`): their HeliBoard lists give the same
  top frequency to their 1,200 to 18,000 commonest words, which loses their
  order. Greek, Macedonian and Serbian come from Leipzig as well, as their
  HeliBoard lists are GPL-3.0 or carry no licence. Each corpus is at
  `https://downloads.wortschatz-leipzig.de/corpora/<corpus>.tar.gz`, offered
  under CC BY (https://wortschatz.uni-leipzig.de/en/usage):

| File | Corpus | Words file SHA-256 |
| --- | --- | --- |
| `bg-bg.tsv` | `bul_news_2022_1M` | `fdeb6e3497da5f7ab3e4c4eea11453d053f6469ebb664c4651e54fb755d2bd2b` |
| `ca-es.tsv` | `cat_newscrawl_2016_1M` | `d30f1a0cc962274d5620fd1b4d23fbc49a86c1075bc6f10f33c3cfa847e0bae5` |
| `hu-hu.tsv` | `hun_news_2024_1M` | `cecc5bddb96bc45c614589913b6cdc96668884e2023f9346136a9f41f0c9fbc5` |
| `sk-sk.tsv` | `slk_newscrawl_2016_1M` | `723fc680889a888e5b440d7dfa5bb98c017c74309d4425d439e10a414a933aa8` |
| `el-gr.tsv` | `ell_news_2022_1M` | `11b305631de69f4f32413594f0f48bc0aa505bcfcff93559aa460f7c16fafdae` |
| `mk-mk.tsv` | `mkd_newscrawl_2011_300K` | `8c601e90cf64121fda6929fb636a5973138d4e46880c99e22560e2c86b475de7` |
| `sr-rs.tsv` | `srp_wikipedia_2021_1M` | `4ec28767adae0aacef66ef5c493307b632e11b8433413de602112c4a67989916` |

- All are reduced to 50,000 words, and sentence-initial capitals are folded
  (`--fold-initial-capitals`). Turkish and Azerbaijani fold I to ı and İ to i
  (`--turkic-case`).
- Bulgarian, Macedonian and Greek keep only words in their own script
  (`--script`). Serbian keeps its Cyrillic words and converts the subtitle
  list, mostly in Latin, to Cyrillic (`--serbian-cyrillic`); Serbian Latin
  respells the same list through its profile's `replace`.
- Romanian ş and ţ with a cedilla become ș and ț with a comma, as Romanian
  keyboards type them (`--respell`).
- Filipino and Azerbaijani have no subtitle list. `fil-ph.tsv` comes from the
  Tagalog list and holds 9,863 words; it stays under CC BY 4.0.
  `az-az.tsv` comes from `main_az.combined`, built by
  https://github.com/subhangadirli/aosp-dict-az from Leipzig lists and
  licensed under CC BY-SA 4.0, which this list keeps.

Arabic and Hebrew followed the same day:

- `ar.tsv` is built like the others and keeps only Arabic letters
  (`--script arabic`), which drops Latin words, words written with harakat and
  words stretched with tatweel. Both sources are Modern Standard Arabic, so
  dialect chat words rank low or are missing.
- `he-il.tsv` comes from the FrequencyWords full Hebrew list alone
  (`--format tsv` with an empty list and `--blend`), Hebrew letters only. The
  HeliBoard Hebrew list ranks its commonest words out of order (מר above של,
  אני outside its top 20), and Hebrew has no capitals for a subtitle list to
  lose. It is CC BY-SA 4.0 like its source.

`zh-pinyin.tsv`, used by both Chinese Pinyin profiles, holds toneless Pinyin
(`v` for ü, apostrophes before vowel-initial syllables) rather than characters:
the composer then offers characters. It is derived from jieba's `dict.txt`
(https://github.com/fxsjy/jieba commit `67fa2e36e72f69d9134b8a1037b83fbb070b9775`,
SHA-256 `7197c3211ddd98962b036cdf40324d1ea2bfaa12bd028e68faa70111a88e12a8`,
MIT, `LICENSE.jieba`) with readings from pypinyin 0.55.0 (MIT,
`LICENSE.pypinyin`). Words of one to four characters were converted, counts of
identical Pinyin summed, and the 100,000 most frequent kept
(`--format jieba-pinyin`).

## Offensive words

Each `<id>-offensive.tsv` lists, one per line, the words of `<id>.tsv` that
typing suggestions rank below every other completion until the user has used
them. Swipe typing doesn't read them: a swipe spells the whole word. The lists
are built by `tools/build_offensive_words.py` from the "List of Dirty, Naughty,
Obscene, and Otherwise Bad Words",
https://github.com/LDNOOBW/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words,
commit `5faf2ba42d7b1c0977169ec3611df25a3c08eb13`, licensed under CC BY 4.0,
https://creativecommons.org/licenses/by/4.0/: a word is listed when the word
list has it and the bad words list does. Inflections and compounds the bad
words list lacks are added where they begin with a root every word built on is
offensive in, reviewed by hand (`ROOTS` in the tool: fuck, German scheiß), or
where the word list's AOSP source marks them `possibly_offensive` and they begin
with a listed word of four letters or more; the AOSP marks alone are too broad
(English marks "AI", German "Club"). Everyday
words the bad words lists include, such as Italian "regina", Portuguese "comer"
or English "hardcore", are kept out by hand (`KEEP` in the tool). Ukrainian
uses the Russian bad words list, and Italian and Ukrainian have no AOSP marks.

The bad words lists have no Slovak, Estonian, Latvian, Lithuanian, Romanian,
Croatian, Serbian, Slovenian, Bulgarian, Macedonian, Greek, Catalan,
Icelandic, Indonesian, Vietnamese or Hebrew. Their lists come from
`tools/offensive_words/<language>.txt` instead (`--words`: one word, `root*`
or `!kept word` per line), drafted with Grok 4.7, matched against the word
list, and reviewed for everyday words by AI models, not native speakers; plain
words for "idiot" stay out, as Spanish "idiota" does, and so do the neutral
words for Roma (Turkish "çingene" too). They don't use the AOSP marks, so
every listed word was reviewed. Corrections are welcome. Arabic's file joins
the LDNOOBW Arabic list to Grok's draft and is reviewed the same way, since the
LDNOOBW list holds medical and news words (ثدي, قضيب). Azerbaijani has no
list: none of the drafted words is in its word list, which has no subtitles
blended in.
Drawn from the word lists, the lists are licensed under CC BY-SA 4.0 as they are.

| Bad words list | Used for | SHA-256 |
| --- | --- | --- |
| `en` | `en-us`, `en-gb` | `af851ecef1d5f212caba17339b12ac39cc2fef7d78c74876f67237644fcee8bd` |
| `de` | `de-de` | `ebe44d7772897390e27f7bec412fdcdf0efc294e4db157f8487c4cd8d4fabd55` |
| `es` | `es-es` | `073334261cc2e7c08339faefd2f19f9623e8240a0ac18fe2a26c368b897496ce` |
| `fr` | `fr-fr` | `79763a705e08256b393952852bb63aa903f4485e0ef02f70735f5e50efff213c` |
| `it` | `it-it` | `d413f41018d58aa79d2c4809d0161f29ffd5652c4dc34c5b7b9ade680ee5599a` |
| `nl` | `nl-nl` | `8298ba0cdc3ca9a1989c349b8ad0148e4504c2804f2f2fb6c2f52bfb39c5ae71` |
| `pl` | `pl-pl` | `98cf824e3692255645c5a72d5913f26fd4f20930f725683b4c96bb312ca5d9b3` |
| `pt` | `pt-br` | `05ef524b0bb7f83f87257f6ec4dabf70e16b8bb6930f769fb93deaf608ff196d` |
| `ru` | `ru-ru`, `uk-ua` | `2598adabd2b761733d72a68d8ee4fcc1d2c38f2f85b6ff489985c6ce1d93bf75` |
| `sv` | `sv-se` | `77f3f99a89bd3b2bde6a654741bacfcb3acca3f5c804af4d8d3fea11e09ea562` |
| `cs` | `cs-cz` | `90706df86709bade1ca39cf9f9d9836409c674fc3bffcb7525bdd20e59b596bc` |
| `da` | `da-dk` | `ddb46ab67e955d1a5f49c89c18497d23c3d8d6d2522f3740a5d7583f90e434c6` |
| `fi` | `fi-fi` | `3e0b35b2533ba22fb526d882bea250ddfad907ee7548aacbe15db17e21034c62` |
| `fil` | `fil-ph` | `fb4b43691b1249fee2eca1e66795daedda8e7366d42abcde6553dce611d23d99` |
| `hu` | `hu-hu` | `a6943a971496f16e9a9d59ba6fde146d229699600e9636828d8d34ed9160aff5` |
| `no` | `nb-no` | `e55b46bcec1ea288256924523a997d8d99e124c9aa3e61cbbbfaeca294f745b4` |
| `tr` | `tr-tr` | `6c7ae03baee0f3fdc22eed169030567669005dad9a1923e0b571c4bd4f2f2ab5` |
| `ar` | `ar` | `23dbd9c55257bf2a4965f46664dc7eac04b61ff4eb3571e41f7e964c7c3ed96b` |

The AOSP marks come from the source lists in the table above, except for
`pt-br`, whose marks come from `wordlists/main_pt_BR.combined` at the same
aosp-dictionaries commit (SHA-256
`1e58ac19ee1732027d0598b837adc9e80cbd0c1af767c6e7e1f2dfac7ebee183`).
