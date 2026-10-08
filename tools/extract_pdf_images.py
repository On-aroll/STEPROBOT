# -*- coding: utf-8 -*-
"""从 PDF 指定页提取内嵌图片（原分辨率），并可选整页高分辨率渲染。"""
import sys, os, pymupdf

def main():
    pdf, outdir, pages = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    doc = pymupdf.open(pdf)
    for p in [int(x) for x in pages.split(",")]:
        page = doc[p - 1]
        # 1) 提取内嵌位图
        for idx, info in enumerate(page.get_images(full=True)):
            xref = info[0]
            img = doc.extract_image(xref)
            ext = img["ext"]
            w, h = img["width"], img["height"]
            fn = os.path.join(outdir, "p%d_img%d_%dx%d.%s" % (p, idx, w, h, ext))
            with open(fn, "wb") as f:
                f.write(img["image"])
            print("image:", fn)
        # 2) 整页高清渲染
        page.get_pixmap(dpi=400).save(os.path.join(outdir, "p%d_full400.png" % p))
        print("page %d rendered @400dpi" % p)

if __name__ == "__main__":
    main()
