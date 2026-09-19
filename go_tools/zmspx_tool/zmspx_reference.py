#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
zmspx (magic "zms2") 精灵文件参考实现  —— 修正版
=================================================
本脚本按逆向得到的真实格式解析 / 解包 / 渲染 .zmspx，可作为 Go 版 zmspx_tool 的对照实现。

用法:
    python3 zmspx_reference.py decode <file.zmspx> [out.json]
    python3 zmspx_reference.py unpack <file.zmspx|目录> -o <输出目录>
    python3 zmspx_reference.py render <file.zmspx|目录> -o <输出目录> [--delay ms] [--scale N]

格式（小端）:
  头 28 字节: "zms2" | u32 纹理数 | u32 0 | u32 帧数 | u32 0 | u32 动画数 | u32 0
  纹理表 (12B/条, 紧随头): w(u16) h(u16) type(u16) 0(u16) dataOffset(u32, 文件绝对偏移)
  帧表   (28B/条): nk(u16) nx(u16) 0(u16) 0(u16)  bbox(x0,y0,x1,y1 各 s16)  kfOff(u32) auxOff(u32) endOff(u32)
  动画表 (8B/条):  count(u32) seqOff(u32)   # seq 为 count 个 u8，每个字节 = 帧号
  关键帧 (8B/条):  texIndex(u16) x(s16) y(s16) flags(u16)
       flags: bit0 = 上下翻转, bit1 = 左右翻转, bit2 = 其它标志(本样例中不改变变换)
  附加矩形 (8B/条, nx 条): x(s16) y(s16) w(s16) h(s16)   # 游戏逻辑用(命中盒/裁剪盒)，渲染不需要

  画布尺寸 = (x1-x0) × (y1-y0)；关键帧贴图左上角落在画布坐标 (x - x0, y - y0)。
  像素格式 = type & 0xFF:  1 = 16bpp RGB565(小端, 不透明)  3 = 32bpp BGRA8888(字节序 B,G,R,A)
                           2 = 8bpp 调色板(本样例未出现, 未验证)
