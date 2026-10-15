"""生成托盘/程序图标 app.ico（一只"眼睛"，表示显示/隐藏）。

用法: python tools/make_icon.py src/app.ico
依赖: Pillow
"""

import sys
from PIL import Image, ImageChops, ImageDraw

S = 1024  # 以 4 倍尺寸绘制再缩放，得到平滑边缘


def make_eye(size_px: int) -> Image.Image:
    """在透明画布上绘制一只眼睛，返回给定像素尺寸的图像。"""
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    # 圆角方形底
    d.rounded_rectangle([0, 0, S - 1, S - 1], radius=int(S * 0.23),
                        fill=(38, 78, 138, 255))

    # 眼形 = 两个错位大圆的交集
    cy = S // 2
    r = int(S * 0.40)
    dy = int(S * 0.20)
    mask_a = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask_a).ellipse([S // 2 - r, cy - dy - r, S // 2 + r, cy - dy + r], fill=255)
    mask_b = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask_b).ellipse([S // 2 - r, cy + dy - r, S // 2 + r, cy + dy + r], fill=255)
    eye_mask = ImageChops.darker(mask_a, mask_b)

    img.paste((255, 255, 255, 255), (0, 0), eye_mask)

    # 瞳孔（限制在眼形内）
    pr = int(S * 0.135)
    pupil = Image.new("L", (S, S), 0)
    ImageDraw.Draw(pupil).ellipse([S // 2 - pr, cy - pr, S // 2 + pr, cy + pr], fill=255)
    pupil = ImageChops.darker(pupil, eye_mask)
    img.paste((38, 78, 138, 255), (0, 0), pupil)

    return img.resize((size_px, size_px), Image.LANCZOS)


def main() -> int:
    out = sys.argv[1] if len(sys.argv) > 1 else "src/app.ico"
    sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256]
    master = make_eye(256)
    master.save(out, format="ICO", sizes=[(s, s) for s in sizes])
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
