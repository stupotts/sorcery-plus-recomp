import { createHash } from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { crc32, deflateSync } from 'node:zlib';

const [imagePath, destination] = process.argv.slice(2);
if (!imagePath || !destination) {
  throw new Error('Usage: node sorcery-extract.mjs <disk image> <new extraction folder>');
}

const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');
const image = readFileSync(imagePath);
const header = image.toString('ascii', 0, 34);
const extended = header === 'EXTENDED CPC DSK File\r\nDisk-Info\r\n';
const standard = header === 'MV - CPCEMU Disk-File\r\nDisk-Info\r\n';
const trackCount = image[48];
if (image.length < 256 || image[49] !== 1 ||
    !(standard && trackCount === 40 || extended && trackCount >= 40 && trackCount <= 204 &&
      image.subarray(92, 52 + trackCount).every(size => size === 0))) {
  throw new Error('Expected a single-sided Amstrad CPC DSK image with 40 recorded tracks.');
}

const sectorSize = 512;
const sectorsPerTrack = 9;
const sectors = new Map();
const unusualTracks = [];
let secondStage;
let alternateSecondStage;
let trackStart = 256;

for (let track = 0; track < 40; track++) {
  const trackSize = extended ? image[52 + track] * 256 : image.readUInt16LE(50);
  const count = image[trackStart + 21];
  if (!count || count > sectorsPerTrack || trackSize < 256 + count * sectorSize ||
      trackStart + trackSize > image.length || image.toString('ascii', trackStart, trackStart + 12) !== 'Track-Info\r\n') {
    throw new Error(`Unsupported layout or missing data on track ${track}`);
  }
  const ids = [];
  for (let index = 0; index < count; index++) {
    const descriptor = trackStart + 24 + index * 8;
    const id = image[descriptor + 2];
    if (image[descriptor + 3] !== 2 || (extended && image.readUInt16LE(descriptor + 6) !== sectorSize) ||
        ids.includes(id)) {
      throw new Error(`Unsupported sector layout on track ${track}`);
    }
    const dataStart = trackStart + 256 + index * sectorSize;
    ids.push(id);
    if (id >= 0xc1 && id <= 0xc9) {
      const logicalSector = track * sectorsPerTrack + id - 0xc1;
      sectors.set(logicalSector, image.subarray(dataStart, dataStart + sectorSize));
    }
    if (track === 3) {
      const data = image.subarray(dataStart, dataStart + sectorSize);
      if (id === 0x7b) secondStage = data;
      if (hash(data) === 'f4d021a5619202129e2c152c5a26180b9f679cc25db5ecdb28aaf5bd24c9637a') {
        alternateSecondStage = data;
      }
    }
  }
  if (count !== sectorsPerTrack || ids.some((id) => id < 0xc1 || id > 0xc9)) {
    unusualTracks.push({ track, sectorIds: ids.map((id) => id.toString(16).padStart(2, '0')) });
  }
  trackStart += trackSize;
}
secondStage ??= alternateSecondStage;
if (!secondStage) throw new Error('No supported Sorcery+ title loader found on track 3.');

function readSectors(first, count) {
  const blocks = [];
  for (let sector = first; sector < first + count; sector++) {
    const data = sectors.get(sector);
    if (!data) throw new Error(`Missing logical sector ${sector}`);
    blocks.push(data);
  }
  return Buffer.concat(blocks);
}

function readName(bytes) {
  return bytes.map((value) => value & 0x7f).toString('ascii').trimEnd();
}