"""
import json
import os
import struct
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

HEADER_SIZE = 28
TEX_REC = 12
FRAME_REC = 28
ANIM_REC = 8
KF_REC = 8
AUX_REC = 8


def bpp_of(fmt):
    """像素格式 -> 每像素字节数。"""
    return {1: 2, 2: 1, 3: 4}.get(fmt, 4)


def fmt_name(fmt):
    return {1: "RGB565", 2: "PAL8", 3: "BGRA"}.get(fmt, "BGRA")


# ----------------------------------------------------------------------------- 解析
def parse(data):
    if data[:4] != b"zms2":
        raise ValueError("不是 zms2 文件: %r" % data[:4])
    ntex, nfr, nanim = (struct.unpack_from("<I", data, 4)[0],
                        struct.unpack_from("<I", data, 12)[0],
                        struct.unpack_from("<I", data, 20)[0])
    tex_table = HEADER_SIZE
    frame_table = tex_table + ntex * TEX_REC
    anim_table = frame_table + nfr * FRAME_REC

    textures = []
    for i in range(ntex):
        w, h, typ, _pad = struct.unpack_from("<HHHH", data, tex_table + i * TEX_REC)
        off = struct.unpack_from("<I", data, tex_table + i * TEX_REC + 8)[0]
        fmt = typ & 0xFF
        bpp = bpp_of(fmt)
        if i + 1 < ntex:
            size = struct.unpack_from("<I", data, tex_table + (i + 1) * TEX_REC + 8)[0] - off
        elif nfr:                       # 最后一张纹理的结束 = 帧 0 关键帧数组的起始
            size = struct.unpack_from("<I", data, frame_table + 16)[0] - off
        else:
            size = len(data) - off
        textures.append(dict(index=i, width=w, height=h, type=typ, format=fmt,
                             mode=fmt_name(fmt), bpp=bpp, offset=off, size=size,
                             expect=w * h * bpp))

    frames = []
    for i in range(nfr):
        o = frame_table + i * FRAME_REC
        nk, nx = struct.unpack_from("<HH", data, o)
        x0, y0, x1, y1 = struct.unpack_from("<hhhh", data, o + 8)
        kf_off, aux_off, end_off = struct.unpack_from("<III", data, o + 16)
        kfs = []
        for k in range(nk):
            ti, x, y, fl = struct.unpack_from("<HhhH", data, kf_off + k * KF_REC)
            kfs.append(dict(tex=ti, x=x, y=y, flags=fl,
                            flip_v=fl & 1, flip_h=(fl >> 1) & 1, flag_bit2=(fl >> 2) & 1))
        aux = []
        for k in range(nx):
            aux.append(struct.unpack_from("<hhhh", data, aux_off + k * AUX_REC))
        frames.append(dict(index=i, nk=nk, nx=nx, x0=x0, y0=y0, x1=x1, y1=y1,
                           width=x1 - x0, height=y1 - y0,
                           kf_off=kf_off, aux_off=aux_off, end_off=end_off,
                           keyframes=kfs, aux_rects=aux))

    anims = []
    for i in range(nanim):
        o = anim_table + i * ANIM_REC
        cnt, off = struct.unpack_from("<II", data, o)
        anims.append(dict(index=i, count=cnt, offset=off,
                          sequence=list(data[off:off + cnt])))

    return dict(tex=textures, frames=frames, anims=anims,
                magic="zms2", ntex=ntex, nframes=nfr, nanim=nanim, size=len(data))


# ----------------------------------------------------------------------------- 像素
def tex_to_image(data, t):
    """纹理 -> PIL RGBA 图像。32bpp 为 BGRA 字节序，需交换 R/B。"""
    raw = data[t["offset"]:t["offset"] + t["expect"]]
    w, h, fmt = t["width"], t["height"], t["format"]
    if w == 0 or h == 0:
        return Image.new("RGBA", (1, 1), (0, 0, 0, 0))
    if fmt == 3:                                    # B,G,R,A -> R,G,B,A
        px = bytes(b for i in range(0, w * h * 4, 4)
                   for b in (raw[i + 2], raw[i + 1], raw[i], raw[i + 3]))
        return Image.frombytes("RGBA", (w, h), px)
    if fmt == 1:                                    # RGB565 -> RGB
        px = bytearray()
        for (v,) in struct.iter_unpack("<H", raw[:w * h * 2]):
            px += bytes((((v >> 11) & 0x1F) * 255 // 31,
                         ((v >> 5) & 0x3F) * 255 // 63,
                         (v & 0x1F) * 255 // 31, 255))
        return Image.frombytes("RGBA", (w, h), bytes(px))
    if fmt == 2:                                    # 8bpp 调色板: 本样例未出现
        px = bytearray()
        for v in raw[:w * h]:
            px += bytes((v, v, v, 255))
        return Image.frombytes("RGBA", (w, h), bytes(px))
    raise ValueError("未知像素格式 0x%x" % t["type"])


def apply_flip(img, kf):
    if kf["flip_h"]:
        img = img.transpose(Image.FLIP_LEFT_RIGHT)
    if kf["flip_v"]:
        img = img.transpose(Image.FLIP_TOP_BOTTOM)
    return img


def over_into(dst, src, ox, oy):
    """把 src 按标准 source-over 合成到 dst 上（非预乘 RGBA，整数运算）。

    sa = src.A, da = dst.A
    numA = sa*255 + da*(255-sa)
    outA = (numA + 127) // 255
    outC = (src.C*sa*255 + dst.C*da*(255-sa) + (numA-1)//2) // numA
    说明：该式与 Pillow 的 Image.alpha_composite 的差异不超过 1/255，
    但用同一套整数式可让本参考实现与修正版 Go 工具逐像素完全一致。
    """
    sw, sh = src.size
    dw, dh = dst.size
    sp = src.load()
    dp = dst.load()
    for y in range(sh):
        dy = oy + y
        if dy < 0 or dy >= dh:
            continue
        for x in range(sw):
            dx = ox + x
            if dx < 0 or dx >= dw:
                continue
            sr, sg, sb, sa = sp[x, y]
            if sa == 0:
                continue
            if sa == 255:
                dp[dx, dy] = (sr, sg, sb, 255)
                continue
            dr, dg, db, da = dp[dx, dy]
            num_a = sa * 255 + da * (255 - sa)
            out = [(num_a + 127) // 255]
            for sc, dc in ((sr, dr), (sg, dg), (sb, db)):
                num = sc * sa * 255 + dc * da * (255 - sa)
                v = (num + (num_a - 1) // 2) // num_a
                out.append(255 if v > 255 else v)
            dp[dx, dy] = (out[1], out[2], out[3], out[0])


def frame_to_image(data, p, frame_index):
    """按关键帧把纹理合成到画布，得到该帧图像。"""
    f = p["frames"][frame_index]
    if f["width"] <= 0 or f["height"] <= 0:
        # 空帧占位 (nk=0, bbox 填 0xCDCD): 无内容
        return Image.new("RGBA", (1, 1), (0, 0, 0, 0))
    canvas = Image.new("RGBA", (f["width"], f["height"]), (0, 0, 0, 0))
    for kf in f["keyframes"]:
        t = p["tex"][kf["tex"]]
        img = apply_flip(tex_to_image(data, t), kf)
        over_into(canvas, img, kf["x"] - f["x0"], kf["y"] - f["y0"])
    return canvas


# ----------------------------------------------------------------------------- 命令
def do_decode(path, out_json=None):
    data = open(path, "rb").read()
    p = parse(data)
    print("ZmspxFile('%s': %d textures, %d frames, %d animations)" %
          (path, p["ntex"], p["nframes"], p["nanim"]))
    for t in p["tex"]:
        print("  tex[%3d] %4dx%-4d type=0x%03x(%s) off=0x%06x size=%d" %
              (t["index"], t["width"], t["height"], t["type"], t["mode"], t["offset"], t["size"]))
    for f in p["frames"]:
        print("  frame[%3d] canvas=%dx%d bbox=(%d,%d,%d,%d) nk=%d nx=%d" %
              (f["index"], f["width"], f["height"], f["x0"], f["y0"], f["x1"], f["y1"], f["nk"], f["nx"]))
        for kf in f["keyframes"]:
            print("        kf tex=%d (%d,%d) flags=0x%x flipH=%d flipV=%d" %
                  (kf["tex"], kf["x"], kf["y"], kf["flags"], kf["flip_h"], kf["flip_v"]))
    for a in p["anims"]:
        print("  anim[%d] frames=%s" % (a["index"], a["sequence"]))
    if out_json:
        json.dump(p, open(out_json, "w"), indent=2, ensure_ascii=False)
        print("metadata -> %s" % out_json)
    return p


def do_unpack(path, out_dir, export_frames=True, export_gif=True, delay=80, scale=1):
    data = open(path, "rb").read()
    p = parse(data)
    name = os.path.splitext(os.path.basename(path))[0]
    d_tex = os.path.join(out_dir, name, "textures")
    os.makedirs(d_tex, exist_ok=True)
    for t in p["tex"]:
        if t["width"] == 0 or t["height"] == 0:
            continue
        img = tex_to_image(data, t)
        img.save(os.path.join(d_tex, "tex_%04d_%dx%d_t%d.png" %
                              (t["index"], t["width"], t["height"], t["type"])))
    if export_frames and p["frames"]:
        d_fr = os.path.join(out_dir, name, "frames")
        os.makedirs(d_fr, exist_ok=True)
        for f in p["frames"]:
            img = frame_to_image(data, p, f["index"])
            if scale > 1:
                img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
            img.save(os.path.join(d_fr, "frame_%02d_%dx%d.png" %
                                  (f["index"], img.width, img.height)))
    if export_gif and p["anims"]:
        d_gif = os.path.join(out_dir, name, "gif")
        os.makedirs(d_gif, exist_ok=True)
        for a in p["anims"]:
            imgs = [frame_to_image(data, p, i) for i in a["sequence"] if i < p["nframes"]]
            if not imgs:
                continue
            if scale > 1:
                imgs = [im.resize((im.width * scale, im.height * scale), Image.NEAREST) for im in imgs]
            imgs = [im.convert("P", palette=Image.ADAPTIVE, colors=255) for im in imgs] \
                if False else imgs
            imgs[0].save(os.path.join(d_gif, "%s_anim%d.gif" % (name, a["index"])),
                         save_all=True, append_images=imgs[1:], duration=delay,
                         loop=0, disposal=2)
    json.dump(p, open(os.path.join(out_dir, name, "metadata.json"), "w"),
              indent=2, ensure_ascii=False)
    print("%-28s tex=%-3d frame=%-3d anim=%-2d -> %s" %
          (name, p["ntex"], p["nframes"], p["nanim"], os.path.join(out_dir, name)))
    return p


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd, path = argv[1], argv[2]
    out = "./zmspx_out"
    delay, scale = 80, 1
    rest = argv[3:]
    for i, a in enumerate(rest):
        if a in ("-o", "--output") and i + 1 < len(rest):
            out = rest[i + 1]
        if a == "--delay" and i + 1 < len(rest):
            delay = int(rest[i + 1])
        if a == "--scale" and i + 1 < len(rest):
            scale = int(rest[i + 1])
    if cmd == "decode":
        do_decode(path, rest[0] if rest and not rest[0].startswith("-") else None)
    elif cmd == "unpack":
        if os.path.isdir(path):
            for f in sorted(os.listdir(path)):
                if f.endswith(".zmspx"):
                    do_unpack(os.path.join(path, f), out, delay=delay, scale=scale)
        else:
            do_unpack(path, out, delay=delay, scale=scale)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
