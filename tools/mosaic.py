"""One frame of every traced game on a few pages, each with its key (PIL).

    python tools/mosaic.py build/trace f03000.ppm build/mosaic [--cols 12 --rows 9 --scale 1]

Each frame (160x144) sits in its cell with the game's key under it.
"""
import argparse
import glob
import os

from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tracedir')
    ap.add_argument('frame')
    ap.add_argument('out')
    ap.add_argument('--cols', type=int, default=12)
    ap.add_argument('--rows', type=int, default=9)
    ap.add_argument('--scale', type=float, default=1.0)
    a = ap.parse_args()
    shots = sorted(glob.glob(os.path.join(a.tracedir, '*', a.frame)))
    os.makedirs(a.out, exist_ok=True)
    cell = int(160 * a.scale)
    per = a.cols * a.rows
    for page in range(0, len(shots), per):
        sheet = Image.new('RGB', (a.cols * (cell + 4), a.rows * (int(144 * a.scale) + 14)), (40, 40, 40))
        d = ImageDraw.Draw(sheet)
        for i, s in enumerate(shots[page:page + per]):
            im = Image.open(s).convert('RGB')
            im = im.resize((int(im.width * a.scale), int(im.height * a.scale)))
            ch = int(144 * a.scale)
            x, y = (i % a.cols) * (cell + 4), (i // a.cols) * (ch + 14)
            sheet.paste(im, (x + (cell - im.width) // 2, y + (ch - im.height) // 2))
            d.text((x + 1, y + ch), os.path.basename(os.path.dirname(s))[:24], fill=(230, 230, 230))
        p = os.path.join(a.out, f'page{page // per + 1}.png')
        sheet.save(p)
        print(p)


if __name__ == '__main__':
    main()
