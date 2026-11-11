"""Create two offline atlas pages from the extracted room data and PNGs."""

import base64
import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent


def write_page(destination, template, name, data, css, details):
    source = (ROOT / template).read_text(encoding="utf-8")
    payload = json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")
    page = source.replace("/* COMMON */", css).replace("/* ROOM DETAILS */", details).replace("/* DATA */", payload)
    (destination / name).write_text(page, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, required=True, help="Directory with generated room data and images")
    parser.add_argument("--sprites-from", type=Path, required=True, help="Recovered asset directory")
    args = parser.parse_args()
    recovered = args.sprites_from
    code = (recovered / "CODE-decoded-7000.bin").read_bytes()
    inks = list(code[0x22FC:0x230C])
    sheets = [(recovered / f"SPRITES{chapter}-decoded-0a00.bin").read_bytes() for chapter in (1, 2)]
    if len(inks) != 16 or any(ink > 26 for ink in inks) or any(len(sheet) != 96 * 144 for sheet in sheets):
        raise ValueError("Unexpected room palette or sprite data")
    css = (ROOT / "common.css").read_text(encoding="utf-8")
    details = (ROOT / "room_details.js").read_text(encoding="utf-8")
    atlas = json.loads((args.data_dir / "atlas-data.json").read_text(encoding="utf-8"))
    rooms = atlas["rooms"]
    destination = args.data_dir / "deliverable"
    destination.mkdir(exist_ok=True)
    write_page(destination, "guide.html.in", "Room-Guide.html", rooms, css, details)
    for room in rooms:
        image = (args.data_dir / "rooms" / f"{room['id']}.png").read_bytes()
        room["image"] = "data:image/png;base64," + base64.b64encode(image).decode("ascii")
    atlas["previewSprites"] = {"inks": inks, "sheets": [base64.b64encode(sheet).decode("ascii") for sheet in sheets]}
    write_page(destination, "atlas.html.in", "Room-Atlas.html", atlas, css, details)


if __name__ == "__main__":
    main()
