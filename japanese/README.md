# Japanese word list

`words.tsv` holds the 20,000 commonest Japanese words with an all-hiragana
reading, as `reading<TAB>word<TAB>cost` (lower is commoner), sorted by reading
and then cost. Japanese input completes a partly typed reading to them
(docs/japanese.md, Prediction): おめでと offers おめでとう and おめでとうございます.

The list is derived from Mozc's open-source dictionary,
`src/data/dictionary_oss/` of https://github.com/google/mozc at commit
`74f8ee66c5546d1f3ce4fadbef23a3b402239eed`. Only dictionary forms of nouns,
verbs, adjectives, adverbs, interjections, adnominals and conjunctions are
kept: no proper nouns, particles, affixes, inflected stems or words written in
Latin letters or digits, and none of Mozc's own suggestion filter. Mozc is
under the BSD 3-Clause license; its dictionary data is derived from IPAdic
(and ICOT Free Software) and the public-domain Okinawa dictionary. All their
notices are in `third_party/mozc/LICENSE`.

Rebuild it from those files with:

```sh
python3 -I tools/build_japanese_words.py <mozc>/src/data/dictionary_oss japanese/words.tsv --count 20000
```

| Source file | SHA-256 |
| --- | --- |
| `dictionary00.txt` | `9e07ce292932c4fff327ed8cb06274c09acca556bd8bdf7c7ef1f6173eae3b61` |
| `dictionary01.txt` | `836fd5fd8399510de0d8ef7b33be913cca6bbaee7b712cc8045fba03f4c75a89` |
| `dictionary02.txt` | `93943858d0cfd092b849a78def6901eaf228177e1ac5663544e865a3a1e45a9e` |
| `dictionary03.txt` | `cb9f1b0b4a7550d682d86244a021cd17e0b15366328819e69135b5f183a1a24b` |
| `dictionary04.txt` | `9fdc4e3948d03463de13ab5782adffbc11dd06e934cb80138c5920029cd3d2f6` |
| `dictionary05.txt` | `199e3b0f050b691823ae7ba943090d437f51af6436b27c5f3dc69cafa12000e4` |
| `dictionary06.txt` | `b95b14f7770ae2a824bc8e9fad8b03c1bc45c504f93a36554b8b56daa0368d2a` |
| `dictionary07.txt` | `7f53649b529d4c8c495d3a8dd9c4d489dfe7696080f7423107ff01331174be48` |
| `dictionary08.txt` | `50a31bb5fc1646721b00be371a5695dffab1292bda143e1e59cc70cbbdd8643f` |
| `dictionary09.txt` | `4e5fcd4cbfbf11395166a37b506f547fc54d20edc0884f5184ecd3fc705f0d9a` |
| `id.def` | `07a05a268c3783b7e02b36e4aa591555a2dd19a1f3c93891576740761c154f2d` |
| `suggestion_filter.txt` | `2c8fccb65be739cadfa3524921584e84914c96b9977bd1d42834d79700eaa6b5` |

## Kaomoji

`kaomoji.tsv` lists 442 kaomoji under their readings, as
`reading<TAB>kaomoji<TAB>order`, and every one of them under かおもじ. Live
candidates offer up to six for the reading as typed, after its emoji, and all of
them for かおもじ. They come from Mozc's `src/data/emoticon/` at the same commit,
under Mozc's BSD 3-Clause license (`third_party/mozc/LICENSE`); a kaomoji in both
of its files keeps its first place and the readings of both. Rebuild it with:

```sh
python3 -I tools/build_japanese_kaomoji.py <mozc>/src/data/emoticon japanese/kaomoji.tsv
```

| Source file | SHA-256 |
| --- | --- |
| `emoticon.tsv` | `366558b380bef07dda26822c9100d1efabee539f7961b2c6363d4614c4a762c4` |
| `categorized.tsv` | `4497c16a706de418b05e73eaddbce13e5d3390e7c2de71200b28c0c97ae5c4fc` |

## Preferred conversions

`preferred.tsv` holds the usual conversions of about 100 common chat phrases,
as `reading<TAB>phrase<TAB>0` sorted by reading: いきます 行きます, ありがとう
ありがとう. It is Full Keyboard's own first draft, under the project's license,
written to be reviewed by native speakers.