const directory = readSectors(0, 4);
const entries = [];
// A repacked disk can add directory entries which the Windows build never reads.
const gameFiles = new Set([
  'TITLEP.BIN', 'CODE.BIN', 'LOCDATA.BIN', 'CONSET1.BIN', 'CONSET2.BIN',
  'SPRITES1.BIN', 'SPRITES2.BIN', 'SMESSC.BIN', 'MUCODE.BIN', 'SOUND.DAT',
  'CONALP.BIN', 'TISCRN.BIN', 'SORCERYP.BIN', 'SORCERY.BIN',
]);
for (let offset = 0; offset < directory.length; offset += 32) {
  const entry = directory.subarray(offset, offset + 32);
  if (entry[0] === 0xe5) continue;
  const name = `${readName(entry.subarray(1, 9))}.${readName(entry.subarray(9, 12))}`;
  if (entry[0] !== 0 || !gameFiles.has(name)) continue;
  if (entry[12] !== 0 || entry[14] !== 0) {
    throw new Error(`Unsupported directory extent for ${name}`);
  }

  const recordCount = entry[15];
  const allocations = [...entry.subarray(16, 32)].filter(Boolean);
  const records = Buffer.concat(allocations.map((block) => readSectors(block * 2, 2)))
    .subarray(0, recordCount * 128);
  if (records.length < 128) throw new Error(`Missing AMSDOS header in ${name}`);
  const header = records.subarray(0, 128);
  const checksum = header.subarray(0, 67).reduce((sum, value) => sum + value, 0);
  const length = header.readUIntLE(64, 3);
  if (checksum !== header.readUInt16LE(67) || length > records.length - header.length) {
    throw new Error(`Invalid AMSDOS header in ${name}`);
  }
  const amsdosFile = records.subarray(0, header.length + length);
  const payload = amsdosFile.subarray(header.length);
  const hexAddress = (value) => `0x${value.toString(16).padStart(4, '0')}`;
  const metadata = {
    name,
    length,
    fileType: header[18],
    loadAddress: hexAddress(header.readUInt16LE(21)),
    executionAddress: hexAddress(header.readUInt16LE(26)),
    systemFile: Boolean(entry[10] & 0x80),
    cpmRecords: recordCount,
    allocations,
    payloadSha256: hash(payload),
    amsdosFileSha256: hash(amsdosFile),
  };
  entries.push({ metadata, amsdosFile, payload });
}

function filePayload(name) {
  const matches = entries.filter(entry => entry.metadata.name === name);
  if (matches.length !== 1) throw new Error(`Expected one ${name} file in the disk directory.`);
  return matches[0].payload;
}

const recovered = new Map([['title-loader-4000.bin', secondStage]]);
if (entries.some(({ metadata }) => metadata.name === 'SORCERYP.BIN')) {
  const bootstrap = Buffer.from(filePayload('SORCERYP.BIN'));
  // The UK bootstrap uses its already-decoded bytes as future XOR seeds.
  for (let pass = 0; pass < 256; pass++) {
    if (pass === 0x18) continue;
    bootstrap[4] = pass;
    let value = bootstrap[pass];
    for (let index = 0x18; index < bootstrap.length; index++) {
      value ^= bootstrap[index];
      bootstrap[index] = value;
    }
  }
  bootstrap[4] = 0;
  recovered.set('SORCERYP-decoded-8000.bin', bootstrap);
}
const roomCount = 79;
const roomSectors = readSectors(31 * sectorsPerTrack, roomCount);
const roomMaps = [];
for (let room = 0; room < roomCount; room++) {
  const offset = room * sectorSize;
  if (roomSectors.toString('ascii', offset, offset + 3) !== 'DC ' || roomSectors[offset + 3] !== room) {
    throw new Error(`Unexpected header for raw room sector ${room}`);
  }
  const sector = roomSectors.subarray(offset, offset + sectorSize);
  const tiles = [];
  let index = 4;
  while (index < sector.length && sector[index] !== 0xff && tiles.length <= 720) {
    const tile = sector[index++];
    if (tile !== 0) {
      tiles.push(tile);
    } else {
      const count = sector[index++];
      tiles.push(...Array(count || 256).fill(0));
    }
  }
  if (tiles.length !== 720 || sector[index] !== 0xff) {
    throw new Error(`Invalid tile map for room ${room}`);
  }
  roomMaps.push(Buffer.from(tiles));
}
recovered.set('room-sectors-00-4e.bin', roomSectors);
recovered.set('room-tile-maps-00-4e.bin', Buffer.concat(roomMaps));
recovered.set('track-39-sector-c8.bin', readSectors(39 * sectorsPerTrack + 7, 1));
recovered.set('track-39-sector-c9-high-scores.bin', readSectors(39 * sectorsPerTrack + 8, 1));

