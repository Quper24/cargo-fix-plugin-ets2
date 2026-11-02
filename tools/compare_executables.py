"""Offline compatibility research. Requires Capstone 5 (not a runtime dependency)."""
import sys, struct, json, hashlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'diagnostics/python-libs'))
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_GRP_JUMP, CS_GRP_CALL
from capstone.x86 import X86_OP_MEM, X86_REG_RIP, X86_OP_IMM

ROOT = Path(__file__).resolve().parents[1]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

class Image:
    def __init__(self, path, mapped=False):
        self.path = path
        raw = path.read_bytes()
        pe = struct.unpack_from('<I', raw, 0x3c)[0]
        count, = struct.unpack_from('<H', raw, pe+6)
        optlen, = struct.unpack_from('<H', raw, pe+20)
        opt = pe+24
        self.base, = struct.unpack_from('<Q', raw, opt+24)
        size, = struct.unpack_from('<I', raw, opt+56)
        self.data = bytearray(raw) if mapped else bytearray(size)
        if not mapped:
            self.data[:0x400] = raw[:0x400]
            for n in range(count):
                s = opt+optlen+n*40
                va, length, offset = struct.unpack_from('<III', raw, s+12)
                self.data[va:va+length] = raw[offset:offset+length]
        pdata, length = struct.unpack_from('<II', raw, opt+112+3*8)
        self.functions = {}
        for p in range(pdata, pdata+length, 12):
            start, end, _ = struct.unpack_from('<III', self.data, p)
            if start and end > start: self.functions[start] = end

    def ins(self, start, end=None):
        return list(md.disasm(bytes(self.data[start:end or self.functions[start]]), start))

    def normalized(self, start):
        code = bytearray(self.data[start:self.functions[start]])
        for i in self.ins(start):
            fields = []
            if any(o.type == X86_OP_MEM and o.mem.base == X86_REG_RIP for o in i.operands):
                fields.append((i.disp_offset, i.disp_size))
            if i.group(CS_GRP_CALL) or i.group(CS_GRP_JUMP):
                fields.append((i.imm_offset, i.imm_size))
            for off, size in fields:
                if size: code[i.address-start+off:i.address-start+off+size] = bytes(size)
        return bytes(code)

    def flow(self, start):
        pending, found = [start], {}
        while pending:
            pc = pending.pop()
            if pc in found: continue
            if not start <= pc < start+0x10000: raise ValueError(f'Unexpected flow {pc:x}')
            for i in md.disasm(bytes(self.data[pc:pc+0x4000]), pc):
                if i.address in found: break
                found[i.address] = i
                if i.group(CS_GRP_JUMP):
                    if i.operands[0].type != X86_OP_IMM: raise ValueError('Indirect jump')
                    pending.append(i.operands[0].imm)
                    if i.mnemonic == 'jmp': break
                if i.mnemonic in ('ret', 'int3'): break
        return [i for _,i in sorted(found.items()) if i.mnemonic != 'nop']

def flow_tokens(ins):
    positions = {i.address:n for n,i in enumerate(ins)}
    tokens = []
    for i in ins:
        if i.group(CS_GRP_JUMP):
            tokens.append((i.mnemonic, positions[i.operands[0].imm]))
        elif i.group(CS_GRP_CALL) and i.operands[0].type == X86_OP_IMM:
            tokens.append(('call', 'relative'))
        else:
            code = bytearray(i.bytes)
            if any(o.type == X86_OP_MEM and o.mem.base == X86_REG_RIP for o in i.operands):
                code[i.disp_offset:i.disp_offset+i.disp_size] = bytes(i.disp_size)
            tokens.append((i.mnemonic, code.hex()))
    return tokens

def discover():
    reference = Image(ROOT/'diagnostics/executables/reference-1.61.1.9-mapped.bin', True)
    funcs = {'clear_vector':0x116510, 'fill':0x7cd0f0,'refill':0x7cddf0,
             'generator':0x7cfba0, 'clear':0x7d0ce0,'schedule':0x81cec9}
    # Schedule hook is a secondary entry following a small shared prologue.
    for name, rva in list(funcs.items()):
        if rva not in reference.functions:
            owner = next(((s,e) for s,e in reference.functions.items() if s <= rva < e), None)
            print(name, hex(rva), 'owner', owner)
            if owner: funcs[name] = owner[0]
    report = {}
    for path in sorted((ROOT/'diagnostics/executables').glob('*/eurotrucks2.exe')):
        target = Image(path)
        index = {}
        lengths = {reference.functions[s]-s for s in funcs.values()}
        for s,e in target.functions.items():
            if e-s in lengths:
                index.setdefault(target.normalized(s), []).append(s)
        result = {}
        for name, s in funcs.items():
            matches = index.get(reference.normalized(s), [])
            result[name] = [hex(x) for x in matches]
        report[path.parent.name] = result
        print(path.parent.name, json.dumps(result))
    (ROOT/'diagnostics/executables/comparison.json').write_text(json.dumps(report, indent=2))

def verify():
    import difflib
    root = ROOT/'diagnostics/executables'
    reference = Image(root/'reference-1.61.1.9-mapped.bin', True)
    names = ['clear_vector', 'fill', 'refill', 'generator', 'clear', 'schedule']
    original = [0x116510, 0x7cd0f0, 0x7cddf0, 0x7cfba0, 0x7d0ce0, 0x81cec0]
    profiles = {
        'temporary_1_61-1.61.1.1': [0x116470,0x7cccd0,0x7cd9d0,0x7cf780,0x7d08c0,0x81caa0],
        'oculus-1.61.1.1009': [0x116760,0x7cd320,0x7ce020,0x7cfdd0,0x7d0f10,0x81d0f0],
        'public_beta-1.62.0.10': [0x1189f0,0x7b7890,0x7b8590,0x7ba390,0x7bb500,0x808860],
    }
    report = {}
    for branch, entries in profiles.items():
        target = Image(root/branch/'eurotrucks2.exe')
        manifest = json.loads((root/branch/'manifest.json').read_text(encoding='utf-8-sig'))
        assert hashlib.sha256(target.path.read_bytes()).hexdigest().upper() == manifest['sha256']
        lines, result = [], {}
        for name, old, new in zip(names, original, entries):
            a, b = reference.flow(old), target.flow(new)
            at, bt = flow_tokens(a), flow_tokens(b)
            result[name] = dict(rva=hex(new), reference_instructions=len(a),
                                target_instructions=len(b), normalized_equal=at == bt)
            lines.append(f'=== {name}: {result[name]} ===')
            for tag,x,y,u,v in difflib.SequenceMatcher(None,at,bt,autojunk=False).get_opcodes():
                if tag == 'equal': continue
                lines.append(tag)
                for i in a[x:y]: lines.append(f'REF {i.address:x} {i.mnemonic} {i.op_str}')
                for i in b[u:v]: lines.append(f'NEW {i.address:x} {i.mnemonic} {i.op_str}')
        report[branch] = result
        (root/f'{branch}-flow-diff.txt').write_text('\n'.join(lines))
        print(branch + ': ' + ', '.join(f'{k}={v["normalized_equal"]}' for k,v in result.items()))
    (root/'flow-comparison.json').write_text(json.dumps(report, indent=2))

if __name__ == '__main__':
    discover() if '--discover' in sys.argv else verify()
