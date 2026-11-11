# Sorcery+ native host handoff

The addresses in this note are hexadecimal CPC addresses. Findings come from static inspection of the supplied image and its recovered code. The documented host behavior still needs runtime verification.

## Interrupt and timing path

- At `7063` the game calls `BCD7` (KL NEW FRAME FLY) with `HL=934B`, `BC=8100`, `DE=8D8B`. This registers an asynchronous near-call event. The handler at `8D8B` writes 1 to the interrupt phase byte at `92FB` and increases the logical ink values for pen 13. The first palette state to render comes from the earlier stage 2 title loader or a later `8F40` palette load.
- `7066` saves the original interrupt target by copying `word(0039)` to `9354`. The CPC464 RAM jumpblock at `0038` has `C3 39 B9`, so the target is `B939`. Seed the jumpblock or its operand with this firmware target. Stage 2 used MC START PROGRAM, which begins with `SP=C000`; the startup excerpt also expects interrupts available for the HALT loop.
- `706C–708B` writes `JP 8D59` at `5555`, fills `5400–5500` with `55`, installs `I=54` and IM 2, then enables interrupts. A host can dispatch straight to `8D59` in this state with an interrupt return address on the stack. The handler pushes registers, picks `word(word(92F9) + 2 * byte(92FB))`, calls it, increments the phase modulo six, and decrements countdown byte `6848`. Its tail at `8D83` restores registers and tail-calls `word(9354)` with the original interrupt return address still on the stack. The firmware continuation should preserve interrupted state, handle scheduled events, enable interrupts, and pop the original return address. The code switches back to IM 1 when it reads disk data.
- A 50 Hz display frame spans six CPC gate-array interrupts, nominally 300 Hz. Schedule interrupts while translated loops spin as well as during HALT. At `70AA`, `B=0` makes the startup loop wait for 256 interrupts. At `7467` the gameplay loop waits for the sign bit of `6848` and resets it to 7. At least eight interrupts elapse between game updates, separate from 50 Hz display presentation. The helper at `7BF3` spins until bit 5 of `684A` is set and clears it. It relies on the video-phase callback at `8DC1`.
- At `92A9`, code enables low ROM, disables interrupts, loads the operand of `BD19` from `BD1A`, strips the top two bits, and calls `001E`. The CPC464 operand is `87BA`, resolving to low-ROM `07BA` (MC WAIT FLYBACK). Implement `001E` as `JP (HL)`. It has another caller at `81DF` using a game actor jump table at `937E`. Handle `07BA` separately as wait for active or next flyback. The code resets `92FB` to zero after that call.
- Music entry `4F03` installs an event at `4F4A` with handler `4F51` through `BCEF`, then installs a 50 Hz ticker at `4F44` through `BCE9` with initial count 100 and repeat interval 1. The callback queries channel room through `BCAD`, builds sound blocks in RAM, and uses `BCAA` (SOUND QUEUE) and `BCB0` (SOUND ARM EVENT). Entry `4F00` deletes the ticker and resets events and sound through `BCEC`, `BCF5`, and `BCA7`. The game stops this music before loading `LOCDATA.BIN`, which overlays `4F00`.

## Video and input

| Phase | Title table at `92ED` | Gameplay table at `92E1` |
|---|---|---|
| 0 | `8DD8` joystick | `8D9D` mode 0 |
| 1 | `8DDA` idle | `8DA1` joystick |
| 2 | `8DDB` idle | `8DBD` screen effect |
| 3 | `8DDC` idle | `8DC0` idle |
| 4 | `8DDD` idle | `8DC1` frame flag, palette and mode 1 |
| 5 | `8DDE` frame flag | `8DD7` idle |

`BC0E` is firmware SCR SET MODE and clears the screen. The switches through `BD1C` in the interrupt table change only the hardware mode. Render gameplay with a raster split, retaining mode 0 and the game palette for the upper play area and using mode 1 and the phase-4 palette for the lower display. `8DC1` waits through a short loop, then invokes `BD25` with `DE=9373` and changes mode to 1. `BD25` reads hardware ink codes, while `BC32` and `BC38` accept firmware color numbers. The exact split scanline is still unverified.

