"""portgen.py: write a copy of native/ for another game build.

  python -I tools/portgen.py port/b2019-0223.json build/port-b2019-0223

native/ stays the single source (the supported build). The port file lists, for the other build: every game address the sources use (new -> old), the byte arrays
whose bytes differ there (expected code bytes with embedded addresses / call displacements), files and values that only look like addresses (colours).
In code (not in comments and strings) every in-image hex literal must be in the table, else this fails: a literal added by a later change cannot slip through.
"""
import json, os, re, shutil, sys

BS = chr(92)
LO, HI = 0x400000, 0x1300000
lit_re = re.compile(r'0[xX]([0-9A-Fa-f]{6,8})([uUlL]*)(?![0-9A-Za-z_])')
arr_re = re.compile(r'\{\s*((?:0[xX][0-9A-Fa-f]{2}u?\s*,\s*){3,}0[xX][0-9A-Fa-f]{2}u?)\s*\}')


def code_spans(src):
    """yield (start, end) of the stretches of src that are code (no comments, no string / char literals)"""
    i = 0
    n = len(src)
    start = 0
    while i < n:
        c = src[i]
        if src.startswith('/*', i) or src.startswith('//', i) or c == '"' or c == "'":
            yield start, i
            if src.startswith('/*', i):
                j = src.find('*/', i + 2)
                j = n if j < 0 else j + 2
            elif src.startswith('//', i):
                j = src.find('\n', i)
                j = n if j < 0 else j
            else:
                q = c
                j = i + 1
                while j < n and src[j] != q:
                    j += 2 if src[j] == BS else 1
                j += 1
            i = j
            start = i
        else:
            i += 1
    yield start, n


def convert(name, src, port, errors):
    addr = {int(k, 16): int(v, 16) for k, v in port['addr'].items()}
    ign_vals = set(int(x, 16) for x in port.get('ignore_values', []))
    out = []
    pos = 0
    for a, b in code_spans(src):
        out.append(src[pos:a])
        seg = src[a:b]

        def sub(m):
            v = int(m.group(1), 16)
            if not (LO <= v < HI) or v in ign_vals:
                return m.group()
            if v in addr:
                width = len(m.group(1))
                nv = addr[v]
                return '0x' + ('%0*X' % (width, nv) if nv < (1 << (4 * width)) else '%X' % nv) + m.group(2)
            line = src.count('\n', 0, a + m.start()) + 1
            errors.append('%s:%d: %s is not in the port table' % (name, line, m.group()))
            return m.group()
        out.append(lit_re.sub(sub, seg))
        pos = b
    out.append(src[pos:])
    s = ''.join(out)
    if True:   # every byte array (the mod's own stubs too: lt.c writes `mov ecx,[0x11E1B9C]`): data addresses inside are translated
        # test files hold raw copies of game code bytes: the data addresses inside them (mov ecx,[0x116B3A4] = 8B 0D A4 B3 16 01) are translated the same way
        def barr(m):
            bs = bytearray(int(x.strip().rstrip('uU'), 16) for x in m.group(1).split(','))
            if bytes(bs).hex() in {a['new'] for a in port.get('arrays', []) if a['file'] == name}:
                return m.group()   # (an exact site array: translated below)
            ch = 0
            for i in range(len(bs) - 3):
                v = int.from_bytes(bs[i:i + 4], 'little')
                if 0x1100000 <= v < HI and v in addr and v not in ign_vals:
                    bs[i:i + 4] = addr[v].to_bytes(4, 'little')
                    ch += 1
            if not ch:
                return m.group()
            return '{' + ', '.join('0x%02X' % x for x in bs) + '}'
        s = arr_re.sub(barr, s)
    # byte arrays
    for ar in port.get('arrays', []):
        if ar['file'] != name:
            continue
        new = bytes.fromhex(ar['new'])
        old = bytes.fromhex(ar['old'])
        done = False
        for m in arr_re.finditer(s):
            bs = bytes(int(x.strip().rstrip('uU'), 16) for x in m.group(1).split(','))
            ln = s.count('\n', 0, m.start()) + 1
            if bs == new and abs(ln - ar['line']) <= 1 and not done:
                txt = ', '.join('0x%02X' % x for x in old)
                s = s[:m.start()] + '{' + txt + '}' + s[m.end():]
                done = True
                break
        if not done:
            errors.append('%s:%d: byte array %s not found for the port table' % (name, ar['line'], ar['new'][:24]))
    return s


def main():
    port = json.load(open(sys.argv[1], encoding='utf-8'))
    out = sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    nat = os.path.join(here, '..', 'native')
    dst = os.path.join(out, 'native')
    if os.path.exists(dst):
        shutil.rmtree(dst)
    os.makedirs(dst)
    errors = []
    ign_files = set(port.get('ignore_files', []))
    nconv = 0
    for fn in sorted(os.listdir(nat)):
        p = os.path.join(nat, fn)
        if os.path.isdir(p):
            continue
        if fn.endswith(('.c', '.h', '.inc')) and fn not in ign_files:
            src = open(p, encoding='utf-8', errors='surrogateescape', newline='').read()
            new = convert(fn, src, port, errors)
            if new != src:
                nconv += 1
            open(os.path.join(dst, fn), 'w', encoding='utf-8', errors='surrogateescape', newline='').write(new)
        else:
            shutil.copyfile(p, os.path.join(dst, fn))
    b = port['build']
    hdr = ['/* GENERATED by tools/portgen.py from %s */' % os.path.basename(sys.argv[1]),
           '#define R1_BUILD_ID "%s"' % b['id'], '#define R1_BUILD_KIND "%s"' % b['kind'], '#define R1_BUILD_LABEL "%s"' % b['label'],
           '#define R1_EXE_SHA256 "%s"' % b['exe_sha256'], '#define R1_NET_TAG_SHA "%s"' % b['net_tag_sha'], '']
    open(os.path.join(dst, 'r1_build_port.h'), 'w', newline=chr(10)).write(chr(10).join(hdr))
    if '--tests' in sys.argv:
        # the offline tests against the generated tree: their addresses are rewritten the same way (their byte expectations are checked against the old image)
        tsrc = os.path.join(here, '..', 'tests')
        tdst = os.path.join(out, 'tests')
        if os.path.exists(tdst):
            shutil.rmtree(tdst)
        shutil.copytree(tsrc, tdst, ignore=shutil.ignore_patterns('build'))
        terr = []
        tport = dict(port)
        tport['addr'] = dict(port['addr'], **port.get('tests_addr', {}))
        tport['arrays'] = port.get('tests_arrays', [])
        for fn in sorted(os.listdir(tdst)):
            p = os.path.join(tdst, fn)
            if os.path.isfile(p) and fn.endswith(('.c', '.h')):
                src = open(p, encoding='utf-8', errors='surrogateescape', newline='').read()
                open(p, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(convert('tests/' + fn, src, tport, terr))
        print('tests: %d literal problems (not in the table)' % len(terr))
        if terr and '--verbose' in sys.argv:
            print(chr(10).join(terr))
    if errors:
        print('\n'.join(errors))
        print('FAILED: %d problems' % len(errors))
        sys.exit(1)
    print('port %s: %d files rewritten in %s' % (port['build']['id'], nconv, dst))


main()
