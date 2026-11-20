"""Reproducible static research of the saved ATS/ETS2 1.62 images (Capstone 5)."""
import json
import difflib
from pathlib import Path
from compare_executables import Image, flow_tokens, ROOT

ROOT_EXE = ROOT / 'diagnostics/executables'
REFERENCE = {'clear_vector': 0x1189f0, 'fill': 0x7b7890, 'refill': 0x7b8590,
             'generator': 0x7ba390, 'clear': 0x7bb500, 'schedule': 0x808860}
TARGETS = ['ats-oculus-1.62.1.1001/amtrucks.exe', 'ats-public-1.62.1.1/amtrucks.exe',
           'ats-oculus-1.62.1.1001-build-26457597-staged/amtrucks.exe',
           'ets2-public-1.62.1.0/eurotrucks2.exe']

def main():
    reference = Image(ROOT_EXE / 'public_beta-1.62.0.10/eurotrucks2.exe')
    report = {}
    for filename in TARGETS:
        image = Image(ROOT_EXE / filename)
        result = {}
        for name, old in REFERENCE.items():
            original = reference.normalized(old)
            candidates = [s for s in image.functions if image.data[s:s+16] == reference.data[old:old+16]]
            exact = [s for s in candidates if image.normalized(s) == original]
            if len(exact) == 1:
                best = exact[0]
            else:
                ranked = sorted(((difflib.SequenceMatcher(None, original, image.normalized(s), autojunk=False).ratio(), s)
                                 for s in candidates), reverse=True)
                if not ranked: raise ValueError((filename, name, 'no candidates'))
                best = ranked[0][1]
                print(filename, name, 'candidates', [(round(score, 4), hex(s)) for score, s in ranked[:3]], flush=True)
            old_flow, new_flow = reference.flow(old), image.flow(best)
            at, bt = flow_tokens(old_flow), flow_tokens(new_flow)
            result[name] = {'rva': hex(best), 'normalized_equal': at == bt,
                            'instructions': len(new_flow), 'reference_instructions': len(old_flow)}
            dump = '\n'.join(f'{i.address:x} {i.bytes.hex():30s} {i.mnemonic} {i.op_str}' for i in new_flow)
            (ROOT_EXE / f'{Path(filename).parent.name}-{name}.txt').write_text(dump)
            differences = []
            for tag, a, b, c, d in difflib.SequenceMatcher(None, at, bt, autojunk=False).get_opcodes():
                if tag == 'equal': continue
                differences.append(tag)
                differences += [f'REF {i.address:x} {i.mnemonic} {i.op_str}' for i in old_flow[a:b]]
                differences += [f'NEW {i.address:x} {i.mnemonic} {i.op_str}' for i in new_flow[c:d]]
            (ROOT_EXE / f'{Path(filename).parent.name}-{name}-diff.txt').write_text('\n'.join(differences))
        report[filename] = result
        print(filename, json.dumps(result), flush=True)
    (ROOT_EXE / 'research-1.62.json').write_text(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
