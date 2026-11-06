"""Compose the app icon from the on-screen bowtie screenshot."""
from PIL import Image

src = Image.open("G:/tts/mean2vc/icon_src.png").convert("RGB")
# tight crop around the bowtie (2400x1080 screen): wings 455..1540, top 230,
# bottom incl. shadow ~650 — small even margin
crop = src.crop((430, 190, 1560, 670))          # 1130 x 480
# square canvas, bowtie centered slightly above middle (optical balance)
side = 1024
canvas = Image.new("RGB", (side, side), (255, 255, 255))
target_w = int(side * 0.86)                     # 880 px wide inside the square
ratio = target_w / crop.width
crop2 = crop.resize((target_w, int(crop.height * ratio)), Image.LANCZOS)
canvas.paste(crop2, ((side - crop2.width) // 2, (side - crop2.height) // 2 - 20))
canvas.save("G:/tts/mean2vc/icon_square.png")

# adaptive-icon FOREGROUND: the bowtie must sit inside the inner ~66% safe
# zone (launchers mask ~18% off every edge), transparent background
fg = Image.new("RGBA", (side, side), (0, 0, 0, 0))
fg_w = int(side * 0.62)
ratio = fg_w / crop.width
crop3 = crop.resize((fg_w, int(crop.height * ratio)), Image.LANCZOS)
mask = Image.new("L", crop3.size, 255)
fg.paste(crop3, ((side - crop3.width) // 2, (side - crop3.height) // 2 - 10), mask)
fg.save("G:/tts/mean2vc/meanvc-android/app/src/main/res/mipmap-anydpi-v29_tmp_fg.png"
        if False else "G:/tts/mean2vc/icon_fg.png")
print("icon_square.png + icon_fg.png written",
      canvas.size, fg.size)
