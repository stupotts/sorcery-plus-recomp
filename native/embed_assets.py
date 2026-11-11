"""Generate read-only game assets and the launch guide for the Windows executable."""

import argparse
import json
import re
from pathlib import Path


ASSET_SIZES = {
    "CONALP-decoded-0100.bin": 2304,
    "CONSET1.BIN": 4096,
    "CONSET2.BIN": 4096,
    "LOCDATA.BIN": 5120,
    "MUCODE.BIN": 1051,
    "SOUND-decoded-a600.bin": 647,
    "SPRITES1.BIN": 13824,
    "SPRITES2.BIN": 13824,
    "TISCRN-decoded-69c0.bin": 720,
    "TITLEP.BIN": 4816,
    "room-sectors-00-4e.bin": 40448,
    "title-loader-4000.bin": 512,
    "track-39-sector-c8.bin": 512,
    "track-39-sector-c9-high-scores.bin": 512,
}
MAX_GUIDE_BYTES = 32767


def write_bytes(file, name, data):
    file.write(f"static const unsigned char {name}[] = {{\n")
    for offset in range(0, len(data), 16):
        values = ", ".join(f"0x{value:02x}" for value in data[offset:offset + 16])
        file.write(f"    {values},\n")
    file.write("};\n\n")


def generate(assets, guide, output, atlas_dir, edition, room_data):
    blob = bytearray()
    entries = []
    sizes = {**ASSET_SIZES, "CODE-decoded-7000.bin": edition["code_size"], "SMESSC.BIN": edition["message_size"]}
    for name, expected_size in sizes.items():
        data = (assets / name).read_bytes()
        if len(data) != expected_size:
            raise ValueError(f"{name}: expected {expected_size} bytes, found {len(data)}")
        entries.append(f'    {{"{name}", {len(blob)}, {len(data)}}},\n')
        blob.extend(data)

    guide_text = guide.read_text(encoding="utf-8-sig")
    guide_bytes = guide_text.replace("\n", "\r\n").encode("utf-8")
    if not guide_text.strip() or "\0" in guide_text or len(guide_bytes) > MAX_GUIDE_BYTES:
        raise ValueError(f"{guide}: expected 1–{MAX_GUIDE_BYTES} bytes of UTF-8 launch guide text without NULs")
    rooms = json.loads(room_data.read_text(encoding="utf-8"))["rooms"] if room_data else []

    with output.open("w", encoding="ascii", newline="\n") as file:
        file.write("/* Generated from the local Sorcery+ disk assets. Do not edit. */\n")
        file.write('#include "embedded_assets.h"\n#include <string.h>\n\n')
        write_bytes(file, "asset_data", blob)
        write_bytes(file, "launch_guide", guide_bytes + b"\0")
        for name, page in [("room_atlas", "Room-Atlas.html"), ("room_guide", "Room-Guide.html")]:
            data = (atlas_dir / page).read_bytes() if atlas_dir else b"\0"
            if atlas_dir and (not data.startswith(b"<!doctype html>") or len(data) > 2_000_000):
                raise ValueError(f"{page}: expected a generated offline HTML page")
            write_bytes(file, name, data)
        file.write("static const wchar_t *const room_labels[] = {\n")
        for room in rooms:
            # Match the room panel's title casing and use its public room number.
            title = re.sub(r"(^|[ .-])([a-z])", lambda match: match[0].upper(), room["title"].lower())
            label = f"#{room['number']:02d} - {title}"
            file.write(f"    [0x{room['id']}] = L{json.dumps(label)},\n")
        if not rooms:
            file.write("    NULL,\n")
        file.write("};\n\n")
        file.write("static const struct {\n    const char *name;\n    size_t offset, length;\n} assets[] = {\n")
        file.writelines(entries)
        file.write("""};

bool embedded_asset_read(const char *name, void *bytes, size_t capacity, size_t *size)
{
    for (size_t index = 0; index < sizeof(assets) / sizeof(assets[0]); ++index) {
        if (strcmp(name, assets[index].name) != 0) {
            continue;
        }
        if (assets[index].length > capacity) {
            return false;
        }
        memcpy(bytes, asset_data + assets[index].offset, assets[index].length);
        *size = assets[index].length;
        return true;
    }
    return false;
}

const char *embedded_launch_guide(size_t *size)
{
    if (size) {
        *size = sizeof(launch_guide) - 1;
    }
    return (const char *)launch_guide;
}

const unsigned char *embedded_atlas_page(bool guide, size_t *size)
{
    *size = guide ? sizeof(room_guide) : sizeof(room_atlas);
    return guide ? room_guide : room_atlas;
}

const wchar_t *embedded_room_label(unsigned room)
{
    return room < sizeof(room_labels) / sizeof(room_labels[0]) ? room_labels[room] : NULL;
}
""")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path)
    parser.add_argument("guide", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--edition-info", required=True, type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--atlas-dir", type=Path)
    mode.add_argument("--capture-only", action="store_true")
    parser.add_argument("--room-data", type=Path)
    args = parser.parse_args()
    if args.atlas_dir and not args.room_data:
        parser.error("--room-data is required when embedding the atlas")
    try:
        edition = json.loads(args.edition_info.read_text(encoding="ascii"))
        generate(args.assets, args.guide, args.output, args.atlas_dir, edition, args.room_data)
    except (OSError, ValueError) as error:
        parser.error(str(error))
