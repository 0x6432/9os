#!/usr/bin/env python3
"""Check real root/user Qt asynchronous image grabs against the packaged asset."""
import argparse
import base64
import io
import pathlib
import re
from PIL import Image, ImageChops, ImageOps, ImageStat


def check_image(im, source):
    if im.size != (320, 240):
        raise ValueError('unexpected image dimensions')
    im.load()
    if im.mode == 'RGBA' and im.getchannel('A').getextrema() != (255, 255):
        raise ValueError('unexpected transparent capture')
    im = im.convert('RGB')
    colors = len(im.getcolors(im.width * im.height) or [])
    if colors < 1000:
        raise ValueError('blank or insufficiently decoded image')
    expected = ImageOps.fit(source.convert('RGB'), im.size, method=Image.Resampling.BILINEAR)
    delta = ImageStat.Stat(ImageChops.difference(im, expected)).mean
    if max(delta) > 3:
        raise ValueError(f'captured pixels differ from actual stock wallpaper: {delta}')
    return colors, delta


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('log')
    ap.add_argument('source', help='actual packaged Next/contents/images/1024x768.png')
    args = ap.parse_args()
    try:
        text = pathlib.Path(args.log).read_text(errors='replace')
        records = re.findall(r'^ALPINE_ASYNC_IMAGE_PNG_BEGIN uid=(\d+)\n(.*?)^ALPINE_ASYNC_IMAGE_PNG_END', text, re.M | re.S)
        if sorted(uid for uid, _ in records) != ['0', '1000']:
            raise ValueError('require exactly one actual root and one actual UID1000 capture')
        with Image.open(args.source) as source:
            source.load()
            for uid, data in records:
                raw = base64.b64decode(''.join(data.split()), validate=True)
                if len(raw) > 1 << 20:
                    raise ValueError('unbounded image capture')
                with Image.open(io.BytesIO(raw)) as im:
                    colors, delta = check_image(im, source)
                print(f'PASS: UID{uid} actual async Qt wallpaper capture: {colors} colors, mean delta {max(delta):.3f}')
    except (OSError, ValueError) as e:
        ap.exit(1, f'FAIL: async image acceptance: {e}\n')


if __name__ == '__main__':
    main()
