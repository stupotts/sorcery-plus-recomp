"""Decode map records and turn the native room renderer's CPC video into PNGs."""

import argparse
import json
import struct
import subprocess
import zlib
from collections import Counter
from pathlib import Path

from layout import layout


ROOMS = 0x4B
CHAPTER_TWO = 0x2F
LOCK_ITEMS = {1: 0x3B, 2: 0x2B, 3: 0x29, 4: 0x38, 5: 0x44}
ENEMY_ITEMS = {1: 0x2D, 2: 0x41, 3: 0x2C}
MONSTERS = {
    1: {0x00: "Hooded head", 0x0C: "Ghost", 0x14: "Witch", 0x3C: "Flying eye", 0x58: "Horned face"},
    2: {0x00: "Skull-faced figure", 0x0C: "Green-robed figure", 0x14: "Grey-robed figure",
        0x3C: "Spinning disc", 0x58: "Winged disc"},
}


def messages(data):
    message = bytearray()
    for byte in data:
        message.append(byte & 0x7F)
        if byte & 0x80:
            yield bytes(message)
            message.clear()


def label(message, words):
    return "".join(words[byte] if byte < len(words) else chr(byte) for byte in message).strip().rstrip(",.")


def item_name(names, sprite):
    name = names.get(sprite)
    if not name:
        return f"Unidentified object (sprite {sprite:02X})"
    for prefix in ("CARRYING A ", "WITH A ", "AVEC UN ", "AVEC UNE ", "AVEC DES "):
        if name.startswith(prefix):
            return name[len(prefix):]
    return name


def chapter_for(room):
    return 1 if room < CHAPTER_TWO else 2


def hex_id(value):
    return f"{value:02X}"


def door_id(room, slot):
    return f"{hex_id(room)}-{chr(65 + slot)}"


