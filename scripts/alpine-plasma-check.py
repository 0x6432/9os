#!/usr/bin/env python3
"""Reject absent/blank desktop captures; this is not a full KDE service test."""
import base64,io,pathlib,re,sys
from PIL import Image

def check(raw):
    blocks=re.findall(rb'ALPINE_PLASMA_PNG_BEGIN\r?\n(.*?)ALPINE_PLASMA_PNG_END',raw,re.S)
    if not blocks or not re.search(rb'PLASMA_DESKTOPS=desktops=[1-9]\d* panels=[1-9]\d*',raw):
        raise ValueError('Missing real Plasma desktop/panel readiness or capture')
    if b'PLASMA_WALLPAPER_CONFIGURED=1' not in raw:
        raise ValueError('Packaged wallpaper was not configured through Plasma')
    if re.search(rb'error when loading applet|module [^\n]*is not installed|PLASMA_EARLY_EXIT',raw):
        raise ValueError('Plasma has missing/failed runtime QML applets')
    for block in blocks:
        im=Image.open(io.BytesIO(base64.b64decode(re.sub(rb'\s',b'',block),validate=True))).convert('RGBA')
        if im.size!=(800,600):raise ValueError('Unexpected desktop size')
        if im.getextrema()[3]!=(255,255):raise ValueError('Transparent output')
        # A mere panel over black (our early partial result) must NOT pass.
        background=im.crop((0,0,800,480))
        if len(background.getcolors(background.width*background.height) or [])<1000:
            raise ValueError('Missing/non-rendered wallpaper; panel alone is insufficient')
        panel=im.crop((0,550,800,600))
        if len(panel.getcolors(panel.width*panel.height) or [])<32:
            raise ValueError('Blank panel region')
    return len(blocks)

if __name__=='__main__':
    try: n=check(pathlib.Path(sys.argv[1]).read_bytes())
    except (ValueError,OSError) as e:raise SystemExit(str(e))
    print(f'PASS: {n} real Plasma 800x600 compositor captures with rendered wallpaper/panel; services/input require separate tests')
