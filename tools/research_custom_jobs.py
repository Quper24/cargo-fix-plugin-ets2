"""Offline custom-job research; never installs hooks or edits game/save files.

Uses the project's saved EXE snapshots and Capstone 5. Output is diagnostic
evidence, NOT a runtime address profile or a claim of gameplay compatibility.
"""
import argparse
import hashlib
import json
from pathlib import Path

from compare_executables import Image, ROOT, flow_tokens
from capstone.x86 import X86_OP_IMM


REFERENCE = 'ets2-public-1.62.1.0'
# Existing research establishes only the two root addresses per snapshot.
SNAPSHOTS = {
    REFERENCE: ('eurotrucks2.exe', 0x7BB610, 0x7B8B10),
    'ets2-public-1.62.3.3': ('eurotrucks2.exe', 0x7BBA10, 0x7B8F10),
    'ats-public-1.62.2.2': ('amtrucks.exe', 0x7BC750, 0x7B9C50),
    'ats-public-1.62.1.1': ('amtrucks.exe', 0x7BB2A0, 0x7B87A0),
    'ats-oculus-1.62.1.1001': ('amtrucks.exe', 0x7BB030, 0x7B8530),
    'ats-oculus-1.62.1.1001-build-26457597-staged':
        ('amtrucks.exe', 0x7BCAA0, 0x7B9FA0),
    'public_beta-1.62.0.10': ('eurotrucks2.exe', 0x7BA390, 0x7B7890),
    'temporary_1_61-1.61.1.1': ('eurotrucks2.exe', 0x7CF780, 0x7CCCD0),
    'oculus-1.61.1.1009': ('eurotrucks2.exe', 0x7CFDD0, 0x7CD320),
}
# (parent role, reference call instruction). Children are resolved by the
# instruction's position in equal normalized reachable code, never RVA deltas.
# Role names express research hypotheses; they are not recovered debug symbols.
EDGES = {
    'base_slot': ('base_fill', 0x7B8C8F),
    'owned_candidates': ('owned_generator', 0x7BB7AA),
    'owned_fixed': ('owned_generator', 0x7BB8E1),
    'destinations': ('owned_generator', 0x7BBA43),
    'company_location': ('owned_fixed', 0x7BB234),
    'route_evaluate': ('owned_fixed', 0x7BB333),
    'owned_factory': ('owned_fixed', 0x7BB114),
    'base_factory': ('base_fill', 0x7B8BC6),
    'base_candidates': ('base_slot', 0x7B9ACC),
    'unit_token': ('company_location', 0x7BCC2E),
}
# Independent call paths into shared helpers are checked as well.
SHARED_CALLS = [
    ('owned_fixed', 0x7BB23F, 'company_location'),
    ('base_slot', 0x7B9FA4, 'destinations'),
    ('base_slot', 0x7BA620, 'company_location'),
    ('base_slot', 0x7BA62B, 'company_location'),
    ('base_slot', 0x7BA653, 'route_evaluate'),
]


def direct_target(instruction):
    if instruction.mnemonic != 'call' or instruction.operands[0].type != X86_OP_IMM:
        raise ValueError(f'Expected direct call at {instruction.address:x}')
    return instruction.operands[0].imm


def corresponding_call(reference_code, target_code, reference_site):
    if flow_tokens(reference_code) != flow_tokens(target_code):
        raise ValueError('Parent reachable code differs; call correspondence unproven')
    index = next(n for n, i in enumerate(reference_code) if i.address == reference_site)
    direct_target(reference_code[index])
    return direct_target(target_code[index])


def dump_function(start, code):
    lines = [f'# RVA {start:x}; role name is a hypothesis, not a native symbol.']
    for i in code:
        lines.append(f'{i.address:08x} {i.bytes.hex():32s} {i.mnemonic} {i.op_str}')
    return '\n'.join(lines) + '\n'


