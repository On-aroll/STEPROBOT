# -*- coding: utf-8 -*-
"""连接 STEPStudio 窗口，枚举 UI 树里的可交互控件（菜单/按钮/选项卡）。"""
import sys
from pywinauto import Desktop, Application

TITLE = 'STEPStudio V 4.7.0'

def main():
    app = Application(backend='uia').connect(title=TITLE, timeout=10)
    win = app.window(title=TITLE)
    print('connected:', win.window_text(), '| class:', win.class_name())

    seen = 0
    for c in win.descendants():
        try:
            ct = c.element_info.control_type
            nm = (c.element_info.name or '').strip()
            auto = c.element_info.automation_id or ''
        except Exception:
            continue
        if not nm:
            continue
        if ct in ('MenuItem', 'Button', 'TabItem', 'ListItem', 'ComboBox', 'CheckBox', 'RadioButton', 'ToolBar'):
            print(f"{ct:12s} | {nm[:44]:44s} | id={auto[:40]}")
            seen += 1
        if seen > 220:
            print('...(truncated)')
            break
    print('total listed:', seen)

if __name__ == '__main__':
    main()