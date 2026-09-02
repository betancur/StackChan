"""Generate icon_claude_buddy.bin (LVGL9 RGB565A8, 188x150) in the launcher's flat black pictogram style."""
import math, struct, sys
from PIL import Image, ImageDraw

W, H = 188, 150
SS = 4  # supersample
img = Image.new('RGBA', (W*SS, H*SS), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
cx, cy = W*SS/2, H*SS/2
stroke = 15*SS

# Eight-ray sparkle: alternating long/short rounded rays, like a starburst
long_r, short_r = 62*SS, 44*SS
for i in range(8):
    a = math.radians(i*45 + 22.5)
    r = long_r if i % 2 == 0 else short_r
    x2, y2 = cx + r*math.cos(a), cy + r*math.sin(a)
    d.line([(cx, cy), (x2, y2)], fill=(0, 0, 0, 255), width=stroke)
    d.ellipse([x2-stroke/2, y2-stroke/2, x2+stroke/2, y2+stroke/2], fill=(0, 0, 0, 255))
# Center dot to soften the joint
d.ellipse([cx-stroke*0.6, cy-stroke*0.6, cx+stroke*0.6, cy+stroke*0.6], fill=(0, 0, 0, 255))

img = img.resize((W, H), Image.LANCZOS)
img.save(sys.argv[1] + '.png')

# Pack: lv_image_header_t (magic=0x19, cf=0x14 RGB565A8, flags, w, h, stride, reserved) + RGB565 + A8
px = img.load()
rgb = bytearray(); alpha = bytearray()
for y in range(H):
    for x in range(W):
        r, g, b, a = px[x, y]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        rgb += struct.pack('<H', v)
        alpha.append(a)
hdr = struct.pack('<BBHHHHH', 0x19, 0x14, 0, W, H, W*2, 0)
open(sys.argv[1] + '.bin', 'wb').write(hdr + rgb + alpha)
print('wrote', sys.argv[1] + '.bin', len(hdr)+len(rgb)+len(alpha), 'bytes')
