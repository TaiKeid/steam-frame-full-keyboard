#!/usr/bin/env python3
"""Prune libime's Chinese language model (an ARPA trigram model) to a size.

  prune_chinese_lm.py IN.arpa OUT.arpa FRACTION

Removes FRACTION of the bigrams and trigrams, those that matter least: each
is scored by how far its probability is from what backing off to the shorter
context would give, weighted by how often its context and the word occur
(P(h) p(w|h) |ln p(w|h) - ln p'(w|h)|, after Seymore and Rosenfeld). A bigram
that is the context of a kept trigram stays. The backoff weights of every
context are then computed again, so each context's probabilities still sum
to one. Unigrams are all kept. Built with libime's slm_build_binary as
libime's own data is (tools/build-chinese.sh).
"""
import math
import sys

LN10 = math.log(10)


def read_arpa(path):
    uni, bi, tri = {}, {}, {}
    order = 0
    with open(path, encoding="utf-8") as source:
        for line in source:
            line = line.rstrip("\n")
            if not line or line.startswith("ngram ") or line == "\\data\\":
                continue
            if line.startswith("\\") and line.endswith("-grams:"):
                order = int(line[1])
                continue
            if line == "\\end\\":
                break
            fields = line.split("\t")
            prob = float(fields[0])
            words = fields[1].split(" ")
            backoff = float(fields[2]) if len(fields) > 2 else 0.0
            if order == 1:
                uni[words[0]] = [prob, backoff]
            elif order == 2:
                bi[(words[0], words[1])] = [prob, backoff]
            else:
                tri[(words[0], words[1], words[2])] = prob
    return uni, bi, tri


def bigram_prob(uni, bi, a, b):
    """log10 p(b | a), backing off to the unigram."""
    entry = bi.get((a, b))
    return entry[0] if entry else uni[a][1] + uni[b][0]


def prune(uni, bi, tri, fraction):
    # Scores in natural logs: P(h) p(w|h) |ln p - ln p'|.
    scored = []
    for (a, b, c), prob in tri.items():
        lower = bi[(a, b)][1] + bigram_prob(uni, bi, b, c) if (a, b) in bi else None
        if lower is None:
            continue  # no context bigram: kept as it is
        context = uni[a][0] + bigram_prob(uni, bi, a, b)
        scored.append((10 ** (context + prob) * abs(prob - lower) * LN10, 3, (a, b, c)))
    for (a, b), (prob, _) in bi.items():
        lower = uni[a][1] + uni[b][0]
        scored.append((10 ** (uni[a][0] + prob) * abs(prob - lower) * LN10, 2, (a, b)))
    scored.sort(key=lambda item: item[0])
    target = int(fraction * (len(bi) + len(tri)))
    # Trigrams go first at a given score; a bigram goes only once no kept
    # trigram needs it as context, so count those against the target.
    removed_tri = set()
    candidates_bi = []
    for score, order, key in scored[:target]:
        if order == 3:
            removed_tri.add(key)
        else:
            candidates_bi.append(key)
    needed = {(a, b) for (a, b, c) in tri if (a, b, c) not in removed_tri}
    removed_bi = {key for key in candidates_bi if key not in needed}
    for key in removed_tri:
        del tri[key]
    for key in removed_bi:
        del bi[key]
    return len(removed_bi), len(removed_tri)


def renormalize(uni, bi, tri):
    """Backoff weights so that each context's probabilities sum to one."""
    def backoff(kept_probs, lower_probs, old):
        numerator = 1 - sum(10 ** p for p in kept_probs)
        denominator = 1 - sum(10 ** p for p in lower_probs)
        if numerator <= 1e-12 or denominator <= 1e-12:
            return old
        return math.log10(numerator / denominator)

    children = {}
    for (a, b), (prob, _) in bi.items():
        children.setdefault(a, []).append((prob, uni[b][0]))
    for word, entry in uni.items():
        kids = children.get(word)
        entry[1] = backoff([k[0] for k in kids], [k[1] for k in kids], entry[1]) if kids else 0.0
    children = {}
    for (a, b, c), prob in tri.items():
        children.setdefault((a, b), []).append((prob, bigram_prob(uni, bi, b, c)))
    for key, entry in bi.items():
        kids = children.get(key)
        entry[1] = backoff([k[0] for k in kids], [k[1] for k in kids], entry[1]) if kids else 0.0


def write_arpa(path, uni, bi, tri):
    with open(path, "w", encoding="utf-8") as out:
        out.write(f"\\data\\\nngram 1={len(uni)}\nngram 2={len(bi)}\nngram 3={len(tri)}\n\n")
        out.write("\\1-grams:\n")
        for word, (prob, backoff) in uni.items():
            out.write(f"{prob:.7g}\t{word}\t{backoff:.7g}\n")
        out.write("\n\\2-grams:\n")
        for (a, b), (prob, backoff) in bi.items():
            out.write(f"{prob:.7g}\t{a} {b}\t{backoff:.7g}\n")
        out.write("\n\\3-grams:\n")
        for (a, b, c), prob in tri.items():
            out.write(f"{prob:.7g}\t{a} {b} {c}\n")
        out.write("\n\\end\\\n")


def main():
    source, target, fraction = sys.argv[1], sys.argv[2], float(sys.argv[3])
    uni, bi, tri = read_arpa(source)
    before = (len(bi), len(tri))
    removed = prune(uni, bi, tri, fraction)
    renormalize(uni, bi, tri)
    write_arpa(target, uni, bi, tri)
    print(f"bigrams {before[0]} -> {len(bi)} (-{removed[0]}), "
          f"trigrams {before[1]} -> {len(tri)} (-{removed[1]})")


if __name__ == "__main__":
    main()
