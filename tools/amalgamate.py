#!/usr/bin/env python3
import os, re, sys
SRC = os.path.join(os.path.dirname(__file__), '..', 'src')
OUT = os.path.join(os.path.dirname(__file__), '..', 'dist')
os.makedirs(OUT, exist_ok=True)

# Collect all .c files
sources = []
for root, dirs, files in os.walk(SRC):
    for f in sorted(files):
        if f.endswith('.c'):
            sources.append(os.path.join(root, f))

with open(os.path.join(OUT, 'kanbudb.c'), 'w') as out:
    out.write('/* KanbuDB Embedded Database - Amalgamated */\n')
    for src in sorted(sources):
        rel = os.path.relpath(src, SRC)
        out.write(f'\n/* === {rel} === */\n\n')
        with open(src) as f:
            out.write(f.read())

# Also copy the public header and inline all #include "..."
with open(os.path.join(OUT, 'kanbudb.h'), 'w') as out:
    out.write('/* KanbuDB Embedded Database - Single Header */\n')
    with open(os.path.join(os.path.dirname(__file__), '..', 'include', 'db.h')) as f:
        content = f.read()
        def repl(m):
            path = os.path.join(SRC, m.group(1))
            if os.path.exists(path):
                with open(path) as inc:
                    return inc.read()
            path2 = os.path.join(os.path.dirname(__file__), '..', 'include', m.group(1))
            if os.path.exists(path2):
                with open(path2) as inc:
                    return inc.read()
            return m.group(0)
        content = re.sub(r'#include\s+"([^"]+)"', repl, content)
        out.write(content)
