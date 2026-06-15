# Minimal QBoot packages (from parent qemu-vexpress-a9/packages)
from building import *
import os

cwd = GetCurrentDir()
parent_pkgs = os.path.normpath(os.path.join(cwd, '../../qemu-vexpress-a9/packages'))

objs = []

crclib = os.path.join(parent_pkgs, 'crclib-v1.02')
objs.append(DefineGroup(
    'crclib',
    [os.path.join(crclib, 'src', 'crc32.c')],
    depend=['PKG_USING_CRCLIB'],
    CPPPATH=[os.path.join(crclib, 'inc')]))

objs.extend(SConscript(
    os.path.join(parent_pkgs, 'quicklz-v1.0.1', 'SConscript'),
    variant_dir='build/packages/quicklz-v1.0.1',
    duplicate=0))

qboot = os.path.join(parent_pkgs, 'qboot-latest')
objs.append(DefineGroup(
    'qboot',
    [
        os.path.join(qboot, 'src', 'qboot.c'),
        os.path.join(qboot, 'src', 'qboot_quicklz.c'),
    ],
    depend=['PKG_USING_QBOOT'],
    CPPPATH=[os.path.join(qboot, 'inc')]))

Return('objs')
