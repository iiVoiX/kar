import sys, glob, os
from PIL import Image, ImageStat
for f in sorted(glob.glob(sys.argv[1] + '/*.png')):
    im = Image.open(f).convert('RGB'); w, h = im.size
    k = im.crop((int(w*0.40), int(h*0.62), int(w*0.60), int(h*0.85)))   # Kirby at the start line
    print(os.path.basename(f), [round(x) for x in ImageStat.Stat(k).mean])