`BB24` returns joystick 0 in A and H and joystick 1 in L. Up, down, left, right, Fire2 and Fire1 use bits 0, 1, 2, 3, 4 and 5. The game samples this through `8DA1`, writing `6852`. Fire2 is the start/action signal in the title and gameplay. The routine applies an additional fire mask from `684B` and clears it once fire is released. `BB1E` is key testing, with Z set when unpressed and carry clear. The game uses CPC key `3C` (S) in an alternate startup path, `3E` (C) for cheat mode if already enabled, and `2F` (Space) for the gameplay pause/resume sequence.

## Raw I/O and translated call sites

- RST `18` uses a two-byte inline descriptor pointer. It must resume after the pointer, including cases with consecutive calls. The nine descriptors written at `99A8` correspond to AMSDOS commands `81` through `89`. `84` reads a sector to HL using E as drive, D as track, and C as sector ID. On success preserve HL, set carry and return A=0. `85` writes, and `87` moves the drive head. The latter can return success without a disk read in the portable build. RST `08` has two inline address bytes as well. At `8DFF`, `CF 00 00` is the failure reset path when a required sector is unreadable or invalid.
- The second quest also reads decimal track 39 sector `C8` at `7A40`, to `5600`, then copies 288 bytes from there to `0A00` as chapter assets. Its disk offset is `2ED00` and its 512-byte SHA-256 is `BFFAC6AEDDF7B443568F9AAE00818F0E9A1F85BF887D958734A3DA20F19903C2`. This sits between the last room sector `C7` and the high scores `C9`.
- `CALL 8CFA` consumes inline text through its translated `EX (SP),HL` instructions, with the final text byte's high bit set. A normal translated CALL is sufficient. A control-flow discovery pass must skip the text after the call.
- Known runtime writes in the sprite drawing path patch instruction operands at `77FD`, `8B6E`, `8B71`, `8B74`, `8B83`, `8B86`, and `8B89`. At `8B73` and `8B88` the CALL starts with target `0000` and is patched to `8BAE` or `8BD9`. The game emits a three-byte instruction into RAM at `5555` for interrupt dispatch. These sites can use original mutable operands and a direct interrupt mapping without an instruction decoder.

## Gameplay start and room checks

The normal start path at `7372` selects one of five triplets at `966C`. It masks the changing byte at `9315` to three bits, then subtracts five from indices 5–7. Triplet fields are room ID, Y, and X. The room ID is stored at `9314`, Y at `6841`, and X at `6842`.

| Room | Y | X | Room text |
|---|---|---|---|
| `00` | `00` | `40` | Near the village |
| `18` | `00` | `4C` | Above the château |
| `1D` | `20` | `7C` | In the wastelands |
| `0F` | `38` | `74` | Near the palace |
| `16` | `30` | `7C` | In the tunnel |

The attract/demo branch at `73FC` instead draws its start and subsequent scripted input from the tables at `99C5` and `9A20`. Those begin in rooms `00` and `32`, respectively. The cheat start at `73DB` begins in `3A`.

`75AE` copies 64 bytes from `4000 + 64 * room_id` into `6800`, where IY points. It calls `8DE4` to read and validate the raw sector and unpack the 720 tile indices into `6C90`. Then it invokes the tile renderer at `8EAA` and populates per-room objects and character state. This makes `9314` and the 720 bytes at `6C90` useful checks when confirming that the translated program reaches a real gameplay room.

The normal player movement handler at `7726` consumes the sampled joystick at `6849`. Up subtracts 2 from Y when clear. With Up released, gravity adds 2 to Y when clear. Left and Right subtract or add 2 to X when clear. The room tile address helper at `8C50` computes `6C90 + 40 * floor(Y / 8) + floor(X / 4)` for a 4 × 8 Mode 0 tile. Four collision helpers near `8F60–900A` test tile bit 7 along the player's leading edge, using an extra row or column when coordinates are misaligned. The screen bounds in these helpers are Y `00–78` and X `00–94`, consistent with a 24-line by 12-pixel character within a 144-line by 160-pixel play area.

[Amstrad CPC firmware jumpblocks](https://cpctech.cpcwiki.de/docs/manual/s968se14.pdf) · [CPC464 firmware ROM disassembly](https://github.com/Richard-Lloyd/Amstrad-CPC-ROM-Disassembly/blob/main/CPC464%20Firmware%20ROM%20Disassembly%20v1.00.dis)