// The second stage and main code apply NEG, RRCA, SUB $25 to file payloads.
// Destinations come from their loader tables, which reuse RAM for overlays.
const loadAddresses = {
  'CONALP.BIN': '0100',
  'CODE.BIN': '7000',
  'TISCRN.BIN': '69c0',
  'SOUND.DAT': 'a600',
  'CONSET1.BIN': '5800',
  'CONSET2.BIN': '5800',
  'SPRITES1.BIN': '0a00',
  'SPRITES2.BIN': '0a00',
  'LOCDATA.BIN': '4000',
  'SMESSC.BIN': '0a00',
  'MUCODE.BIN': '4f00',
};
for (const { metadata, payload } of entries) {
  const address = loadAddresses[metadata.name];
  if (!address) continue;
  const decoded = payload.map((byte) => {
    const negated = (-byte) & 0xff;
    const rotated = (negated >>> 1) | ((negated & 1) << 7);
    return (rotated - 0x25) & 0xff;
  });
  recovered.set(`${metadata.name.split('.')[0]}-decoded-${address}.bin`, decoded);
}

function pngChunk(type, data) {
  const length = Buffer.alloc(4);
  length.writeUInt32BE(data.length);
  const body = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const checksum = Buffer.alloc(4);
  checksum.writeUInt32BE(crc32(body));
  return Buffer.concat([length, body, checksum]);
}

function renderTileMap(tiles, tileMap, palette) {
  const width = 640;
  const height = 288;
  const scanlines = Buffer.alloc((width + 1) * height);
  // Each of the 720 tiles contains four Mode 0 pixels by eight raster lines.
  for (let y = 0; y < height; y++) {
    const raster = y >>> 1;
    for (let x = 0; x < width; x++) {
      const pixel = x >>> 2;
      const tile = tileMap[(raster >>> 3) * 40 + (pixel >>> 2)];
      const packed = tiles[(raster & 7) * 512 + (pixel & 2) * 128 + tile];
      const shift = pixel & 1;
      const leftAligned = packed << shift;
      const pen = ((leftAligned >>> 7) & 1) | ((leftAligned >>> 2) & 2)
        | ((leftAligned >>> 3) & 4) | ((leftAligned << 2) & 8);
      scanlines[y * (width + 1) + 1 + x] = pen;
    }
  }
  const levels = [0, 128, 255];
  const rgb = Buffer.from([...palette].flatMap((ink) => [
    levels[Math.floor(ink / 3) % 3],
    levels[Math.floor(ink / 9)],
    levels[ink % 3],
  ]));
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width);
  header.writeUInt32BE(height, 4);
  header[8] = 8;
  header[9] = 3;
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    pngChunk('IHDR', header),
    pngChunk('PLTE', rgb),
    pngChunk('IDAT', deflateSync(scanlines)),
    pngChunk('IEND', Buffer.alloc(0)),
  ]);
}

const title = filePayload('TITLEP.BIN');
recovered.set('title-art.png', renderTileMap(title, title.subarray(4096), secondStage.subarray(0x119, 0x129)));
const code = recovered.get('CODE-decoded-7000.bin');
const paletteOffset = secondStage === alternateSecondStage ? 0x22f8 : 0x22fc;
const roomPalette = code.subarray(paletteOffset, paletteOffset + 16);
const sampleRooms = [[0x00, 1], [0x02, 1], [0x32, 2], [0x47, 2]];
for (const [room, chapter] of sampleRooms) {
  const tiles = recovered.get(`CONSET${chapter}-decoded-5800.bin`);
  const roomId = room.toString(16).padStart(2, '0');
  recovered.set(`room-${roomId}.png`, renderTileMap(tiles, roomMaps[room], roomPalette));
}

mkdirSync(destination);
mkdirSync(join(destination, 'amsdos'));
mkdirSync(join(destination, 'payload'));
mkdirSync(join(destination, 'recovered'));
for (const { metadata, amsdosFile, payload } of entries) {
  writeFileSync(join(destination, 'amsdos', metadata.name), amsdosFile, { flag: 'wx' });
  writeFileSync(join(destination, 'payload', metadata.name), payload, { flag: 'wx' });
}
for (const [name, bytes] of recovered) {
  writeFileSync(join(destination, 'recovered', name), bytes, { flag: 'wx' });
}
const manifest = {
  diskSha256: hash(image),
  diskLength: image.length,
  format: extended ? 'Extended CPCEMU DSK' : 'Standard CPCEMU DSK',
  trackCount,
  sides: image[49],
  unusualTracks,
  files: entries.map(({ metadata }) => metadata),
  recovered: [...recovered].map(([name, bytes]) => ({ name, length: bytes.length, sha256: hash(bytes) })),
};
writeFileSync(join(destination, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n', { flag: 'wx' });
