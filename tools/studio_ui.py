# -*- coding: utf-8 -*-
"""连接 STEPStudio 主窗口并按名字枚举可交互控件。
用法: python tools/studio_ui.py [hwnd]
"""
import sys
from pywinauto import Desktop

DEFAULT_HWND = 460840
KEEP = ('MenuItem', 'Button', 'TabItem', 'ListItem', 'ComboBox',
        'CheckBox', 'RadioButton', 'SplitButton', 'TreeItem', 'Text')


def main():
    hwnd = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_HWND
    desk = Desktop(backend='uia')
    win = desk.window(handle=hwnd)
    print('window:', win.window_text(), '| class:', win.class_name())

    n = 0
    for c in win.descendants():
        try:
            ei = c.element_info
            ct = ei.control_type
            nm = (ei.name or '').strip()
        except Exception:
            continue
        if not nm or ct not in KEEP:
            continue
        print(f'{ct:11s} | {nm[:52]:52s} | id={(ei.automation_id or "")[:26]}')
        n += 1
        if n >= 250:
            print('...(truncated)')
            break
    print('total:', n)


if __name__ == '__main__':
    main()