"""Check and stage the Sorcery+ data used by the Windows build."""
import argparse
from hashlib import sha256
from pathlib import Path

from editions import EDITIONS, FRENCH

# The native host relies on the addresses and data layout of this edition.
# Hash the data it uses so a different disk container can pass the same check.
PAYLOAD_SHA256 = {
    "CONSET1.BIN": {"9e5a9c1c31e72b6100ef9c6385a277f046e739d25ddc09c301491d73cda63ddf"},
    "CONSET2.BIN": "4d0cd8cb0319f64def6b90d8379e1d0ffa4b6d1d6c61d7fa48dcee1af788d8e5",
    "LOCDATA.BIN": "39b00d238a71f96d5b0ee6023992988f844f5aae39570a0918ec6efc5566cb27",
    "MUCODE.BIN": "d325fab4f2116576f1a8dffb9b613ce42adaf2fcf53d62adf67d383a19aa8eae",
    "SPRITES1.BIN": "0b60373c365e4264fc6e2b3050f5c98c501d3e26a5b9abb5bf1fc2a932fdb3a7",
    "SPRITES2.BIN": "57634a63a6ac42fd54fbb5ec691bd57e93ff1640c89942c6b62f69bbff25058e",
    "TITLEP.BIN": "07e4edaa38dbada9178df9ba4137774e33e73c14cae16d6911d88d05422d329f",
}
RECOVERED_SHA256 = {
    "CONALP-decoded-0100.bin": "ab328ec4efe372fc07e6c9c5b9ef4d1b156ffb6d8958988e2eedaf44ca57411d",
    "TISCRN-decoded-69c0.bin": "89a2fa734e3f93bd91c50b0d905d5cdfac37b8157e1e3448fbfd48f64a9c9137",
    "SOUND-decoded-a600.bin": "fda051af9da464c32ecf484dfa5ffce2592a411b6b9e6c9fd5c1dc89b662e0cb",
    "room-sectors-00-4e.bin": "0d69768b319ca1c00d7cc0dff810f34b0f6241db588dd38ddf6e53191764d663",
    "track-39-sector-c8.bin": "bffac6aeddf7b443568f9aae00818f0e9a1f85bf887d958734a3da20f19903c2",
}

SCORE_SECTOR = "track-39-sector-c9-high-scores.bin"


def read_high_scores(path):
    data = path.read_bytes()
    # The CPC writes this sector when players set new records. Check the same
    # packed decimal scores and descending order as the Windows score ledger.
    scores = [data[offset + 21:offset + 24] for offset in range(0, 7 * 24, 24)]
    if (len(data) != 512 or any((byte >> 4) > 9 or (byte & 15) > 9 for score in scores for byte in score)
            or scores != sorted(scores, reverse=True)):
        raise ValueError(f"Invalid high-score table: {path.name}")
    return data


def prepare(extraction, output):
    code = (extraction / "recovered" / "CODE-decoded-7000.bin").read_bytes()
    edition = EDITIONS.get(sha256(code).hexdigest())
    if edition is None:
        raise ValueError("Unsupported Sorcery+ game code in CODE.BIN")
    payload = {**PAYLOAD_SHA256, "SMESSC.BIN": edition.message_hash}
    if edition is FRENCH:
        payload["CONSET1.BIN"] = payload["CONSET1.BIN"] | {
            "353323579a152889586d10a6ee7d4ee8a08a2a8bb7027e8008dfc111bd7c43a1"
        }
    recovered = {**RECOVERED_SHA256, "title-loader-4000.bin": edition.loader_hash}
    staged = {"CODE-decoded-7000.bin": code}
    for folder, expected in (("payload", payload), ("recovered", recovered)):
        for name, digests in expected.items():
            path = extraction / folder / name
            if not path.is_file():
                raise ValueError(f"Missing required game data: {name}")
            data = path.read_bytes()
            allowed = (digests,) if isinstance(digests, str) else digests
            if sha256(data).hexdigest() not in allowed:
                raise ValueError(f"Incompatible game data: {name} differs from the tested edition")
            staged[name] = data
    staged[SCORE_SECTOR] = read_high_scores(extraction / "recovered" / SCORE_SECTOR)
    assets = output / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    for name, data in staged.items():
        (assets / name).write_bytes(data)
    edition.generate(output)
    print(f"Detected {edition.label}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("extraction", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        prepare(args.extraction, args.output)
    except (OSError, ValueError) as error:
        parser.error(str(error))