def analyze(snapshot_root, output, selected):
    reference_path = snapshot_root / REFERENCE / SNAPSHOTS[REFERENCE][0]
    reference = Image(reference_path)
    reference_rvas = dict(zip(('owned_generator', 'base_fill'), SNAPSHOTS[REFERENCE][1:]))
    reference_code = {role: reference.flow(rva) for role, rva in reference_rvas.items()}
    for role, (parent, site) in EDGES.items():
        parent_code = reference_code[parent]
        rva = direct_target(next(i for i in parent_code if i.address == site))
        if rva not in reference.functions:
            raise ValueError(f'{role}: reference target has no unwind function entry')
        reference_rvas[role] = rva
        reference_code[role] = reference.flow(rva)

    results = {}
    for name in selected:
        filename, generator, fill = SNAPSHOTS[name]
        path = snapshot_root / name / filename
        if not path.exists():
            results[name] = {'status': 'missing_snapshot'}
            continue
        image = Image(path)
        folder = output / name
        folder.mkdir(parents=True, exist_ok=True)
        resolved = {'owned_generator': generator, 'base_fill': fill}
        code = {}
        roles = {}
        for role in reference_rvas:
            try:
                if role in EDGES:
                    parent, site = EDGES[role]
                    resolved[role] = corresponding_call(reference_code[parent], code[parent], site)
                rva = resolved[role]
                if rva not in image.functions:
                    raise ValueError('Target has no unwind function entry')
                code[role] = image.flow(rva)
                equal = flow_tokens(reference_code[role]) == flow_tokens(code[role])
                calls = [dict(site=hex(i.address), target=hex(direct_target(i)))
                         for i in code[role] if i.mnemonic == 'call'
                         and i.operands[0].type == X86_OP_IMM]
                roles[role] = dict(rva=hex(rva), instructions=len(code[role]),
                                   normalized_equal=equal, direct_calls=calls)
                (folder / f'{role}.asm.txt').write_text(
                    dump_function(rva, code[role]), encoding='utf-8')
            except (KeyError, ValueError, StopIteration) as error:
                roles[role] = {'status': 'unresolved', 'reason': str(error)}
        shared = []
        for parent, site, child in SHARED_CALLS:
            try:
                target = corresponding_call(reference_code[parent], code[parent], site)
                shared.append(dict(parent=parent, child=child, target=hex(target),
                                   matches=target == resolved[child]))
            except (KeyError, ValueError, StopIteration) as error:
                shared.append(dict(parent=parent, child=child, status='unresolved', reason=str(error)))
        results[name] = dict(sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                             roles=roles, shared_calls=shared)
        equal_count = sum(r.get('normalized_equal', False) for r in roles.values())
        print(f'{name}: {equal_count}/{len(roles)} roles match reference; '
              f'{sum(c.get("matches", False) for c in shared)}/{len(shared)} shared calls verified')
    report = dict(reference=REFERENCE,
                  limitation='Static code correspondence only; does not validate ABI, '
                             'all callees, data, arbitrary jobs, or gameplay.', snapshots=results)
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--snapshot-root', type=Path, default=ROOT / 'diagnostics/executables')
    parser.add_argument('--output', type=Path, default=ROOT / 'build-dev-research/custom-jobs')
    parser.add_argument('--snapshot', choices=list(SNAPSHOTS), action='append')
    parser.add_argument('--emit-catalog-profiles', type=Path,
                        help='Explicitly generate exact-snapshot offline catalog address fixtures')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = analyze(args.snapshot_root, args.output, args.snapshot or list(SNAPSHOTS))
    if args.emit_catalog_profiles:
        lines = ['// Generated by tools/research_custom_jobs.py --emit-catalog-profiles.',
                 '// Offline address fixtures only. Never used to gate runtime compatibility.']
        for name, result in report['snapshots'].items():
            roles = result.get('roles', {})
            if not roles or not all(r.get('normalized_equal') for r in roles.values()):
                continue
            if not all(c.get('matches') for c in result['shared_calls']):
                continue
            image = Image(args.snapshot_root / name / SNAPSHOTS[name][0])
            token_rva = int(roles['unit_token']['rva'], 16)
            # The native registered-unit branch indexes an array of token pools.
            table = [i for i in image.flow(token_rva)
                     if i.mnemonic == 'lea' and i.op_str.startswith('rdx, [rip +')]
            if len(table) != 1:
                raise ValueError(f'{name}: ambiguous token pool reference')
            ins = table[0]
            pool = ins.address + ins.size + ins.disp
            guard = ','.join(f'0x{v:02x}' for v in ins.bytes)
            lines.append('{"%s", "%s", 0x%x, 0x%x, 0x%x, 0x%x, {%s}},' % (
                name, result['sha256'], int(roles['owned_generator']['rva'],16),
                int(roles['base_fill']['rva'],16), pool, ins.address, guard))
        args.emit_catalog_profiles.write_text('\n'.join(lines)+'\n', encoding='utf-8')


if __name__ == '__main__':
    main()
