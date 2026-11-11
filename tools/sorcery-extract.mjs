import { createHash } from 'node:crypto';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { crc32, deflateSync } from 'node:zlib';

const expectedHash = 'f5dee21140345ea84bee66ccf86db892c079ce13bc1d13c240a2103ba0ce2a26';
const [imagePath, destination] = process.argv.slice(2);
if (!imagePath || !destination) {
  throw new Error('Usage: node sorcery-extract.mjs <disk image> <new extraction folder>');
}

const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');
const image = readFileSync(imagePath);
if (hash(image) !== expectedHash) {
  throw new Error('This extractor is scoped to the identified Sorcery Plus disk image.');
}

const trackCount = image[48];
const trackSize = image.readUInt16LE(50);
const sectorSize = 512;
const sectorsPerTrack = 9;
const sectors = new Map();
const unusualTracks = [];
let secondStage;

for (let track = 0; track < trackCount; track++) {
  const trackStart = 256 + track * trackSize;
  const count = image[trackStart + 21];
  const ids = [];
  for (let index = 0; index < count; index++) {
    const descriptor = trackStart + 24 + index * 8;
    const id = image[descriptor + 2];
    const dataStart = trackStart + 256 + index * sectorSize;
    ids.push(id);
    if (id >= 0xc1 && id <= 0xc9) {
      const logicalSector = track * sectorsPerTrack + id - 0xc1;
      sectors.set(logicalSector, image.subarray(dataStart, dataStart + sectorSize));
    }
    if (track === 3 && id === 0x7b) {
      secondStage = image.subarray(dataStart, dataStart + sectorSize);
    }
  }
  if (count !== sectorsPerTrack || ids.some((id) => id < 0xc1 || id > 0xc9)) {
    unusualTracks.push({ track, sectorIds: ids.map((id) => id.toString(16).padStart(2, '0')) });
  }
}

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
for (let offset = 0; offset < directory.length; offset += 32) {
  const entry = directory.subarray(offset, offset + 32);
  if (entry[0] === 0xe5) continue;
  const name = `${readName(entry.subarray(1, 9))}.${readName(entry.subarray(9, 12))}`;
  if (entry[0] !== 0 || entry[12] !== 0 || entry[14] !== 0 || !/^[A-Z0-9]+\.[A-Z0-9]+$/.test(name)) {
    throw new Error(`Unexpected directory entry ${name}`);
  }

  const recordCount = entry[15];
  const allocations = [...entry.subarray(16, 32)].filter(Boolean);
  const records = Buffer.concat(allocations.map((block) => readSectors(block * 2, 2)))
    .subarray(0, recordCount * 128);
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

const bootstrap = Buffer.from(entries.find(({ metadata }) => metadata.name === 'SORCERYP.BIN').payload);
// The bootstrap uses its already-decoded bytes as future XOR seeds.
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

const recovered = new Map([
  ['SORCERYP-decoded-8000.bin', bootstrap],
  ['track-03-sector-7b-4000.bin', secondStage],
]);
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

const title = entries.find(({ metadata }) => metadata.name === 'TITLEP.BIN').payload;
recovered.set('title-art.png', renderTileMap(title, title.subarray(4096), secondStage.subarray(0x119, 0x129)));
const code = recovered.get('CODE-decoded-7000.bin');
const roomPalette = code.subarray(0x22fc, 0x230c);
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
  diskSha256: expectedHash,
  diskLength: image.length,
  format: 'Standard CPCEMU DSK',
  trackCount,
  sides: image[49],
  unusualTracks,
  files: entries.map(({ metadata }) => metadata),
  recovered: [...recovered].map(([name, bytes]) => ({ name, length: bytes.length, sha256: hash(bytes) })),
};
writeFileSync(join(destination, 'manifest.json'), JSON.stringify(manifest, null, 2) + '\n', { flag: 'wx' });
