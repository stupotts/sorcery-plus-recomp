/* Development-only atlas capture. The release executable never calls this tool. */
#define main sorcery_main
#include "machine.c"
#undef main
#include <fcntl.h>
#include <io.h>

static bool call_room_routine(uint16_t entry)
{
    const uint16_t finish = 0x2000;
    push(finish);
    pc = entry;
    for (unsigned attempt = 0; attempt < 400000 && pc != finish && !stopped; ++attempt) {
        // Skip the pickup-object draw call in the original room initializer.
        if (pc == 0x7623) pc = 0x7626;
        game_step();
    }
    return pc == finish && !faults;
}

static bool read_dump(const char *path, uint8_t *data, size_t size)
{
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    bool result = fread(data, 1, size, file) == size && fgetc(file) == EOF;
    return fclose(file) == 0 && result;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    const unsigned room = strtoul(argv[1], NULL, 16);
    if (room >= 0x4b) return 2;
    uint8_t tiles[4096], sprites[13824];
    if (!read_dump(argv[2], tiles, sizeof tiles) || !read_dump(argv[3], sprites, sizeof sprites)) return 2;

    headless = true;
    if (!boot() || !valid_profile("atlas-capture")) return 3;
    injected_start = 90;
    injected_end = 140;
    while (!stopped && host_frames < 210) game_step();
    if (!player_game_active() || faults) return 4;

    memcpy(ram + 0x5800, tiles, sizeof tiles);
    memcpy(ram + 0x0a00, sprites, sizeof sprites);
    if (room >= 0x2f) RAM(0x684c) &= 0x7f;
    memset(ram + 0xc000, 0, 0x4000);
    halted = false;
    SET(IY, 0x6800);
    sp = 0xbff0;
    a = (uint8_t)room;
    if (!call_room_routine(0x75ae)) {
        fprintf(stderr, "Room %02X stopped at %04X with %llu faults\n", room, pc, (unsigned long long)faults);
        return 5;
    }

    if (RAM(0x6832) != 0xff) {
        SET(IX, 0x943c);
        if (!call_room_routine(0x8aba)) return 6;
    }
    if (_setmode(_fileno(stdout), _O_BINARY) == -1) return 7;
    return fwrite(ram + 0xc000, 1, 0x4000, stdout) == 0x4000 ? 0 : 7;
}
