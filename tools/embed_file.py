#!/usr/bin/env python3
"""Embed a file's bytes as a C++ array: embed_file.py INPUT OUTPUT NAMESPACE NAME."""
from pathlib import Path
import sys

source, output, namespace, name = sys.argv[1:]
data = Path(source).read_bytes()
rows = [', '.join(str(byte) for byte in data[at:at + 24]) for at in range(0, len(data), 24)]
Path(output).write_text('// Generated from ' + Path(source).name + '; do not edit.\n'
                        '#include <cstddef>\n'
                        f'namespace {namespace} {{\n'
                        f'extern const unsigned char {name}[] = {{\n' + ',\n'.join(rows) + '\n};\n'
                        f'extern const std::size_t {name}_size = {len(data)};\n'
                        '}\n')
