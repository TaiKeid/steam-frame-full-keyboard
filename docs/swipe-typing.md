# Swipe typing plan

Status: implemented on `feature/swipe-typing` for English (US, UK), German, French, Spanish, Italian, Brazilian Portuguese, Ukrainian, Russian and Chinese Pinyin, and deployed to the Frame for in-headset testing (2026-10-23). Japanese and Korean are not started. Feasibility was checked the same day; see [results](#feasibility-results). Mark milestone checkboxes only after their acceptance conditions pass. User-facing behavior is described in [configuration](configuration.md#swipe-typing).

Swipe typing lets a user hold the trigger, trace a word across the letter keys and release. The keyboard decodes the path into a word and alternatives. Tapping keeps working exactly as before.

## Decisions

- **Decoder:** FUTO Swipe encoder (`honorable_sturgeon`, 635K parameters, ExecuTorch `.pte`) plus our own trie-constrained CTC beam search. Its key layout is supplied at runtime, so it works with file-defined layouts. FUTO's `swipe-library` is GPL-3 and is not used or copied; preprocessing and search follow the published technical report.
- **Notice:** the weights license requires a visible end-user notice. Settings shows "Powered by FUTO Swipe" as a second line under the Swipe typing label, whether swipe is on or off. Ship FUTO's `LICENSE.md` beside the model. The FUTO name is used for nothing else.
- **Setting:** a Swipe typing checkbox in the Languages and Layouts card, beside the Layout and Language dropdowns. It is saved as the optional global boolean `swipe_typing` and defaults to false. Apply and save activates it; Back discards it.
- **Unsupported languages:** the checkbox is disabled (drawn with the theme's disabled color, not clickable) whenever the language selected in the dropdown has no `swipe` support, or the swipe engine module or that language's word list is unavailable. Like Start with SteamOS, the saved choice is kept while disabled, but swipe is active only when the saved boolean is true and the active language supports it. Cycling to a favorite without swipe support turns swipe off for that language without changing the saved value.
- **Language order:** English, German, Spanish and Japanese romaji; then Korean and Chinese; then the remaining XKB profiles; Russian last.
- **Privacy:** no learning, no typing history, no logging of paths or decoded words. The development trace recorder only records prompted test phrases.

## Interaction model

1. **Tap or swipe.** With swipe disabled, keys keep sending on press, exactly as today. For now, with swipe enabled and a swipe-capable language active, a letter key's text is sent on release instead of on press. A press becomes a swipe once the pointer leaves the start key by more than half a key width. Non-letter keys, modifiers, shortcuts and Settings are unchanged. Key repeat only starts if the pointer stays on the start key for 500 ms. The owner considers send-on-release unsuitable for normal typing, so this is temporary; see the open issue below.
2. **Capture.** `App::move` already receives OpenVR `MouseMove` events while the trigger is held. A `SwipeTracker` stores time-stamped case coordinates for the owning pointer. One swipe is active at a time; a second controller's press cancels nothing and is ignored for swiping.
3. **Decode.** On release, decoding runs on a worker thread, never on the VR loop: resample to 60 Hz and then 64 points, normalise to the letter block, encoder, beam search, rescoring.
4. **Commit.** Open decision, below.
5. **Shift and Caps.** A latched Shift capitalises the first letter; Caps Lock uppercases the word. Otherwise the word list's own capitalisation is used, so German nouns stay capitalised.
6. **Auto-space.** A space goes before a swiped word when the previous action was a swiped word. Punctuation keys follow normal rules.
7. **Cancellation.** Hide, drag, recentre, target change, profile change, FocusLeave of the owning controller and shutdown discard an unfinished swipe and any pending alternatives; a word still decoding when its laser leaves is discarded rather than typed at a cursor that may have moved. Lost tracking cancels a swipe like a held key. They release nothing extra, because a swipe holds no backend key.

### How a swiped word reaches the field

The first version uses option A below: immediate commit with guarded replacement. The owner has not chosen yet; option B stays possible.

| Option | Behaviour | Risk |
| --- | --- | --- |
| **A. Immediate commit (recommended)** | The top word is committed through the existing text path. Alternatives show in the candidate row. Picking one sends Backspace × length plus the new word. | Picking an alternative after the user moved the cursor would edit the wrong text. Alternatives are invalidated when any key is pressed, any pointer leaves the panel, the target changes or the panel hides. |
| B. Preedit first | The word stays in the panel preedit, like Chinese/Korean composition, and is committed by the next swipe, Space or tap. | Safe, but the text doesn't appear in the field until the next action, which is unusual for Latin swipe typing. |

Japanese always uses the composer preedit, because the result is a reading for kanji conversion.

### Open issue: tap timing with swipe enabled

Sending on release is the interim behaviour while swipe is enabled, but it won't work for normal typing. The owner has further ideas, which are pending. The final design must keep press-time typing for taps while still letting a press start a swipe. Don't build anything in M1 that can only work with release-time text.

## Architecture

| Piece | Responsibility | Notes |
| --- | --- | --- |
| `SwipeTracker` | Per-pointer path capture, tap/swipe classification and cancellation | Host-testable; no model needed |
| `SwipeLayout` | Swipe key centres from layout geometry and the keymap's level-1 letters; accent folding | Derived per active layout/language pair |
| `SwipeLexicon` | Word list loading, collapsed key path trie, frequencies | File-based; user overrides by ID |
| `SwipeDecoder` | Interface: path → ranked words | FUTO implementation behind it; a stub decoder for tests |
| Swipe engine module | ExecuTorch, XNNPACK and the model, loaded on demand with `dlopen` | Same pattern as the CJK engine adapters. Missing module disables swipe only. |
| App integration | Deferred letter text, commit or preedit, candidate row, Settings notice | Rendering stays free of input delivery |

The language profile gains an optional `swipe` object, for example `{"lexicon": "en-us"}`. A profile without it doesn't offer swipe. Swipe support for a language means: a word list, measured accuracy and on-device acceptance. Relabelled keys alone don't count.

### Data files

- Model: `share/framekeyboard/swipe/futo-swipe/model_fp32.pte` (2.65 MB, SHA-256 `725242ba…fbaf`), with FUTO's `LICENSE.md` and a provenance file. Keep the original `.pte`; converting the weights would make them a modified Derivative Model with extra notice duties.
- Word lists: `share/framekeyboard/swipe/lexicons/<id>.tsv` with `word<TAB>frequency`, plus user overrides under `~/.config/framekeyboard/swipe/lexicons/`. Size target is about 100k entries per language, loaded lazily and bounded.
- Scoring constants: in the lexicon file header or a small JSON file, tuned per language on held-out data.

### Build

- ExecuTorch v1.5.1 (commit `3b60683923245cf472b7323426920e15623ba361`, BSD-3) with XNNPACK. Built from a pinned source by a script into a separate prefix for host and ARM64, then linked statically into the swipe engine module. Builds without that prefix skip the module, as builds without PyZy skip Chinese.
- `cmake/steam-frame-aarch64.cmake` must set `CMAKE_ASM_COMPILER`, `CMAKE_ASM_COMPILER_TARGET` and the matching `--gcc-toolchain` flags. Without them, XNNPACK's `.S` kernels are assembled for x86 and the build fails.
- Third-party notices: ExecuTorch, XNNPACK, cpuinfo, pthreadpool and FP16 (BSD); flatbuffers and KleidiAI (Apache-2.0); FUTO Swipe weights (FUTO Model Weights License 1.0); word list attributions.

## Milestones

First-version status (2026-10-23): M0 and M1 code exist with host and ARM64 tests; M2's Settings, notice, trail, suggestions and commit are implemented and deployed, awaiting in-headset acceptance; M3 has word lists and per-language checks but no headset acceptance. The desktop preview and trace recorder are not done.

- [ ] **M0 Build and runtime.** Toolchain ASM fix; pinned ExecuTorch build script; swipe engine module loading the model; host and ARM64 builds. Acceptance: module loads on Frame, decodes the reference swipe, and absence is handled cleanly.
- [ ] **M1 Core path, host-tested.** `SwipeTracker`, deferred letter text, `SwipeLayout`, lexicon trie, C++ beam search, decoder interface, cancellation. Acceptance: host tests for tap/swipe classification, every cancel path, folding, collapsed paths and decoding of fixed traces.
- [ ] **M2 English end-to-end.** Languages and Layouts checkbox with the support-based disabled state and the notice, candidate row, chosen commit option, desktop-preview mouse swiping, development trace recorder. Acceptance: on-device prompted-phrase test with a dedicated receiver, latency under one frame on the VR loop, and the user's in-headset judgement.
- [ ] **M3 German and Spanish.** Word lists with case, ä/ö/ü/ß and ñ/ç keys, accent folding for dead-key letters, per-language scoring tuned on FUTO swipe-5 data. Acceptance: held-out accuracy no worse than the feasibility numbers, plus in-headset phrases.
- [ ] **M4 Japanese romaji.** Reading word list from Mozc OSS data, romaji paths generated by reversing `roman_table()` in `src/japanese.cpp`. The decoded reading goes to `JapaneseComposer::kana()`, then the existing Anthy conversion. Clean fragments out of the readings. Acceptance: synthetic and recorded tests, plus fluent-speaker review.
- [ ] **M5 Korean and Chinese.** Korean: jamo paths on two-set keys, Shift jamo folded, syllables composed for output. Chinese: pinyin word paths, decoded pinyin fed to `CjkComposer` for character candidates. Each needs a word list source, accuracy measurement and fluent review.
- [ ] **M6 Remaining profiles.** French, Italian, Portuguese, Ukrainian; Russian last.

## Added languages (2026-10-23)

- Word lists: HeliBoard/Leipzig CC BY lists for English UK, French, Italian, Ukrainian and Russian; Brazilian Portuguese built from Leipzig `por-br_newscrawl_2011_1M` (HeliBoard's Brazilian list has no stated licence); Chinese as toneless Pinyin from jieba (MIT) and pypinyin (MIT).
- French on real FUTO AZERTY swipes, top-1: accent keys only 72.0%, plain letters only 78.7%, both accepted 77.8%. Number-row accent keys therefore accept both paths; accents inside the letter rows keep their own key.
- The normalization frame now spans every letter key in the three main letter rows, found by physical position. German 84.5% top-1 (83.7% with a–z only), Spanish unchanged at 92.5%; it also keeps keys such as Italian à inside the encoder's range, where they no longer mirror onto inner keys.
- Chinese: a swipe decodes toneless Pinyin and types it into the existing composer, which offers characters. Its accuracy is not measured beyond fixed checks; it needs fluent review.
- Not measured on real swipes: English UK, Italian, Brazilian Portuguese, Ukrainian, Russian (FUTO reports 83–86% for Russian on its own setup).

## Feasibility results

All results were measured on 2026-10-23. Unless noted, they come from host x86_64 runs using ExecuTorch's Python runtime and a simplified Python beam search (width 64, frequency and length terms tuned on English dev swipes). Production numbers will differ.

### Real swipes on Full Keyboard geometry

Real phone swipes from FUTO's MIT-licensed dataset. English uses the swipe-1 test split; German and Spanish come from swipe-5, recorded after the encoder was exported. Each swipe was warped piecewise-affinely from the phone layout's key centres to Full Keyboard's `en-us`/`international` key centres. Tremor adds a smoothed random offset with 120 ms correlation, scaled in key pitches. Word lists: wordfreq top 100k (test only); target words not in the list were added, as in FUTO's paper. 600 swipes per language.

| Language | Phone layout | Full Keyboard | + tremor 0.08 | + tremor 0.15 | Full Keyboard top-3 / top-5 |
| --- | --- | --- | --- | --- | --- |
| English | 89.8% | 90.3% | 89.3% | 87.5% | 96.7% / 96.8% |
| German | 84.7% | 83.2% | 81.2% | 80.3% | 90.8% / 92.5% |
| Spanish | 92.7% | 92.2% | 92.2% | 91.5% | 96.7% / 98.0% |

Moving swipes onto our geometry costs at most about 1.5 points. FUTO reports 92.9% English top-1 with their full search, so a production search should gain a little.

### Japanese romaji (synthetic)

No real romaji swipes exist. Synthetic paths use minimum-jerk motion between keys, a 60 ms corner-rounding average and a Gaussian landing error per key. A landing error of 0.25 key pitch reproduces the real English result (88.0% synthetic vs 90.3% real) and was used for Japanese. Readings: Mozc OSS dictionary, top 100k readings by cost, written as Hepburn romaji.

| Landing error | English top-1 | Japanese reading top-1 | Japanese top-3 / top-5 |
| --- | --- | --- | --- |
| 0.15 | 94.7% | 87.3% | 97.0% / 98.7% |
| 0.25 (calibrated) | 88.0% | 82.3% | 94.3% / 97.0% |
| 0.25 + tremor 0.15 | 91.3% | 80.7% | 93.7% / 96.0% |

Typical misses are near-identical readings (ずか/づか, きしゃ/こしゃ) and lexicon fragments such as `っし`. Anthy's conversion step and a cleaner reading list should absorb part of this. 21 of 100k readings exceed the 31-key path limit of the 32-step output.

### Frame runtime

ExecuTorch 1.5.1 with XNNPACK cross-built for ARM64 once the ASM target was set. A small Module API benchmark (5.7 MB unstripped, only system library dependencies) ran from a temporary directory on Frame, which was removed afterwards:

| Core | Encoder p50 | p90 | Model load |
| --- | --- | --- | --- |
| Cortex-A720 (cpu7) | 1.34 ms | 1.37 ms | 5.3 ms |
| Cortex-A520 (cpu0) | 8.03 ms | 8.35 ms | 9.1 ms |

The reference swipe decoded correctly on Frame; peak resident memory was 10.7 MB. The beam search is not yet measured in C++; the Python version took 10–17 ms per word on the host.

### Word lists

| Language | Candidate source | License | Notes |
| --- | --- | --- | --- |
| English, German, Spanish | HeliBoard `aosp-dictionaries` experimental/main lists built from Leipzig Wortschatz | CC BY 4.0 | Case preserved (`Straße`, `Haus`); frequencies 0–255 |
| Japanese | Mozc `dictionary_oss` readings and costs | IPAdic/ICOT terms, BSD-style with notice | Needs a fragment filter |
| Korean, Chinese | Not chosen | — | M5 |
| Testing only | wordfreq | Data CC BY-SA 4.0 | Lowercases everything, so unsuitable for German shipping |

## Risks and unknowns

- **No real laser swipes yet.** Tremor and landing error are simulated. M2's recorded phrases are the first real test; stop or retune if in-headset accuracy is well below these numbers.
- **German is the weakest tier-one language** (about 83%). Long compounds and capitalised duplicates need per-language tuning.
- **Japanese is a new pipeline.** Only synthetic evidence exists, and fluent review is required.
- **Little-core scheduling.** About 8 ms on an A520 is still below one 90 Hz frame, but decoding must stay off the VR thread.
- **Build host dependencies.** The ExecuTorch configure step used a Python environment with PyTorch for kernel code generation. Confirm the minimal requirements before writing the build script.
- **Package size.** About 2.6 MB for the model, a few MB for the static engine, and roughly 1–2 MB per compressed word list.
