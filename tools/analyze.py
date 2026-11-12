"""Control-flow discovery for OS-9 program modules.

Produces a set of decoded instructions reachable from the entry point and
other code pointers, plus a listing for inspection.
"""
import struct
import sys
from m68k import decode, DecodeError, CC
from os9mod import Module

MODULE_BASE = 0x00100000   # default; per-module bases come from game.json

# os9 calls that never return
NORETURN_OS9 = {0x06, 0x1e}  # F$Exit, F$RTE


class Analysis:
    def __init__(self, mod, extra_entries=(), extra_tables=(), a5=True, symbols=None):
        self.mod = mod
        self.symbols = symbols or {}
        self.base = mod.base
        self.code_lo = self.base + 0x48
        self.code_hi = self.base + mod.idataoff
        self.insns = {}
        self.funcs = set()
        self.labels = set()
        self.errors = {}
        self.jumptables = {}   # insn addr -> list of targets
        self.indirect = set()  # unresolved indirect jumps/calls
        self.code_ptrs = set()
        self.extra_tables = list(extra_tables)
        if a5 == 'auto':
            a5 = uses_a5_table(mod)
        self.uses_a5 = bool(a5)
        self.a5table = a5_jumptable(mod) if a5 else {}
        self.pcrel_calls = {}  # jsr/jmp d8(pc,dn.l) with dn loaded by move.l #imm -> target
        self.a6stubs = a6_jumpstubs(mod)  # jsr d(a6) into a static 'jmp abs.l' table
        work = [self.base + mod.execoff]
        for t in set(self.a6stubs.values()):
            work.append(t)
            self.funcs.add(t)
        # symbol-table function starts (Microware .stb next to the module)
        for a in sorted(self.symbols):
            if self.in_code(a) and not a & 1:
                work.append(a)
                self.funcs.add(a)
        # code pointers stored in initialized data (code-relative irefs)
        for t in data_code_ptrs(mod):
            if self.in_code(t):
                self.code_ptrs.add(t)
        for t in set(self.a5table.values()):
            work.append(t)
            self.funcs.add(t)
        self.funcs.add(self.base + mod.execoff)
        if mod.excptoff:
            work.append(self.base + mod.excptoff)
            self.funcs.add(self.base + mod.excptoff)
        for e in extra_entries:
            work.append(e)
            self.funcs.add(e)
        self.work = work
        self.run()
        # function pointers taken with lea/pea x(pc): accept those that start
        # with a function prologue (link / movem to the stack)
        while True:
            added = False
            for p in sorted(self.code_ptrs):
                if p in self.insns or p in self.errors or p & 1:
                    continue
                try:
                    i = decode(self.mod.data, self.base, p)
                except DecodeError:
                    continue
                if i.op == 'link' or (i.op == 'movem' and i.dst is not None and i.dst.kind == 'predec'):
                    self.add(p, func=True)
                    added = True
            if not added:
                break
            self.run()
        self.mark_unrolled()
        self.names = macsbug_names(mod, self.funcs)
        for a, n in self.symbols.items():
            if a in self.funcs:
                self.names[a] = n

    def mark_unrolled(self, min_run=8):
        """Unrolled copy/fill loops (Duff's device) are entered with a
        computed jsr/jmp at any instruction of the run: make each one an
        entry point."""
        d = self.mod.data
        addrs = sorted(self.insns)
        k = 0
        while k < len(addrs):
            a = addrs[k]
            i = self.insns[a]
            raw = d[a - self.base:a - self.base + i.length]
            j = k + 1
            while j < len(addrs) and addrs[j] == addrs[j - 1] + i.length and                     d[addrs[j] - self.base:addrs[j] - self.base + i.length] == raw:
                j += 1
            if j - k >= min_run and i.op in ('move', 'clr', 'movem'):
                for x in addrs[k:j + 1]:
                    if x in self.insns:
                        self.labels.add(x)
            k = j

    def in_code(self, a):
        return self.code_lo <= a < self.code_hi

    def add(self, a, func=False):
        if not self.in_code(a) or a & 1:
            return
        if func:
            self.funcs.add(a)
        self.labels.add(a)
        if a not in self.insns:
            self.work.append(a)

    def run(self):
        while self.work:
            a = self.work.pop()
            self.trace(a)

    def trace(self, a):
        while True:
            if a in self.insns or a in self.errors:
                return
            if not self.in_code(a):
                self.errors[a] = 'outside code'
                return
            try:
                i = decode(self.mod.data, self.base, a)
            except DecodeError as e:
                self.errors[a] = str(e)
                return
            self.insns[a] = i
            nxt = a + i.length
            op = i.op
            # code pointers via lea/pea pc-relative
            for ea in (i.src, i.dst):
                if ea is not None and ea.kind == 'pcdisp' and op in ('lea', 'pea'):
                    if self.in_code(ea.addr):
                        self.code_ptrs.add(ea.addr)
            if op in ('bra',):
                self.add(i.target)
                return
            if op == 'b' or op == 'db':
                self.add(i.target)
                self.labels.add(nxt)
                a = nxt
                continue
            if op == 'bsr':
                self.add(i.target, func=True)
                self.labels.add(nxt)
                a = nxt
                continue
            if op in ('jsr', 'jmp'):
                ea = i.src
                if ea.kind == 'pcdisp':
                    self.add(ea.addr, func=(op == 'jsr'))
                elif ea.kind == 'disp' and ea.reg == 5 and ea.disp in self.a5table:
                    ea.addr = self.a5table[ea.disp]
                    self.add(ea.addr, func=True)
                elif ea.kind == 'disp' and ea.reg == 6 and ea.disp in self.a6stubs:
                    ea.addr = self.a6stubs[ea.disp]
                    self.add(ea.addr, func=(op == 'jsr'))
                elif ea.kind == 'pcindex':
                    t = self.pcrel_const(i)
                    if t is not None:
                        self.pcrel_calls[a] = t
                        self.add(t, func=(op == 'jsr'))
                    else:
                        self.jumptable(i)
                elif ea.kind == 'index' and self.offset_table(i, op == 'jmp'):
                    pass
                else:
                    self.indirect.add(a)
                if op == 'jmp':
                    return
                self.labels.add(nxt)
                a = nxt
                continue
            if op in ('rts', 'rte', 'rtr', 'illegal', 'stop', 'reset'):
                return
            if op == 'os9':
                self.labels.add(nxt)
                if i.extra == 0x09:
                    # F$Icpt: the intercept routine is usually loaded with lea x(pc),a0
                    for back in range(2, 24, 2):
                        p = self.insns.get(a - back)
                        if p is not None and p.op == 'lea' and p.dst.reg == 0 and p.src.kind == 'pcdisp':
                            self.add(p.src.addr, func=True)
                            break
                if i.extra in NORETURN_OS9:
                    return
            if op == 'trap':
                self.labels.add(nxt)
            if op in ('linea', 'linef'):
                return
            a = nxt

    def pcrel_const(self, i):
        """Microware large-model call: move.l #imm,dN ; jsr d8(pc,dN.l)"""
        ea = i.src
        if not ea.xlong:
            return None
        for back in (6, 2):
            p = self.insns.get(i.addr - back)
            if p is None or p.addr + p.length != i.addr:
                continue
            if p.src is None or p.src.kind != 'imm' or p.size != 'l':
                continue
            if ea.xreg < 8 and p.op in ('move', 'moveq') and p.dst.kind == 'dn' and p.dst.reg == ea.xreg:
                return (ea.addr + p.src.value) & 0xffffffff
            if ea.xreg >= 8 and p.op == 'movea' and p.dst.kind == 'an' and p.dst.reg == ea.xreg - 8:
                return (ea.addr + p.src.value) & 0xffffffff
        return None

    def offset_table(self, i, is_jmp):
        """Hand-written dispatch through a table of long offsets relative to
        the table itself:
              and.w #mask,dX ; lea T(pc),aN ; move.l 0(aN,dX.w),dY ; jsr 0(aN,dY.l)
           The entry count comes from the mask (or an explicit cmp bound)."""
        ea = i.src
        if ea.disp != 0 or ea.xreg is None or ea.xreg >= 8:
            return False
        an, dy = ea.reg, ea.xreg
        mv = None
        lea = None
        a = i.addr
        dx = None
        mask = None
        for _ in range(8):
            prv = [x for x in (a - 2, a - 4, a - 6) if x in self.insns and
                   self.insns[x].addr + self.insns[x].length == a]
            if not prv:
                break
            a = prv[0]
            p = self.insns[a]
            if mv is None and p.op == 'move' and p.size == 'l' and p.src.kind == 'index' and                     p.src.reg == an and p.src.disp == 0 and p.dst.kind == 'dn' and p.dst.reg == dy:
                mv = p
                dx = p.src.xreg
            elif mv is not None and lea is None and p.op == 'lea' and p.dst.reg == an and p.src.kind == 'pcdisp':
                lea = p
            elif mv is not None and p.op in ('and', 'andi') and p.src.kind == 'imm' and                     p.dst.kind == 'dn' and p.dst.reg == dx:
                mask = p.src.value
                break
        if mv is None or lea is None or mask is None or mask > 0x3fc or mask & 3:
            return False
        tbl = lea.src.addr
        targets = []
        for k in range(mask // 4 + 1):
            if (k * 4) & ~mask:
                continue
            t = (tbl + self.mod.l(tbl - self.base + 4 * k)) & 0xffffffff
            if not self.in_code(t) or t & 1:
                return False
            targets.append(t)
        for t in targets:
            self.add(t, func=not is_jmp)
        if is_jmp:
            self.jumptables[i.addr] = targets
        self.tables_data = getattr(self, 'tables_data', [])
        self.tables_data.append((tbl, 4 * (mask // 4 + 1)))
        return True

    def jumptable(self, i):
        """Compiler switch idiom:
              cmpi.x #N,d0 ; b(hi|gt) default ; ... add d0,d0
              move.w T(pc,d0),d0 ; jmp T-2(pc,d0.w)
           Table T holds N+1 word offsets relative to the jmp's ea base."""
        prev = self.insns.get(i.addr - 4)
        if prev is None or prev.op != 'move' or prev.src.kind != 'pcindex':
            self.indirect.add(i.addr)
            return
        tbl = prev.src.addr
        base = i.src.addr
        # find the bound
        n = None
        a = prev.addr
        for _ in range(8):
            cands = [x for x in (a - 2, a - 4, a - 6, a - 8) if x in self.insns and
                     self.insns[x].addr + self.insns[x].length == a]
            if not cands:
                break
            a = cands[0]
            p = self.insns[a]
            if p.op == 'cmpi':
                n = p.src.value
                if p.size == 'b' and n & 0x80: n = None
                break
            if p.op == 'cmp' and p.src.kind == 'dn':
                # Microware C: moveq #N,dR ; cmp.l dR,dX ; bhi default
                q = self.insns.get(a - 2)
                if q is not None and q.op == 'moveq' and q.dst.reg == p.src.reg:
                    n = q.src.value & 0xffffffff
                    if n & 0x80000000: n = None
                break
        if n is None or n > 512:
            self.indirect.add(i.addr)
            return
        targets = []
        for k in range(n + 1):
            off = self.mod.w(tbl - self.base + 2 * k)
            if off & 0x8000: off -= 0x10000
            t = base + off
            targets.append(t)
            self.add(t)
        self.jumptables[i.addr] = targets
        self.tables_data = getattr(self, 'tables_data', [])
        self.tables_data.append((tbl, 2 * (n + 1)))


def a5_jumptable(mod):
    """MPW-style jump table: idata = [tblsize, a5off, entries...]; entries are
    (dc.w seg, jmp abs.l) copied to a5+0x20.  Returns {a5 displacement: target}."""
    doff, b = mod.idata()
    tsize, a5off = struct.unpack('>II', b[0:8])
    tbl = {}
    for k in range((tsize - 0x20) // 8):
        e = b[8 + k * 8: 16 + k * 8]
        if len(e) < 8 or e[2:4] != bytes([0x4e, 0xf9]):
            continue
        tbl[0x20 + k * 8 + 2] = mod.base + struct.unpack('>I', e[4:8])[0]
    return tbl


def a6_jumpstubs(mod):
    """Microware C programs (Voyeur) call through a table of 'jmp abs.l'
    stubs in static storage (the linker's _jmptbl), addressed as d(a6) with
    a6 = data + 0x8000.  The stubs live in idata and each operand has a
    code-relative iref.  Returns {a6 displacement: target}."""
    try:
        doff, b = mod.idata()
        code_refs, _ = mod.irefs()
    except Exception:
        return {}
    stubs = {}
    for r in code_refs:
        o = r - 2 - doff
        if 0 <= o and o + 6 <= len(b) and b[o:o + 2] == bytes([0x4e, 0xf9]):
            disp = r - 2 - 0x8000
            t = struct.unpack('>I', b[o + 2:o + 6])[0]
            # 'jmp 0' placeholders are patched at run time
            if -0x8000 <= disp < 0x8000 and t >= 0x48:
                stubs[disp] = mod.base + t
    return stubs


def data_code_ptrs(mod):
    """targets of all code-relative irefs (function pointers in data)"""
    try:
        doff, b = mod.idata()
        code_refs, _ = mod.irefs()
    except Exception:
        return []
    out = []
    for r in code_refs:
        o = r - doff
        if 0 <= o and o + 4 <= len(b):
            out.append(mod.base + struct.unpack('>I', b[o:o + 4])[0])
    return out


def stb_symbols(path, base, code_size):
    """Microware linker symbol module (<module>.stb): 10-byte entries
    (value.l, type.b, pad.b, name offset.l).  Returns {address: name} for
    code symbols (module offsets below etext)."""
    import os
    if not os.path.exists(path):
        return {}
    d = open(path, 'rb').read()
    if d[:2] != bytes([0x4a, 0xfc]):
        return {}
    # after the module name: ... entry table offset.l, entry count.l
    nameoff = struct.unpack('>I', d[12:16])[0]
    name_end = d.index(b'\0', nameoff) + 1
    ents = []
    for k in range(name_end, name_end + 32):
        off, cnt = struct.unpack('>II', d[k:k + 8])
        if k + 8 <= off and 0 < cnt and off + 10 * cnt <= len(d):
            for j in range(cnt):
                v, t, _, no = struct.unpack('>IBBI', d[off + 10 * j:off + 10 * j + 10])
                if no >= len(d):
                    break
                e = d.index(b'\0', no)
                ents.append((v, d[no:e].decode('latin-1')))
            break
    syms = {}
    for v, n in ents:
        if 0x48 <= v < code_size and n not in ('etext', 'btext', 'bname'):
            syms.setdefault(base + v, n)
    return syms


def uses_a5_table(mod):
    """Heuristic: the initialized data is an MPW jump table (every entry a
    'dc.w seg; jmp abs.l' stub) as produced by Apple MPW / compatible
    compilers.  Microware C programs do not have one."""
    try:
        doff, b = mod.idata()
        tsize = struct.unpack('>I', b[0:4])[0]
    except Exception:
        return False
    n = (tsize - 0x20) // 8
    if n < 3 or 8 + n * 8 > len(b):
        return False
    return len(a5_jumptable(mod)) == n


def macsbug_names(mod, funcs):
    """Functions end with rts/jmp followed by a MacsBug symbol: byte 0x80+len, name."""
    import bisect
    d = mod.data
    fl = sorted(funcs)
    names = {}
    for o in range(0x48, mod.idataoff - 2):
        if d[o] & 0x80 and 0x82 <= d[o] <= 0x9f and o >= 2 and d[o-2:o] in (bytes([0x4e, 0x75]), bytes([0x4e, 0xd0]), bytes([0x4e, 0x73])):
            n = d[o] & 0x7f
            nm = d[o + 1:o + 1 + n]
            if all(32 < c < 127 for c in nm):
                a = mod.base + o
                k = bisect.bisect_right(fl, a) - 1
                if k >= 0 and fl[k] not in names:
                    names[fl[k]] = nm.decode()
    return names


def load(path=None, base=None):
    """load a module; defaults to the main program from game.json"""
    if path is None or base is None:
        import config
        m = config.load()['modules'][0]
        path = path or 'extracted/' + config.module_file(m)
        base = base if base is not None else m['base']
    data = open(path, 'rb').read()
    return Module(data, base)


def analyze_module(m):
    """Analysis for a game.json module entry"""
    import config
    path = 'extracted/' + config.module_file(m)
    mod = load(path, m['base'])
    syms = stb_symbols(path + '.stb', m['base'], mod.idataoff)
    return Analysis(mod, extra_entries=m['extra_entries'], a5=m['a5_table'], symbols=syms)


def listing(an, out):
    mod = an.mod
    addrs = sorted(an.insns)
    prev_end = None
    for a in addrs:
        i = an.insns[a]
        if prev_end is not None and a != prev_end:
            out.write('\n; ---- gap %x-%x\n' % (prev_end, a) if a > prev_end else '; ---- overlap\n')
        if a in an.funcs:
            out.write('\n;================ func_%06x %s\n' % (a, an.names.get(a, '')))
        lab = 'L%06x:' % a if a in an.labels else ''
        raw = mod.data[a - an.base:a - an.base + i.length].hex()
        out.write('%-9s %06x  %-20s %s\n' % (lab, a, raw, i))
        prev_end = a + i.length


if __name__ == '__main__':
    # python tools/analyze.py [module index]  -> build/listing_<file>.asm
    import os
    import config
    cfg = config.load()
    idx = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    m = cfg['modules'][idx]
    an = analyze_module(m)
    os.makedirs('build', exist_ok=True)
    out = 'build/listing_%s.asm' % config.module_file(m)
    with open(out, 'w') as f:
        listing(an, f)
    print('listing written to', out, '(A5 jump table: %s)' % an.uses_a5)
    print('insns', len(an.insns), 'funcs', len(an.funcs), 'errors', len(an.errors), 'indirect', len(an.indirect))
    for a, e in sorted(an.errors.items())[:40]:
        print('err %x %s' % (a, e))
    print('jumptables', len(an.jumptables))
    extra = sorted(p for p in an.code_ptrs if p not in an.insns)
    print('lea/pea code ptrs not decoded:', ' '.join('%x' % x for x in extra))
    print('indirect:', ' '.join('%x' % x for x in sorted(an.indirect)))
    covered = sum(i.length for i in an.insns.values())
    print('code bytes covered %d of %d' % (covered, an.code_hi - an.code_lo))
