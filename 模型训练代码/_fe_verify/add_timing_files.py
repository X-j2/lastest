# -*- coding: utf-8 -*-
"""把 timing.c/h 加入 1.uvprojx 的 HARDWARE 分组。幂等。"""
import os
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UVPROJX = os.path.join(ROOT, 'ndir-f407', '1.uvprojx')

NEW = [('timing.c', r'.\HARDWARE\timing.c', '1'),
       ('timing.h', r'.\HARDWARE\timing.h', '5')]

with open(UVPROJX, 'r', encoding='utf-8') as f:
    s = f.read()

if '<FileName>timing.c</FileName>' in s:
    print('timing.c 已在工程中，跳过')
    sys.exit(0)

marker = '<GroupName>HARDWARE</GroupName>'
i = s.find(marker)
if i < 0:
    print('ERROR: 未找到 HARDWARE 分组', file=sys.stderr)
    sys.exit(1)
j = s.find('</Files>', i)
if j < 0:
    print('ERROR: HARDWARE 分组未闭合', file=sys.stderr)
    sys.exit(1)

block = []
for name, path, ftype in NEW:
    block += ['            <File>',
              f'              <FileName>{name}</FileName>',
              f'              <FileType>{ftype}</FileType>',
              f'              <FilePath>{path}</FilePath>',
              '            </File>']

s = s[:j] + '\n'.join(block) + '\n          ' + s[j:]

with open(UVPROJX, 'w', encoding='utf-8', newline='') as f:
    f.write(s)

# 校验
root = ET.parse(UVPROJX).getroot()
g = [x for x in root.iter('Group') if x.findtext('GroupName') == 'HARDWARE'][0]
names = [f.findtext('FileName') for f in g.findall('./Files/File')]
print('HARDWARE 分组现有文件:', len(names))
print('  timing.c 存在:', 'timing.c' in names)
print('  timing.h 存在:', 'timing.h' in names)
