"""Stage only files used by the native Sorcery+ executable, from this disk copy."""
import argparse
import hashlib
from pathlib import Path
from shutil import copyfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('disk', type=Path)
p.add_argument('extraction', type=Path)
p.add_argument('output', type=Path)
args = p.parse_args()
disk = args.disk.read_bytes()
if hashlib.sha256(disk).hexdigest() != 'f5dee21140345ea84bee66ccf86db892c079ce13bc1d13c240a2103ba0ce2a26':
    p.error('Unexpected disk version')
assets = args.output / 'assets'
assets.mkdir(parents=True, exist_ok=True)
for name in ('CODE-decoded-7000.bin', 'CONALP-decoded-0100.bin', 'TISCRN-decoded-69c0.bin',
             'SOUND-decoded-a600.bin', 'track-03-sector-7b-4000.bin', 'room-sectors-00-4e.bin',
             'track-39-sector-c9-high-scores.bin'):
    copyfile(args.extraction / 'recovered' / name, assets / name)
for name in ('TITLEP.BIN', 'CONSET1.BIN', 'CONSET2.BIN', 'SPRITES1.BIN', 'SPRITES2.BIN',
             'LOCDATA.BIN', 'SMESSC.BIN', 'MUCODE.BIN'):
    copyfile(args.extraction / 'payload' / name, assets / name)
start = 0x100 + 39 * 0x1300
for n in range(disk[start + 21]):
    if disk[start + 26 + 8*n] == 0xc8:
        sector = start + 0x100 + 512*n
        (assets / 'track-39-sector-c8.bin').write_bytes(disk[sector:sector+512])
        break
else:
    p.error('Missing track 39 sector C8')
