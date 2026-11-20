# Four run shots (sorted) in a 2x2 grid of 400x225 tiles: tools/kar_shot_grid.py <run dir> <out.png>
import sys,os
from PIL import Image
d,out=sys.argv[1],sys.argv[2]
fs=sorted(f for f in os.listdir(d) if f.endswith('.png'))[:4]
ims=[Image.open(os.path.join(d,f)).convert('RGB').resize((400,225)) for f in fs]
W=Image.new('RGB',(800,450)); [W.paste(im,((i%2)*400,(i//2)*225)) for i,im in enumerate(ims)]; W.save(out)
