# -*- coding: utf-8 -*-
"""把指定 PDF 的指定页渲染成 PNG。用法：
  python render_pdf_pages.py <pdf> <outdir> <page1,page2,...>   # 页号为 1 起
"""
import sys, os, fitz

def main():
    pdf, outdir, pages = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    doc = fitz.open(pdf)
    print("total pages:", doc.page_count)
    for p in [int(x) for x in pages.split(",")]:
        page = doc[p - 1]
        fp = os.path.join(outdir, "page%03d.png" % p)
        page.get_pixmap(dpi=180).save(fp)
        print("rendered page", p, "->", fp)

if __name__ == "__main__":
    main()
