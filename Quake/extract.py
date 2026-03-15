import os
import re

cvars = []
for root, _, files in os.walk('c:/Quake/source/ironwail/Quake'):
    for file in files:
        if file.endswith('.c'):
            try:
                with open(os.path.join(root, file), 'r', encoding='utf-8') as f:
                    for line in f:
                        match = re.search(r'Cvar_RegisterVariable\s*\(\w*&(r_|gl_|v_|_r_)\w+\)', line)
                        if match:
                            v = re.search(r'&(r_|gl_|v_|_r_)(\w+)', line)
                            if v:
                                cvars.append(v.group(1) + v.group(2))
            except Exception: pass

with open('cvars.txt', 'w') as out:
    out.write('\n'.join(sorted(set(cvars))))
