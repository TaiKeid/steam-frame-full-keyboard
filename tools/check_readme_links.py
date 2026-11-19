#!/usr/bin/env python3
"""Check the links in every README.

check_readme_links.py ROOT

Every README.md and README.*.md in ROOT: a relative link or image must name
a file or folder that exists, and an anchor (#...) must name a heading of the
file it points into, as GitHub makes heading anchors. Web links are not
fetched.
Prints each broken link; exit 1 when there is one.
"""

import re
import sys
from pathlib import Path

LINK = re.compile(r'\[[^\]]*\]\(([^)\s]+)\)|<(?:a|img)\s[^>]*(?:href|src)="([^"]+)"')
HEADING = re.compile(r'^(#{1,6})\s+(.*?)\s*#*\s*$')
FENCE = re.compile(r'^\s*(```|~~~)')


def slug(text):
    """GitHub's anchor for a heading: lower case, without markup and
    punctuation, spaces as hyphens."""
    text = re.sub(r'`([^`]*)`', r'\1', text)
    text = re.sub(r'\[([^\]]*)\]\([^)]*\)', r'\1', text)
    text = re.sub(r'<[^>]+>', '', text)
    text = text.strip().lower()
    text = ''.join(c for c in text if c.isalnum() or c in ' -_')
    return text.replace(' ', '-')


def anchors(path):
    found, seen, fenced = set(), {}, False
    for line in path.read_text(encoding='utf-8').splitlines():
        if FENCE.match(line):
            fenced = not fenced
            continue
        match = None if fenced else HEADING.match(line)
        if match:
            base = slug(match.group(2))
            count = seen.get(base, 0)
            seen[base] = count + 1
            found.add(base if count == 0 else f'{base}-{count}')
    return found


def links(path):
    fenced = False
    for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        if FENCE.match(line):
            fenced = not fenced
            continue
        if fenced:
            continue
        for match in LINK.finditer(line):
            yield number, match.group(1) or match.group(2)


def check(root):
    problems = []
    readmes = sorted(root.glob('README.md')) + sorted(root.glob('README.*.md'))
    for readme in readmes:
        for number, target in links(readme):
            if re.match(r'^[a-z][a-z0-9+.-]*:', target):  # https:, mailto:
                continue
            name, _, anchor = target.partition('#')
            file = (readme.parent / name).resolve() if name else readme
            where = f'{readme.name}:{number}: {target}'
            if not file.exists():
                problems.append(f'{where}: no such file')
            elif anchor and file.suffix == '.md' and anchor not in anchors(file):
                problems.append(f'{where}: no such heading')
    return readmes, problems


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    readmes, problems = check(Path(argv[1]))
    for problem in problems:
        print(problem)
    print(f'{len(readmes)} READMEs, {len(problems)} broken links')
    return 1 if problems or not readmes else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
