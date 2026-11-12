"""Per-game configuration (game.json in the project root).

{
  "title":    window title / messages,
  "exe_name": name of the built executable,
  "modules":  program modules to recompile.  The first one is the main
              program the runtime starts.  Each entry:
      "file":          module file name on the disc, or "auto" for the
                       application named in the disc label (main program)
      "base":          fixed load address (hex string).  Every module needs
                       its own non-overlapping range below 0x140000 (the heap
                       starts there); the runtime loads it at this address.
      "extra_entries": code addresses the analyzer cannot find on its own
                       (hex strings, absolute, i.e. base + offset)
      "a5_table":      true / false / "auto": the program uses an MPW-style
                       A5 jump table ("jsr N(a5)" calls)
}
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _hex(v):
    return int(v, 16) if isinstance(v, str) else int(v)


def load():
    with open(os.path.join(ROOT, 'game.json')) as f:
        cfg = json.load(f)
    cfg.setdefault('title', 'CD-i Game')
    cfg.setdefault('exe_name', 'cdigame')
    mods = []
    for m in cfg.get('modules', []):
        mods.append({
            'file': m.get('file', 'auto'),
            'base': _hex(m.get('base', '0x100000')),
            'extra_entries': [_hex(x) for x in m.get('extra_entries', [])],
            'a5_table': m.get('a5_table', 'auto'),
        })
    if not mods:
        mods.append({'file': 'auto', 'base': 0x100000, 'extra_entries': [], 'a5_table': 'auto'})
    cfg['modules'] = mods
    return cfg


def app_name():
    """main program name recorded by tools/cdi_extract.py from the disc label"""
    p = os.path.join(ROOT, 'extracted', '_app.txt')
    if os.path.exists(p):
        return open(p).read().strip()
    return None


def module_file(m):
    f = m['file']
    if f == 'auto':
        f = app_name()
        if not f:
            raise SystemExit('cannot determine the main program: run tools/cdi_extract.py first')
    return f