def extract(code, locations, edition):
    def at(start, end):
        return code[start - 0x7000:end - 0x7000]

    dictionary_start, first_items, second_items, item_codes, room_titles, text_end = edition["text"]
    dictionary = list(messages(at(dictionary_start, first_items)))[1:]
    words = []
    for word in dictionary:
        words.append("".join(words[byte] if byte < len(words) else chr(byte) for byte in word).rstrip(","))
    titles = [label(record, words) for record in list(messages(at(room_titles, text_end)))[1:]]
    codes = at(item_codes, room_titles)
    items = {chapter: {code: label(record, words) for code, record in zip(codes, list(messages(data))[1:])}
             for chapter, data in [(1, at(first_items, second_items)), (2, at(second_items, item_codes))]}
    rooms = []
    for index in range(ROOMS):
        record = locations[index * 64:(index + 1) * 64]
        chapter = chapter_for(index)
        names = items[chapter]
        pickups, monsters, cauldrons, barriers, doors, notes = [], [], [], [], [], []
        for offset in (0x34, 0x37, 0x3A, 0x3D):
            y, x, sprite = record[offset:offset + 3]
            if y != 0xFF:
                pickups.append({"name": item_name(names, sprite), "x": x, "y": y,
                                "sprite": hex_id(sprite), "collectible": index != 0x46})
        for slot, offset in enumerate((2, 12)):
            state, y, x, sprite = record[offset:offset + 4]
            if state == 0xFF:
                continue
            name = MONSTERS[chapter].get(sprite, f"Unidentified enemy (sprite {sprite:02X})")
            active_state = record[offset + 9] if state == 0 else state
            weapon = item_name(names, ENEMY_ITEMS[active_state]) if active_state in ENEMY_ITEMS else None
            monsters.append({"name": name, "sprite": hex_id(sprite), "state": state, "slot": slot, "x": x, "y": y,
                             "contactWeapon": weapon})
        for offset, effect in [(0x26, "Restores energy"), (0x29, "Drains energy")]:
            y, x = record[offset:offset + 2]
            if y != 0xFF:
                cauldrons.append({"effect": effect, "x": x, "y": y})
        for offset in (0x2C, 0x2F):
            y, x, sprite = record[offset:offset + 3]
            if y != 0xFF:
                barriers.append({"requires": item_name(names, sprite), "x": x, "y": y})
        for slot in range(4):
            offset = 0x16 + 4 * slot
            y, x, target, flags = record[offset:offset + 4]
            if y == 0xFF:
                continue
            lock = flags & 0x3F
            requirement = item_name(names, LOCK_ITEMS[lock]) if lock in LOCK_ITEMS else None
            if index == 0x1C and slot == 0:
                requirement = "Free all eight sorcerers and complete the Sanctuary reunion"
            if index == 0x3A and slot == 3:
                requirement = "Sealed after the chapter transition"
            if index == 0x1C and slot == 2:
                requirement = "Seals when the Sanctuary reunion is complete"
            doors.append({"id": door_id(index, slot), "slot": slot, "x": x, "y": y,
                          "targetRoom": hex_id(target), "targetDoor": door_id(target, flags >> 6),
                          "lock": lock, "requires": requirement})
        if index == 0x3D:
            doors[0]["alternates"] = [{"x": 0, "y": y} for y in (48, 80, 112)]
            notes.append("The four left doors share one exit. Its active position changes during play.")
        if index == 0x46:
            notes.append("The pickup action is disabled in this room.")
        if index == 0x1C:
            notes.append("Freed sorcerers gather here as you rescue them. The image shows the room before the reunion.")
        # The carried-item use table includes optional room checks in its third byte.
        for offset in range(edition["item_uses"], edition["item_uses"] + 24, 6):
            sprite, _, required_room = at(offset, offset + 3)
            if index == required_room:
                notes.append(f"Room-specific use: {item_name(names, sprite)}.")
        prisoner = None if record[0x32] == 0xFF else {"y": record[0x32], "x": record[0x33]}
        rooms.append({"id": hex_id(index), "number": index + 1, "chapter": chapter,
                      "title": titles[record[0]], "doors": doors, "items": pickups,
                      "monsters": monsters, "cauldrons": cauldrons, "barriers": barriers,
                      "prisoner": prisoner, "notes": notes})

    all_doors = {door["id"]: door for room in rooms for door in room["doors"]}
    connections = []
    for room in rooms:
        for door in room["doors"]:
            start, end = door["id"], door["targetDoor"]
            reciprocal = all_doors[end]["targetDoor"] == start
            gate = {start, end} == {"1C-A", "3A-D"}
            if gate and start != "1C-A" or reciprocal and start > end:
                continue
            connections.append({"from": start, "to": end, "bidirectional": reciprocal and not gate,
                                "type": "chapter" if gate else "locked" if door["lock"] or (
                                    reciprocal and all_doors[end]["lock"]) else "open"})
    item_catalog = {chapter: {hex_id(code): item_name(names, code) for code in names if code != 0xff}
                    for chapter, names in items.items()}
    return {"rooms": rooms, "connections": connections, "itemCatalog": item_catalog}


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def png_from_vram(vram, inks):
    width, height = 320, 173
    rows = bytearray()
    for y in range(height):
        raster = y * 144 // height
        start = (raster % 8) * 2048 + (raster // 8) * 80
        rows.append(0)
        for byte in vram[start:start + 80]:
            for bits in (byte, byte << 1):
                pen = (bits >> 7 & 1) | (bits >> 2 & 2) | (bits >> 3 & 4) | (bits << 2 & 8)
                rows.extend((pen, pen))
    levels = (0, 128, 255)
    palette = bytes(component for ink in inks for component in
                    (levels[ink // 3 % 3], levels[ink // 9], levels[ink % 3]))
    header = struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"PLTE", palette)
            + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def capture(atlas, renderer, recovered, destination, palette):
    destination.mkdir(exist_ok=True)
    for room in atlas["rooms"]:
        chapter = room["chapter"]
        result = subprocess.run([renderer, room["id"], f"CONSET{chapter}-decoded-5800.bin",
                                 f"SPRITES{chapter}-decoded-0a00.bin"], cwd=recovered,
                                capture_output=True, timeout=15)
        if result.returncode:
            raise RuntimeError(f"Room {room['id']} capture failed ({result.returncode}): "
                               f"{result.stderr.decode(errors='replace').strip()}")
        if len(result.stdout) != 16384:
            raise ValueError(f"Wrong video RAM length for room {room['id']}")
        (destination / f"{room['id']}.png").write_bytes(png_from_vram(result.stdout, palette))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recovered", type=Path)
    parser.add_argument("--renderer", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    code = (args.recovered / "CODE-decoded-7000.bin").read_bytes()
    locations = (args.recovered / "LOCDATA-decoded-4000.bin").read_bytes()
    edition = json.loads((args.out / "edition.json").read_text(encoding="ascii"))
    atlas = extract(code, locations, edition)
    if args.renderer:
        palette = edition["palette"] - 0x7000
        capture(atlas, args.renderer.resolve(), args.recovered.resolve(), args.out / "rooms", code[palette:palette + 16])
    atlas["layout"] = layout(atlas["rooms"], atlas["connections"])
    (args.out / "atlas-data.json").write_text(json.dumps(atlas, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(Counter(room["chapter"] for room in atlas["rooms"]), len(atlas["connections"]), "connections")


if __name__ == "__main__":
    main()
