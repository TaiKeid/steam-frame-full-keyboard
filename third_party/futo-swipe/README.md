# FUTO Swipe encoder

Swipe typing is powered by "FUTO Swipe" technology.

`model_fp32.pte` is the unmodified `honorable_sturgeon` encoder from
https://huggingface.co/futo-org/futo-swipe at revision
`18328c3042b066952c0936b3771d492fe2ec289a`, file
`honorable_sturgeon/model_fp32.pte`, SHA-256
`725242bab5d14345e96ff214e8de2bfbc1f962c232d320df9c24cb82ffd1fbaf`.
It is distributed under the FUTO Model Weights License 1.0 in the adjacent
`LICENSE.md`; those terms cover only the weights. The model card cites the
technical report "FUTO Swipe: Layout-Agnostic Neural Swipe Decoding"
(arXiv:2606.25247).

Full Keyboard does not use FUTO's GPL-3 `swipe-library`. Input resampling,
the lexicon trie and the beam search are its own implementation in
`src/swipe.cpp`. The model runs through ExecuTorch, built separately by
`tools/build-executorch.sh`; see `../README.md`.
