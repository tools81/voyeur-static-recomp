"""Build the AI-upscaled video pack for voyeur_hd.

    python tools/hd_pack.py                       # whole disc, Real-ESRGAN x4plus, 2x pack
    python tools/hd_pack.py --scale 4             # keep the full 4x output (about 3x the disk space)
    python tools/hd_pack.py --clips 0,60          # only some clips (see --list)
    python tools/hd_pack.py --upscaler lanczos    # no AI (fast; for testing the pipeline)

Requirements: voyeur_hd.exe built (build.bat), Python with numpy, Pillow and
opencv-python, and for the AI step realesrgan-ncnn-vulkan (any Vulkan GPU):
https://github.com/xinntao/Real-ESRGAN/releases (realesrgan-ncnn-vulkan-*-windows.zip),
unzipped to tools/ai/realesrgan/ or given with --realesrgan PATH.

What it does, per clip of RTVA.rtf (the game's video file):
  1. voyeur_hd --hd-extract decodes the frames (384x240 RL7 video with a
     transparent colour key) and the clip's DYUV background still;
  2. transparent areas are filled with the nearest opaque colour (so the
     network does not smear the key colour into edges) and every picture
     is upscaled;
  3. dense optical flow between consecutive frames is computed (OpenCV DIS)
     for 60 fps interpolation of the 12-15 fps video;
  4. results are written to hd_pack/: JPEG pictures, native-resolution
     alpha masks and flow fields as PNG, the per-line hashes, and index.bin.
The runtime recognises a picture on screen by the hashes of its coded lines
(any vertical position, partly hidden lines are fine) and draws the
upscaled version.  Re-running skips clips that are already complete.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import time

import cv2
import numpy as np
from PIL import Image

if getattr(sys, 'frozen', False):
    # hd_pack_builder.exe (PyInstaller) in the release folder, next to voyeur_hd.exe
    ROOT = os.path.dirname(os.path.abspath(sys.executable))
    REALESRGAN_DIR = os.path.join(ROOT, 'tools', 'realesrgan')
    WORK_DIR = os.path.join(ROOT, 'hd_work')
else:
    ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    REALESRGAN_DIR = os.path.join(ROOT, 'tools', 'ai', 'realesrgan')
    WORK_DIR = os.path.join(ROOT, 'build', 'hd_work')
REALESRGAN_URL = ('https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/'
                  'realesrgan-ncnn-vulkan-20220424-windows.zip')
W, H = 384, 240
VERSION = 1


def find_disc():
    """the disc chosen in the installer / first-run dialog (saves/disc_path.txt), else rom/"""
    dirs = []
    saved = os.path.join(ROOT, 'saves', 'disc_path.txt')
    if os.path.exists(saved):
        p = open(saved, encoding='utf-8', errors='replace').readline().strip()
        if os.path.isfile(p):
            return p
        if os.path.isdir(p):
            dirs += [p, os.path.join(p, 'rom')]
    dirs.append(os.path.join(ROOT, 'rom'))
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for ext in ('.cue', '.bin'):
            for n in sorted(os.listdir(d)):
                if n.lower().endswith(ext):
                    return os.path.join(d, n)
    sys.exit('No disc image found. Start the game once and select the disc, or pass --disc PATH.')


def fetch_realesrgan(dest):
    """download realesrgan-ncnn-vulkan (BSD-3-Clause, Xintao Wang) from its official release"""
    import io
    import urllib.request
    import zipfile
    print('downloading Real-ESRGAN (about 45 MB) from github.com/xinntao/Real-ESRGAN ...', flush=True)
    data = urllib.request.urlopen(REALESRGAN_URL, timeout=60).read()
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for m in z.infolist():
            n = m.filename.replace('\\', '/')
            keep = n.endswith('.exe') or n.endswith('.dll') or n.startswith('models/realesrgan-x4plus.') \
                or n.startswith('models/realesr-animevideov3') or n.lower().startswith('readme')
            if keep and not m.is_dir():
                out = os.path.join(dest, *n.split('/'))
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with open(out, 'wb') as f:
                    f.write(z.read(m))


def run_extract(exe, disc, out, clip=None, hashes_only=False):
    cmd = [exe, '--headless']
    if hashes_only:
        cmd.append('--hd-hashes')
    if clip is not None:
        cmd += ['--hd-clip', str(clip)]
    cmd += ['--hd-extract', out, disc]
    tmp = os.path.join(out, '_run')
    os.makedirs(tmp, exist_ok=True)
    shutil.copy(exe, tmp)          # the exe keeps its log/saves next to itself
    subprocess.run([os.path.join(tmp, os.path.basename(exe))] + cmd[1:], cwd=tmp, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    shutil.rmtree(tmp, ignore_errors=True)


def read_clip_txt(path):
    info = {'frames': [], 'bg': None}
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == 'f':
            info['frames'].append(int(p[1]))
        elif p[0] == 'bg':
            info['bg'] = p[1]
        elif p[0] in ('fps', 'lsn'):
            info[p[0]] = int(p[1])
        elif p[0] == 'name':
            info['name'] = p[1] if len(p) > 1 else ''
    return info


def load_rgba(path):
    return np.array(Image.open(path).convert('RGBA'))


def fill_transparent(rgba):
    """RGB with transparent pixels replaced by the nearest opaque colour"""
    rgb, a = rgba[:, :, :3].copy(), rgba[:, :, 3]
    if not (a == 0).any() or not (a > 0).any():
        return rgb
    _, lab = cv2.distanceTransformWithLabels((a == 0).astype(np.uint8), cv2.DIST_L2, 5,
                                             labelType=cv2.DIST_LABEL_PIXEL)
    ys, xs = np.nonzero(a > 0)
    src = np.zeros(lab.max() + 1, np.int64)
    src[lab[ys, xs]] = np.arange(len(ys))
    idx = src[lab]
    return rgb[ys[idx], xs[idx]]


def upscale_dir(src, dst, args):
    os.makedirs(dst, exist_ok=True)
    if args.upscaler == 'lanczos':
        for n in os.listdir(src):
            im = cv2.imread(os.path.join(src, n))
            cv2.imwrite(os.path.join(dst, n), cv2.resize(im, (W * args.scale, H * args.scale),
                                                         interpolation=cv2.INTER_LANCZOS4))
        return
    exe = args.realesrgan
    model = args.model
    s = 4
    cmd = [exe, '-i', src, '-o', dst, '-n', model, '-s', str(s), '-f', 'png']
    if args.gpu is not None:
        cmd += ['-g', str(args.gpu)]
    subprocess.run(cmd, cwd=os.path.dirname(exe), check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if args.scale != 4:
        for n in os.listdir(dst):
            p = os.path.join(dst, n)
            im = cv2.imread(p)
            cv2.imwrite(p, cv2.resize(im, (W * args.scale, H * args.scale), interpolation=cv2.INTER_AREA))


def flow_png(a_rgb, b_rgb, dis):
    """flow from a to b at half resolution: R,G = 128 + 4*dx, 128 + 4*dy (native pixels)"""
    ga = cv2.cvtColor(a_rgb, cv2.COLOR_RGB2GRAY)
    gb = cv2.cvtColor(b_rgb, cv2.COLOR_RGB2GRAY)
    f = dis.calc(ga, gb, None)
    f = cv2.resize(f, (W // 2, H // 2), interpolation=cv2.INTER_AREA)
    q = np.clip(np.round(f * 4 + 128), 0, 255).astype(np.uint8)
    out = np.zeros((H // 2, W // 2, 3), np.uint8)
    out[:, :, 0] = q[:, :, 0]
    out[:, :, 1] = q[:, :, 1]
    return out


def build_clip(c, args, exe, disc, work, pack):
    cdir = os.path.join(pack, 'img', '%02d' % c)
    done = os.path.join(cdir, 'done')
    if os.path.exists(done) and not args.force:
        return
    t0 = time.time()
    ex = os.path.join(work, 'x')
    shutil.rmtree(ex, ignore_errors=True)
    run_extract(exe, disc, ex, clip=c)
    src = os.path.join(ex, 'clip_%02d' % c)
    info = read_clip_txt(os.path.join(src, 'clip.txt'))
    os.makedirs(cdir, exist_ok=True)
    inp = os.path.join(work, 'in')
    out = os.path.join(work, 'out')
    shutil.rmtree(inp, ignore_errors=True)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(inp)
    natives = {}
    names = ['f_%05d' % i for i in info['frames']]
    if info['bg']:
        names.append('bg')
    for n in names:
        rgba = load_rgba(os.path.join(src, n + '.tga'))
        rgb = fill_transparent(rgba)
        natives[n] = (rgb, rgba[:, :, 3])
        cv2.imwrite(os.path.join(inp, n + '.png'), cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR))
    upscale_dir(inp, out, args)
    dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
    for n in names:
        hi = cv2.imread(os.path.join(out, n + '.png'))
        tag = 'bg' if n == 'bg' else 'f%05d' % int(n[2:])
        cv2.imwrite(os.path.join(cdir, tag + '.jpg'), hi, [cv2.IMWRITE_JPEG_QUALITY, args.quality])
        if n != 'bg':
            a = natives[n][1]
            if (a == 0).any():
                cv2.imwrite(os.path.join(cdir, tag + '.m.png'), a)
            nxt = 'f_%05d' % (int(n[2:]) + 1)
            if nxt in natives:
                cv2.imwrite(os.path.join(cdir, tag + '.v.png'),
                            cv2.cvtColor(flow_png(natives[n][0], natives[nxt][0], dis), cv2.COLOR_RGB2BGR))
    shutil.copy(os.path.join(src, 'lines.bin'), os.path.join(cdir, 'lines.bin'))
    if info['bg']:
        shutil.copy(os.path.join(src, 'bglines.bin'), os.path.join(cdir, 'bglines.bin'))
    with open(os.path.join(cdir, 'clip.txt'), 'w') as f:
        f.write('name %s\nfps %d\nframes %d\nbg %d\nscale %d\nupscaler %s\n' % (
            info.get('name', ''), info.get('fps', 15), len(info['frames']), 1 if info['bg'] else 0,
            args.scale, args.upscaler if args.upscaler != 'ai' else args.model))
    open(done, 'w').close()
    shutil.rmtree(ex, ignore_errors=True)
    print('clip %2d: %4d frames%s in %.0fs' % (c, len(info['frames']), ' + background' if info['bg'] else '',
                                              time.time() - t0), flush=True)


def build_index(pack, hashes_dir, scale):
    """index.bin: clips, pictures and the lookup keys (line hashes that are
    unique on the whole disc, up to 12 per picture spread over its height)"""
    clips = []
    for d in sorted(os.listdir(os.path.join(pack, 'img'))):
        cdir = os.path.join(pack, 'img', d)
        if not os.path.exists(os.path.join(cdir, 'done')):
            continue
        meta = {}
        for line in open(os.path.join(cdir, 'clip.txt')):
            p = line.split()
            if len(p) >= 2 and p[0] in ('fps', 'frames', 'bg'):
                meta[p[0]] = int(p[1])
        clips.append((int(d), meta))
    # global line-hash counts over the whole disc (all clips, all pictures)
    allh = []
    for d in os.listdir(hashes_dir):
        if not d.startswith('clip_'):
            continue
        for fn in ('lines.bin', 'bglines.bin'):
            p = os.path.join(hashes_dir, d, fn)
            if os.path.exists(p):
                allh.append(np.fromfile(p, dtype='<u8'))
    allh = np.concatenate(allh)
    uniq, counts = np.unique(allh, return_counts=True)
    unique_set = uniq[counts == 1]
    images = []
    keys = []
    targets = np.linspace(12, 228, 12).astype(int)
    for ci, (c, meta) in enumerate(clips):
        cdir = os.path.join(pack, 'img', '%02d' % c)
        fl = np.fromfile(os.path.join(cdir, 'lines.bin'), dtype='<u8').reshape(-1, H)
        pics = [(i, fl[i]) for i in range(fl.shape[0])]
        if meta.get('bg'):
            pics.append((-1, np.fromfile(os.path.join(cdir, 'bglines.bin'), dtype='<u8')))
        for frame, lines in pics:
            img = len(images)
            tag = 'bg' if frame < 0 else 'f%05d' % frame
            flags = 1
            if frame >= 0:
                flags |= 8
                if os.path.exists(os.path.join(cdir, tag + '.m.png')):
                    flags |= 2
                if os.path.exists(os.path.join(cdir, tag + '.v.png')):
                    flags |= 4
            images.append((ci, frame, flags))
            pos = np.minimum(np.searchsorted(unique_set, lines), len(unique_set) - 1)
            ok = unique_set[pos] == lines
            rows = np.nonzero(ok)[0]
            if len(rows) == 0:
                continue
            chosen = set()
            for t in targets:
                chosen.add(int(rows[np.argmin(np.abs(rows - t))]))
            for r in sorted(chosen):
                keys.append((int(lines[r]), img, r))
    keys.sort()
    with open(os.path.join(pack, 'index.bin'), 'wb') as f:
        f.write(b'VHD1')
        f.write(struct.pack('<5I', VERSION, scale, len(clips), len(images), len(keys)))
        first = 0
        for c, meta in clips:
            n = meta.get('frames', 0) + (1 if meta.get('bg') else 0)
            f.write(struct.pack('<4I', c, meta.get('fps', 15), meta.get('frames', 0), first))
            first += n
        for ci, frame, flags in images:
            f.write(struct.pack('<IiI', ci, frame, flags))
        for h, img, r in keys:
            f.write(struct.pack('<QII', h, img, r))
    print('index: %d clips, %d pictures, %d keys' % (len(clips), len(images), len(keys)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', default=os.path.join(ROOT, 'voyeur_hd.exe'))
    ap.add_argument('--disc', default=None)
    ap.add_argument('--out', default=os.path.join(ROOT, 'hd_pack'))
    ap.add_argument('--work', default=WORK_DIR)
    ap.add_argument('--clips', default=None, help='comma-separated clip numbers (default: all)')
    ap.add_argument('--list', action='store_true', help='list the clips and exit')
    ap.add_argument('--scale', type=int, default=2, choices=(2, 3, 4), help='pack resolution (x native)')
    ap.add_argument('--upscaler', default='ai', choices=('ai', 'lanczos'))
    ap.add_argument('--model', default='realesrgan-x4plus',
                    help='realesrgan-x4plus (photographic, default) or realesr-animevideov3 (faster)')
    ap.add_argument('--realesrgan', default=os.path.join(REALESRGAN_DIR, 'realesrgan-ncnn-vulkan.exe'))
    ap.add_argument('--gpu', type=int, default=None)
    ap.add_argument('--quality', type=int, default=92, help='JPEG quality')
    ap.add_argument('--force', action='store_true', help='rebuild clips that are already done')
    args = ap.parse_args()
    if not os.path.exists(args.exe):
        sys.exit('%s not found (run build.bat first, or run this next to voyeur_hd.exe)' % args.exe)
    if args.upscaler == 'ai' and not os.path.exists(args.realesrgan) \
            and args.realesrgan == os.path.join(REALESRGAN_DIR, 'realesrgan-ncnn-vulkan.exe'):
        try:
            fetch_realesrgan(REALESRGAN_DIR)
        except Exception as e:
            print('download failed: %s' % e)
    if args.upscaler == 'ai' and not os.path.exists(args.realesrgan):
        sys.exit('realesrgan-ncnn-vulkan not found at %s\n'
                 'Download it from https://github.com/xinntao/Real-ESRGAN/releases and unzip it to\n'
                 'tools/ai/realesrgan/, or pass --realesrgan PATH (or use --upscaler lanczos).' % args.realesrgan)
    disc = args.disc or find_disc()
    os.makedirs(args.work, exist_ok=True)
    hashes = os.path.join(args.work, 'hashes')
    run_extract(os.path.abspath(args.exe), os.path.abspath(disc), hashes, hashes_only=True)
    allc = sorted(int(d[5:]) for d in os.listdir(hashes) if d.startswith('clip_'))
    if args.list:
        for c in allc:
            i = read_clip_txt(os.path.join(hashes, 'clip_%02d' % c, 'clip.txt'))
            print('%2d  %-16s %5d frames  %2d fps%s' % (c, i.get('name', ''), len(i['frames']), i.get('fps', 0),
                                                       '  + background' if i['bg'] else ''))
        return
    sel = [int(x) for x in args.clips.split(',')] if args.clips else allc
    os.makedirs(args.out, exist_ok=True)
    for c in sel:
        build_clip(c, args, os.path.abspath(args.exe), os.path.abspath(disc), os.path.abspath(args.work), args.out)
    build_index(args.out, hashes, args.scale)


if __name__ == '__main__':
    main()
