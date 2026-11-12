import sys, glob
from PIL import Image, ImageStat
for f in sorted(glob.glob(sys.argv[1] + '/*.png')):
    im = Image.open(f).convert('L'); w, h = im.size
    st = ImageStat.Stat(im.crop((0, int(h*0.25), w, h)))   # below the sky/HUD top
    import os; print(os.path.basename(f), round(st.mean[0]))
