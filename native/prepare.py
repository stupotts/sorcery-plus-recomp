"""Check and stage the Sorcery+ data used by the Windows build."""
import argparse
from hashlib import sha256
from pathlib import Path


# The native host relies on the addresses and data layout of this edition.
# Hash the data it uses so a different disk container can pass the same check.
PAYLOAD_SHA256 = {
    "CONSET1.BIN": "9e5a9c1c31e72b6100ef9c6385a277f046e739d25ddc09c301491d73cda63ddf",
    "CONSET2.BIN": "4d0cd8cb0319f64def6b90d8379e1d0ffa4b6d1d6c61d7fa48dcee1af788d8e5",
    "LOCDATA.BIN": "39b00d238a71f96d5b0ee6023992988f844f5aae39570a0918ec6efc5566cb27",
    "MUCODE.BIN": "d325fab4f2116576f1a8dffb9b613ce42adaf2fcf53d62adf67d383a19aa8eae",
    "SMESSC.BIN": "958cbc391d9fb7b54ecd36e28d42ea865e6676ae481e0529e4bbfea49fcc4faa",
    "SPRITES1.BIN": "0b60373c365e4264fc6e2b3050f5c98c501d3e26a5b9abb5bf1fc2a932fdb3a7",
    "SPRITES2.BIN": "57634a63a6ac42fd54fbb5ec691bd57e93ff1640c89942c6b62f69bbff25058e",
    "TITLEP.BIN": "07e4edaa38dbada9178df9ba4137774e33e73c14cae16d6911d88d05422d329f",
}
RECOVERED_SHA256 = {
    "CODE-decoded-7000.bin": "668b2893592e8786ae4ec356bc426217d9254538b7ff561f82b4ddbe30d499d0",
    "CONALP-decoded-0100.bin": "ab328ec4efe372fc07e6c9c5b9ef4d1b156ffb6d8958988e2eedaf44ca57411d",
    "TISCRN-decoded-69c0.bin": "89a2fa734e3f93bd91c50b0d905d5cdfac37b8157e1e3448fbfd48f64a9c9137",
    "SOUND-decoded-a600.bin": "fda051af9da464c32ecf484dfa5ffce2592a411b6b9e6c9fd5c1dc89b662e0cb",
    "room-sectors-00-4e.bin": "0d69768b319ca1c00d7cc0dff810f34b0f6241db588dd38ddf6e53191764d663",
    "track-03-sector-7b-4000.bin": "56bb89f6484741c5b0789d1afe433d754876afdaa063fa3b894b03f497aca0e7",
    "track-39-sector-c8.bin": "bffac6aeddf7b443568f9aae00818f0e9a1f85bf887d958734a3da20f19903c2",
    "track-39-sector-c9-high-scores.bin": "f283b9d7f8ad3e65945f0d7c78933d0793efc344458546c29ecd904ac747ac22",
}


def prepare(extraction, output):
    staged = {}
    for folder, expected in (("payload", PAYLOAD_SHA256), ("recovered", RECOVERED_SHA256)):
        for name, digest in expected.items():
            path = extraction / folder / name
            if not path.is_file():
                raise ValueError(f"Missing required game data: {name}")
            data = path.read_bytes()
            if sha256(data).hexdigest() != digest:
                raise ValueError(f"Incompatible game data: {name} differs from the tested edition")
            staged[name] = data
    assets = output / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    for name, data in staged.items():
        (assets / name).write_bytes(data)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("extraction", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        prepare(args.extraction, args.output)
    except (OSError, ValueError) as error:
        parser.error(str(error))
