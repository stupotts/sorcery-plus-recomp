"""Create two offline atlas pages from the extracted room data and PNGs."""

import base64
import argparse
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent


def write_page(destination, template, name, data, css, details, edition):
    source = (ROOT / template).read_text(encoding="utf-8")
    payload = json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("<", "\\u003c")
    page = (source.replace("/* COMMON */", css).replace("/* ROOM DETAILS */", details)
            .replace("/* DATA */", payload).replace("/* EDITION */", edition["label"].upper()))
    (destination / name).write_text(page, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, required=True, help="Directory with generated room data and images")
    parser.add_argument("--sprites-from", type=Path, required=True, help="Recovered asset directory")
    parser.add_argument("--alternate-data", type=Path, help="Verified atlas data directory for the other language")
    args = parser.parse_args()
    edition = json.loads((args.data_dir / "edition.json").read_text(encoding="ascii"))
    recovered = args.sprites_from
    code = (recovered / "CODE-decoded-7000.bin").read_bytes()
    palette = edition["palette"] - 0x7000
    inks = list(code[palette:palette + 16])
    sheets = [(recovered / f"SPRITES{chapter}-decoded-0a00.bin").read_bytes() for chapter in (1, 2)]
    if len(inks) != 16 or any(ink > 26 for ink in inks) or any(len(sheet) != 96 * 144 for sheet in sheets):
        raise ValueError("Unexpected room palette or sprite data")
    css = (ROOT / "common.css").read_text(encoding="utf-8")
    details = (ROOT / "room_details.js").read_text(encoding="utf-8")
    atlas = json.loads((args.data_dir / "atlas-data.json").read_text(encoding="utf-8"))
    atlas["nameLanguage"] = edition["language"]
    if args.alternate_data:
        alternate = json.loads((args.alternate_data / "atlas-data.json").read_text(encoding="utf-8"))
        alternate_edition = json.loads((args.alternate_data / "edition.json").read_text(encoding="ascii"))
        if (alternate_edition["language"] == edition["language"]
                or alternate["connections"] != atlas["connections"]
                or [room["id"] for room in alternate["rooms"]] != [room["id"] for room in atlas["rooms"]]):
            raise ValueError("The alternate names must come from the other edition with matching room connections")
        atlas["alternateNames"] = alternate["rooms"]
    rooms = atlas["rooms"]
    destination = args.data_dir / "deliverable"
    destination.mkdir(exist_ok=True)
    write_page(destination, "guide.html.in", "Room-Guide.html", rooms, css, details, edition)
    for room in rooms:
        image = (args.data_dir / "rooms" / f"{room['id']}.png").read_bytes()
        room["image"] = "data:image/png;base64," + base64.b64encode(image).decode("ascii")
    atlas["previewSprites"] = {"inks": inks, "sheets": [base64.b64encode(sheet).decode("ascii") for sheet in sheets]}
    write_page(destination, "atlas.html.in", "Room-Atlas.html", atlas, css, details, edition)


if __name__ == "__main__":
    main()
