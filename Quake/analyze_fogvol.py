import re

with open('c:/Quake/source/ironwail/Quake/r_fogvol.c', 'r', encoding='utf-8') as f:
    lines = f.readlines()

funcs = []
current_func = None
start_line = 0

brace_depth = 0

for i, line in enumerate(lines):
    # simple heuristic for function start
    if current_func is None and re.match(r'^((static\s+)?[a-zA-Z0-9_]+\s+[*]?)?([a-zA-Z0-9_]+)\s*\(.*\)\s*$', line):
        if not line.endswith(';\n'):
            current_func = re.search(r'([a-zA-Z0-9_]+)\s*\(', line).group(1)
            start_line = i + 1

    if current_func is not None:
        brace_depth += line.count('{')
        brace_depth -= line.count('}')
        if brace_depth == 0 and '}' in line:
            funcs.append((current_func, start_line, i + 1, (i + 1) - start_line + 1))
            current_func = None
            start_line = 0

funcs.sort(key=lambda x: x[3], reverse=True)
with open('c:/tmp/fogvol_analysis.txt', 'w') as out:
    for f in funcs:
        out.write(f"{f[0]} : {f[1]}-{f[2]} ({f[3]} lines)\n")
