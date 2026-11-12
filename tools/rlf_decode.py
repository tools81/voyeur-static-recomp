"""Offline decoder for Voyeur's real-time video clips (RTVA.rtf).

The file is a sequence of clips.  Each clip is:
  * header data sectors (channel 0, submode 08/09).  First word: number of
    header sectors.  +0x24 frame count, +0x26 frame rate, +0x6A a 128-entry
    RGB colour table (studio-range bytes), +0x36A one byte per frame: the
    number of video sectors that frame occupies;
  * an optional DYUV background still (channel 0, coding 05);
  * the frames: RL7 scanlines (channel 0, coding 0x90) packed back to back,
    each frame starting on a sector boundary.
Audio sectors (channel 1) are interleaved and skipped.

Used by tools/hd_pack.py to build the AI upscaling pack.
"""
import struct

RAW = 2352
W, H = 384, 240


def to_host(v):
    return max(0, min(255, (v - 16) * 255 // 219))


class Disc:
    def __init__(self, path):
        self.f = open(path, 'rb')

    def sector(self, lsn):
        self.f.seek(lsn * RAW)
        return self.f.read(RAW)


def form_payload(s):
    sub = s[18]
    if sub & 0x20:
        return s[24:24 + 2324]
    return s[24:24 + 2048]


DYUV_Q = [0, 1, 4, 9, 16, 27, 44, 79, 128, 177, 212, 229, 240, 247, 252, 255]


def yuv_to_rgb(y, u, v):
    b = y + (u - 128) * 1733 // 1000
    r = y + (v - 128) * 1371 // 1000
    r = max(0, min(255, r)); b = max(0, min(255, b))
    g = (y * 1000 - 299 * r - 114 * b) // 587
    return r, max(0, min(255, g)), b


def decode_dyuv(data, w=W, h=H):
    """returns list of rows of (r,g,b) host colours"""
    rows = []
    p = 0
    for _ in range(h):
        y, u, v = 128, 128, 128      # the game starts each line at 80 80 80
        pu, pv = u, v
        row = []
        for x in range(0, w, 2):
            b1, b2 = data[p], data[p + 1]
            p += 2
            nu = (u + DYUV_Q[b1 >> 4]) & 255
            nv = (v + DYUV_Q[b2 >> 4]) & 255
            y0 = (y + DYUV_Q[b1 & 15]) & 255
            y1 = (y0 + DYUV_Q[b2 & 15]) & 255
            c0 = yuv_to_rgb(y0, (pu + nu) // 2, (pv + nv) // 2)
            c1 = yuv_to_rgb(y1, nu, nv)
            row += [tuple(map(to_host, c0)), tuple(map(to_host, c1))]
            pu, pv = nu, nv
            u, v, y = nu, nv, y1
        rows.append(row)
    return rows


def decode_rl7(data, p, w=W):
    """one RL7 line from data[p:]: returns (indices, new p)"""
    out = []
    while len(out) < w and p < len(data):
        b = data[p]; p += 1
        run = 1
        if b & 0x80:
            run = data[p]; p += 1
            if run == 0:
                run = w - len(out)
        out += [b & 0x7F] * min(run, w - len(out))
    return out, p


class Clip:
    pass


def scan_clips(disc, lsn0, nsect):
    """yield Clip objects with header info and sector lists"""
    clips = []
    cur = None
    for i in range(nsect):
        s = disc.sector(lsn0 + i)
        ch, sub, cod = s[17], s[18], s[19]
        if ch != 0:
            continue
        if sub & 0x08 and not (sub & 0x20):           # data sector: header
            if cur is None or cur.vsect or cur.dyuv:
                cur = Clip()
                cur.lsn = lsn0 + i
                cur.hdr = b''
                cur.vsect = []
                cur.dyuv = []
                clips.append(cur)
            cur.hdr += form_payload(s)
        elif cur is not None and sub & 0x02:          # video
            if cod == 0x05:
                cur.dyuv.append(lsn0 + i)
            elif cod == 0x90:
                cur.vsect.append(lsn0 + i)
    for c in clips:
        h = c.hdr
        c.name = h[0x12:0x22].decode('ascii', 'replace')
        c.frames = struct.unpack('>H', h[0x24:0x26])[0]
        c.fps = struct.unpack('>H', h[0x26:0x28])[0]
        c.clut = [tuple(to_host(v) for v in h[0x6A + k * 3:0x6A + k * 3 + 3]) for k in range(128)]
        c.fsect = list(h[0x36A:0x36A + c.frames])
    return clips


def clip_frames(disc, clip):
    """yield (frame index, list of H rows of RL7 colour indices)"""
    k = 0
    for fi, n in enumerate(clip.fsect):
        data = b''.join(form_payload(disc.sector(l)) for l in clip.vsect[k:k + n])
        k += n
        rows = []
        p = 0
        for _ in range(H):
            row, p = decode_rl7(data, p)
            if len(row) < W:
                row += [0] * (W - len(row))
            rows.append(row)
        yield fi, rows


def clip_background(disc, clip):
    if len(clip.dyuv) * 2324 < W * H:
        return None
    data = b''.join(form_payload(disc.sector(l)) for l in clip.dyuv)
    return decode_dyuv(data)
