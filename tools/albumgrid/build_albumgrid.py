#!/usr/bin/env python3
"""Build <root>/.rockbox/albumgrid.dat for the albumgrid plugin."""
import os, re, struct, sys, unicodedata

TILE, REC = 70, 10216
EXTS = ('.flac', '.mp3', '.m4a', '.ogg', '.opus', '.wav')
COVERS = ['cover.jpg', 'cover.png', 'folder.jpg', 'folder.png', 'front.jpg', 'front.png']


def nfc(s):
    return unicodedata.normalize('NFC', s)


def fixed(s, n):
    b = nfc(s).encode('utf-8')[:n - 1]
    while True:
        try:
            b.decode('utf-8')
            break
        except UnicodeDecodeError:
            b = b[:-1]
    return b.ljust(n, b'\0')


def pack_record(title, subtitle, d, pixels):
    return fixed(title, 96) + fixed(subtitle, 64) + fixed(d, 256) + pixels


def embedded_art(path):
    try:
        import mutagen
    except ImportError:
        return None
    try:
        f = mutagen.File(path)
        if f is None:
            return None
        if getattr(f, 'pictures', None):
            return f.pictures[0].data
        if f.tags is not None:
            for k in f.tags.keys():
                if k.startswith('APIC'):
                    return f.tags[k].data
            if 'covr' in f.tags and f.tags['covr']:
                return bytes(f.tags['covr'][0])
    except Exception:
        pass
    return None


def tile_pixels(folder, files):
    from PIL import Image
    import io
    low = {n.lower(): n for n in files}
    src = None
    for c in COVERS:
        if c in low:
            src = os.path.join(folder, low[c])
            break
    if src is None:
        for n in sorted(files):
            if n.lower().endswith(('.jpg', '.png')):
                src = os.path.join(folder, n)
                break
    img = None
    try:
        if src:
            img = Image.open(src)
        else:
            audio = sorted(n for n in files if n.lower().endswith(EXTS))
            data = embedded_art(os.path.join(folder, audio[0]))
            if data:
                img = Image.open(io.BytesIO(data))
        if img is not None:
            img = img.convert('RGB')
            w, h = img.size
            s = min(w, h)
            l, t = (w - s) // 2, (h - s) // 2
            img = img.crop((l, t, l + s, t + s)).resize((TILE, TILE), Image.LANCZOS)
    except Exception:
        img = None
    if img is None:
        return struct.pack('<H', 0x8410) * (TILE * TILE)
    return b''.join(struct.pack('<H', ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3))
                    for r, g, b in img.getdata())


def sortkey(a):
    artist = a['artist'].casefold()
    if artist.startswith('the '):
        artist = artist[4:]
    return (artist, a['year'] is None, a['year'] or 0, a['title'].casefold())


def build(root):
    music = os.path.join(root, 'Music')
    albums = []
    for artist in os.listdir(music):
        ap = os.path.join(music, artist)
        if artist.startswith('.') or not os.path.isdir(ap):
            continue
        for album in os.listdir(ap):
            p = os.path.join(ap, album)
            if album.startswith('.') or not os.path.isdir(p):
                continue
            files = [n for n in os.listdir(p) if os.path.isfile(os.path.join(p, n))]
            if not any(n.lower().endswith(EXTS) for n in files):
                continue
            m = re.match(r'^(.*) \((\d{4})\)$', nfc(album))
            title, year = (m.group(1), int(m.group(2))) if m else (nfc(album), None)
            a = nfc(artist)
            albums.append(dict(artist=a, title=title, year=year, folder=p, files=files,
                               dir='/Music/%s/%s' % (a, nfc(album))))
    albums.sort(key=sortkey)
    out = [struct.pack('<4sIII', b'AGR1', len(albums), TILE, 0)]
    for a in albums:
        sub = '%s · %d' % (a['artist'], a['year']) if a['year'] else a['artist']
        out.append(pack_record(a['title'], sub, a['dir'], tile_pixels(a['folder'], a['files'])))
    d = os.path.join(root, '.rockbox')
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, 'albumgrid.dat.tmp')
    with open(tmp, 'wb') as f:
        f.write(b''.join(out))
    os.replace(tmp, os.path.join(d, 'albumgrid.dat'))
    print(len(albums))


def _selftest():
    rec = pack_record('T', 'A · 1999', '/Music/A/T (1999)', b'\0' * (TILE * TILE * 2))
    assert len(rec) == REC
    hdr = struct.pack('<4sIII', b'AGR1', 1, TILE, 0)
    assert len(hdr) == 16 and hdr[:4] == b'AGR1'
    assert len(fixed('é' * 100, 96)) == 96 and fixed('é' * 100, 96)[-1] == 0
    print('selftest ok')


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == '--selftest':
        _selftest()
    elif len(sys.argv) == 2:
        build(sys.argv[1])
    else:
        sys.exit('usage: build_albumgrid.py <device_root> | --selftest')
