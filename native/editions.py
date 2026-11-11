"""Code and data locations for the supported CPC editions."""

from dataclasses import dataclass
import json


@dataclass(frozen=True)
class Edition:
    label: str
    language: str
    code_hash: str
    code_size: int
    save_format: int
    loader_hash: str
    message_hash: str
    message_size: int
    addresses: dict
    text: tuple

    def generate(self, output):
        constants = dict(self.addresses, CODE_SIZE=self.code_size, SAVE_FORMAT=self.save_format)
        header = "#pragma once\n"
        header += "".join(f"#define SORCERY_{name} 0x{address:x}\n" for name, address in constants.items())
        (output / "edition.h").write_text(header, encoding="ascii")
        info = {"label": self.label, "language": self.language,
                "code_size": self.code_size, "message_size": self.message_size,
                "palette": self.addresses["FRAME_PHASE"] + 1,
                "item_uses": self.addresses["ITEM_USES"], "text": self.text}
        (output / "edition.json").write_text(json.dumps(info), encoding="ascii")


UK = Edition(
    "UK disk edition", "en",
    "668b2893592e8786ae4ec356bc426217d9254538b7ff561f82b4ddbe30d499d0",
    12631, 2,
    "56bb89f6484741c5b0789d1afe433d754876afdaa063fa3b894b03f497aca0e7",
    "958cbc391d9fb7b54ecd36e28d42ea865e6676ae481e0529e4bbfea49fcc4faa",
    4957,
    dict(FRAME_TABLE_POINTER=0x92F9, GAMEPLAY_FRAME_TABLE=0x92E1, FRAME_PHASE=0x92FB,
         ROOM_NUMBER=0x9314, HUD_PALETTE=0x9374, HUD_PALETTE_RETURN=0x8DD2,
         HIGH_SCORES=0x9900, COMPARE_SCORE=0x882B,
         OPTIONAL_SPLASH=0x7167, AFTER_SPLASH=0x71A5, GAME_LOOP=0x7467, ROOM_READY=0x74B9,
         ROOM_ENTRY=0x75AE, PICKUP_DRAW=0x7623, AFTER_PICKUP_DRAW=0x7626,
         PRISONER_SPRITE=0x943C, DRAW_SPRITE=0x8ABA, ITEM_USES=0x9446,
         SANCTUARY_ACTORS=0x95EB, SANCTUARY_SLOTS=0x962B,
         DOOR_START=0x7985, DOOR_OPENING=0x7998, DOOR_ENTERING=0x79B9,
         DOOR_DESTINATION=0x7A94, DOOR_OPEN=0x7B15, DOOR_LEAVING=0x7B2D,
         DOOR_CLOSING=0x7B54, DOOR_REDRAW=0x7BEB),
    (0x9A55, 0x9AE7, 0x9BE1, 0x9CDD, 0x9CF0, 0x9E78),
)

FRENCH = Edition(
    "French disk edition", "fr",
    "b3f3fe02a4409dae20b7c7faad508500baf05bc264ba4c172aaaa02484fdb4ad",
    12618, 3,
    "f4d021a5619202129e2c152c5a26180b9f679cc25db5ecdb28aaf5bd24c9637a",
    "084441ab6e2c42b38f0d92e5e441554454d7f6a63d880ed694a259ab90540115",
    4426,
    dict(FRAME_TABLE_POINTER=0x92F5, GAMEPLAY_FRAME_TABLE=0x92DD, FRAME_PHASE=0x92F7,
         ROOM_NUMBER=0x9310, HUD_PALETTE=0x9370, HUD_PALETTE_RETURN=0x8DCE,
         HIGH_SCORES=0x98EF, COMPARE_SCORE=0x8827,
         OPTIONAL_SPLASH=0x7174, AFTER_SPLASH=0x71B2, GAME_LOOP=0x745D, ROOM_READY=0x74AF,
         ROOM_ENTRY=0x75A4, PICKUP_DRAW=0x7619, AFTER_PICKUP_DRAW=0x761C,
         PRISONER_SPRITE=0x9438, DRAW_SPRITE=0x8AB6, ITEM_USES=0x9442,
         SANCTUARY_ACTORS=0x95E7, SANCTUARY_SLOTS=0x9627,
         DOOR_START=0x797B, DOOR_OPENING=0x798E, DOOR_ENTERING=0x79AF,
         DOOR_DESTINATION=0x7A8A, DOOR_OPEN=0x7B0B, DOOR_LEAVING=0x7B23,
         DOOR_CLOSING=0x7B4A, DOOR_REDRAW=0x7BE1),
    (0x9A44, 0x9AC8, 0x9BB9, 0x9CA4, 0x9CB7, 0x9EA3),
)


EDITIONS = {edition.code_hash: edition for edition in (UK, FRENCH)}
