"""Verify the local atlas data, room images and offline browser controls."""

import argparse
import json
import subprocess
from collections import Counter
from pathlib import Path


PNG_HEADER = b"\x89PNG\r\n\x1a\n\x00\x00\x00\x0dIHDR\x00\x00\x01\x40\x00\x00\x00\xad"

ATLAS_CHECKS = """
check(document.querySelectorAll('.room').length === 75, '75 room cards');
check(document.querySelectorAll('#map image').length === 75, '75 embedded images');
check(document.querySelectorAll('#map path[marker-end]').length >= 2, 'one-way and chapter arrows');
check(document.querySelector('#room-panel h2').textContent.includes('Village'), 'default village selection');
const divider = document.getElementById('panel-divider');
const preview = document.querySelector('#room-panel img');
const originalImage = preview.getBoundingClientRect();
divider.setPointerCapture = () => {}; // Synthetic events do not register a real pointer.
const pointer = (type,x) => divider.dispatchEvent(new PointerEvent(type,{pointerId:1,clientX:x,button:0}));
pointer('pointerdown',900);
pointer('pointermove',740);
pointer('pointerup',740);
const largerImage = preview.getBoundingClientRect();
check(largerImage.width === originalImage.width+160 && largerImage.height > originalImage.height, 'drag widens panel and image proportionally');
divider.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowRight'}));
check(preview.getBoundingClientRect().width === largerImage.width-32, 'keyboard narrows panel and image');
const toggles = [...document.querySelectorAll('#room-panel input[type=checkbox]')];
const spriteCanvas = document.querySelector('#room-panel canvas');
check(toggles.length === 4 && spriteCanvas.hidden, 'individual object and monster controls start unchecked');
await preview.decode();
const base = document.createElement('canvas');
base.width=320; base.height=173;
const background = base.getContext('2d');
background.drawImage(preview,0,0);
const pixels = background.getImageData(0,0,320,173).data;
async function changedBounds(toggle) {
  toggle.click();
  await preview.decode();
  const output = spriteCanvas.getContext('2d').getImageData(0,0,320,173).data;
  const changed = [];
  for (let i=0; i<output.length; i+=4) {
    if (output[i]!==pixels[i] || output[i+1]!==pixels[i+1] || output[i+2]!==pixels[i+2]) {
      changed.push([i/4%320,Math.floor(i/1280)]);
    }
  }
  return changed;
}
const itemPixels = await changedBounds(toggles[0]);
check(itemPixels.length>0 && itemPixels.every(([x,y])=>x>=232 && x<256 && y>=10 && y<39), 'shooting star drawn at original starting coordinates');
toggles[0].click();
await preview.decode();
check(spriteCanvas.hidden, 'unchecking restores plain image');
const monsterPixels = await changedBounds(toggles[2]);
check(monsterPixels.length>0 && monsterPixels.every(([x,y])=>x>=296 && x<320 && y>=58 && y<87), 'monster drawn at original starting coordinates');
toggles[0].click();
await preview.decode();
check(toggles[0].checked && toggles[2].checked && !spriteCanvas.hidden, 'object and monster can be shown together');
document.querySelector('#room-panel .door-list button:nth-child(2)').click();
check(location.hash === '#room-27', 'village to wine cellar');
check(document.querySelector('#room-panel a').getAttribute('href') === 'Room-Guide.html#room-27', 'matching guide link');
const details = () => document.querySelector('#room-panel .room-details');
check(details().querySelectorAll('h3').length === 5, 'five room detail categories');
check(details().textContent.includes('Captured sorcerer') && details().textContent.includes('Restores energy'), 'wine cellar notes and cauldron');
check(details().querySelector('.heal'), 'restorative cauldron styling');
document.getElementById('search').value = '#05';
document.getElementById('find').click();
check(details().textContent.includes('Strong Sword') && details().textContent.includes('Witch') && details().textContent.includes('Door Key'), 'castle objects, monsters and barrier');
check(!details().textContent.includes('Captured sorcerer'), 'previous room details replaced');
document.getElementById('search').value = '#54';
document.getElementById('find').click();
check(details().querySelector('.drain')?.textContent.includes('Drains energy'), 'draining cauldron styling');
document.getElementById('search').value = '#62';
document.getElementById('find').click();
check(location.hash === '#room-3D', 'room number search');
check(document.querySelectorAll('.wire.alternate').length === 6, 'moving exit branches in base and selected links');
const before = document.getElementById('map').getAttribute('viewBox');
document.getElementById('plus').click();
check(before !== document.getElementById('map').getAttribute('viewBox'), 'zoom button');
document.getElementById('chapter2').click();
check(document.querySelectorAll('.room.muted-room').length === 47, 'chapter focus');
document.getElementById('fit').click();
check(!document.querySelector('.room.muted-room'), 'all rooms');
showRoom(byId.get('00'));
const objectSelectControl = document.getElementById('object-search');
objectSelectControl.value = 'STRONG SWORD';
objectSelectControl.dispatchEvent(new Event('change'));
const matchesForObject = () => [...document.querySelectorAll('.object-matches button')];
check(matchesForObject().map(button=>button.dataset.room).join(',')==='00,27,26,07,02,14,29,04,16,1A,1E', 'sword rooms ranked by door distance');
check(matchesForObject()[0].textContent.includes('In this room') && matchesForObject()[1].textContent.includes('1 room away'), 'current and adjacent distances');
matchesForObject()[1].click();
check(location.hash==='#room-27' && cards.get('27').classList.contains('selected'), 'result selects and highlights room');
check(matchesForObject()[0].dataset.room==='00' && matchesForObject()[1].getAttribute('aria-current')==='true', 'results retain search origin');
const chooseObject = value => {
  objectSelectControl.value = value;
  objectSelectControl.dispatchEvent(new Event('change'));
};
check(!document.getElementById('find-object'), 'object dropdown searches without a separate button');
chooseObject('');
chooseObject('STRONG SWORD');
check(matchesForObject().map(button=>button.dataset.room).join(',')==='27,00,26,07,02,29,04,14,1A,16,1E', 'new search starts from the selected room');
const mapViewport = document.getElementById('viewport');
const leftSidebar = document.getElementById('search-panel');
const rightSidebar = document.getElementById('room-panel');
const searchToggle = document.getElementById('toggle-search');
const detailsToggle = document.getElementById('toggle-details');
check(leftSidebar.contains(matchesForObject()[0]) && leftSidebar.getBoundingClientRect().right <= mapViewport.getBoundingClientRect().left, 'search and results are left of the map');
const openWidth = mapViewport.clientWidth;
searchToggle.click();
check(leftSidebar.hidden && !rightSidebar.hidden && mapViewport.clientWidth > openWidth && searchToggle.getAttribute('aria-expanded')==='false', 'hide search independently and reclaim map width');
const searchHiddenWidth = mapViewport.clientWidth;
detailsToggle.click();
check(rightSidebar.hidden && divider.hidden && mapViewport.clientWidth > searchHiddenWidth, 'hide room details and divider');
searchToggle.click();
check(!leftSidebar.hidden && rightSidebar.hidden && matchesForObject()[0].dataset.room==='27', 'search reopens with results intact');
detailsToggle.click();
check(!rightSidebar.hidden && !divider.hidden && location.hash==='#room-27' && mapViewport.clientWidth===openWidth, 'details reopen at their prior width and selected room');
check(focusLayer.children.length===4 && document.querySelectorAll('.room.connected').length===3, 'all cellar doors and destinations highlighted');
check(wires.filter(wire=>wire.classList.contains('muted-wire')).length===93, 'unrelated door trails dimmed');
showRoom(byId.get('3A'));
chooseObject('');
chooseObject('STRONG SWORD');
check(matchesForObject().every(button=>button.textContent.includes('No route')), 'one-way chapter gate respected');
objectSelectControl.value = '';
objectSelectControl.dispatchEvent(new Event('change'));
const live = document.getElementById('live-update');
check(live.disabled, 'standalone page explains that live mode requires the game');
showRoom(byId.get('00'));
live.checked = true;
const sample = {active:true,room:'00',player:{x:76,y:92,sprite:'20'},freed:['27'],sanctuary:[{x:4,y:104,sprite:'54'}],rooms:{
  '00':{items:[{slot:0,x:40,y:60,sprite:'41'}],monsters:[{slot:1,x:50,y:40,sprite:'0C'}],prisoner:false},
  '27':{items:[],monsters:[],prisoner:false},
  '1C':{items:[],monsters:[],prisoner:false}
}};
applyLiveState(sample);
check(details().textContent.includes('Live objects') && details().textContent.includes('Strong Sword') && !details().textContent.includes('Shooting Star'), 'live inventory replaces default inventory');
check(details().textContent.includes('Live monsters') && !details().textContent.includes('Hooded head'), 'defeated actor absent from live room');
check(liveRoom(byId.get('00')).monsters[0].x===50, 'live actor position');
const playerFrame = document.querySelector('.player-frame');
const atlasPosition = id => `translate(${positions[id].join(',')})`;
check(playerFrame.getAttribute('visibility')==='visible' && playerFrame.getAttribute('transform')===atlasPosition('00'), 'ornate border follows current player room');
const livePhoto = document.querySelector('#room-panel img');
await livePhoto.decode();
const liveCanvas = document.querySelector('#room-panel canvas');
const liveContext = liveCanvas.getContext('2d');
const playerPixels = liveContext.getImageData(152,112,24,27).data;
const playerMoved = {...sample,player:{...sample.player,x:96}};
applyLiveState(playerMoved);
await livePhoto.decode();
check(liveContext.getImageData(152,112,24,27).data.some((pixel,index)=>pixel!==playerPixels[index]), 'preview draws player and updates moving position');
showRoom(byId.get('27'));
check(playerFrame.getAttribute('transform')===atlasPosition('00') && !liveRoom(byId.get('27')).player, 'selecting another room leaves the player border in place');
check(details().textContent.includes('Sorcerer freed.') && liveRoom(byId.get('27')).extraSprites.length===1, 'freed prisoner removed from the preview');
applyLiveState({...sample,room:'27'});
check(playerFrame.getAttribute('transform')===atlasPosition('27'), 'border moves on a live room transition');
showRoom(byId.get('1C'));
check(details().textContent.includes('1 of 8 sorcerers freed') && liveRoom(byId.get('1C')).extraSprites.length===1, 'Sanctuary shows freed sorcerers');

// Room 16-B and room 19-D have separate Coat of Arms locks on one route.
const lockIndex = connections.findIndex(edge => edge.from==='16-B' && edge.to==='19-D');
const selectedTrail = () => focusLayer.querySelector(`[data-edge="${lockIndex}"]`);
const lockHint = () => [...document.querySelectorAll('.door-list button')]
  .find(button=>button.textContent.startsWith('B →')).querySelector('small');
const withDoorLocks = (near,far) => ({...sample,rooms:{...sample.rooms,
  '16':{items:[],monsters:[],prisoner:false,doorLocks:[0,near,null,null]},
  '19':{items:[],monsters:[],prisoner:true,doorLocks:[0,0,0,far]}
}});
showRoom(byId.get('16'));
check(lockHint().textContent.includes('COAT OF ARMS') && selectedTrail().dataset.type==='locked', 'default and older live data show original door lock');
applyLiveState(withDoorLocks(0,1));
check(lockHint().textContent==='Unlocked' && selectedTrail().dataset.type==='locked', 'unlocked departure is visible while the opposite lock remains');
check(!details().textContent.includes('Coat Of Arms'), 'unlocked door leaves the requirements list');
applyLiveState(withDoorLocks(0,0));
check(wires[lockIndex].dataset.type==='open' && selectedTrail().dataset.type==='open' &&
      getComputedStyle(selectedTrail().querySelector('.trail')).strokeDasharray==='none', 'both directions unlocked turn the map and highlighted trail green');
check(wires[connections.findIndex(edge=>edge.type==='chapter')].dataset.type==='chapter', 'chapter gate keeps its own trail style');
live.checked=false;
applyLiveState(null);
check(lockHint().textContent.includes('COAT OF ARMS') && selectedTrail().dataset.type==='locked', 'disabling live mode restores the original lock and trail');
check(playerFrame.getAttribute('visibility')==='hidden', 'turning off live mode hides the player border');
showRoom(byId.get('00'));
check(details().textContent.includes('Objects at the start') && details().textContent.includes('Shooting Star'), 'unchecking restores starting state');
const setNote = async text => {
  await notesReady;
  const editor = document.querySelector('.personal-notes textarea');
  editor.value = text;
  editor.dispatchEvent(new Event('input'));
  check(document.querySelector('.note-status').textContent==='Unsaved changes.', 'typing leaves a draft');
  document.querySelector('.personal-notes button').click();
  check(document.querySelector('.note-status').textContent==='Saved in this browser.', 'Save note persists the draft');
};
showRoom(byId.get('27'));
await setNote('Wine cellar: use the cauldron.');
showRoom(byId.get('00'));
await setNote('Village: bring the sword.');
showRoom(byId.get('27'));
check(document.querySelector('.personal-notes textarea').value==='Wine cellar: use the cauldron.', 'notes follow the selected room');
"""

NOTES_REOPEN_CHECKS = """
const editor = () => document.querySelector('.personal-notes textarea');
check(editor().value==='Village: bring the sword.', 'village note survived browser restart');
showRoom(byId.get('27'));
check(editor().value==='Wine cellar: use the cauldron.', 'cellar note survived browser restart');
"""

GUIDE_CHECKS = """
check(document.querySelectorAll('#rows tr').length === 75, '75 guide rows');
const cellar = document.getElementById('room-27');
check(cellar.textContent.includes('Wine Cellar') && cellar.textContent.includes('Captured sorcerer') && cellar.textContent.includes('Restores energy'), 'wine cellar notes');
check(cellar.querySelector('a').getAttribute('href') === 'Room-Atlas.html#room-27', 'matching atlas link');
const cauldrons = document.getElementById('cauldrons');
cauldrons.checked = true;
cauldrons.dispatchEvent(new Event('input'));
check(document.querySelectorAll('#rows tr:not([hidden])').length === 11, 'cauldron filter');
cauldrons.checked = false;
const search = document.getElementById('filter');
search.value = 'fleur de lys';
search.dispatchEvent(new Event('input'));
const matches = [...document.querySelectorAll('#rows tr:not([hidden])')];
check(matches.length > 0 && matches.every(row => row.textContent.toLowerCase().includes('fleur de lys')), 'search filter');
"""


def verify_data(root):
    atlas = json.loads((root / "atlas-data.json").read_text(encoding="utf-8"))
    rooms, links = atlas["rooms"], atlas["connections"]
    assert [room["id"] for room in rooms] == [f"{index:02X}" for index in range(75)]
    assert Counter(room["chapter"] for room in rooms) == {1: 47, 2: 28}
    assert sum(bool(room["prisoner"]) for room in rooms) == 8
    assert Counter(cauldron["effect"] for room in rooms for cauldron in room["cauldrons"]) == {
        "Restores energy": 10, "Drains energy": 1}
    assert len(links) == len(atlas["layout"]["paths"]) == 97
    doors = {door["id"]: door for room in rooms for door in room["doors"]}
    assert len(doors) == 193
    for edge, points in zip(links, atlas["layout"]["paths"]):
        if (edge["from"], edge["to"]) == ("11-B", "13-D"):
            assert len(points) == 2 and points[0][1] == points[1][1], "Tunnel doors should share a straight link"
        card = atlas["layout"]["card"]
        for first, second in zip(points, points[1:]):
            horizontal = first[1] == second[1]
            assert horizontal or first[0] == second[0], edge
            assert abs(first[0] - second[0]) + abs(first[1] - second[1]) >= 8, edge
            axis = 0 if horizontal else 1
            low, high = sorted((first[axis], second[axis]))
            size = (card["width"], card["height"])
            for room, position in atlas["layout"]["positions"].items():
                if room in (edge["from"][:2], edge["to"][:2]):
                    continue
                across = 1 - axis
                assert not (low < position[axis]+size[axis] and high > position[axis] and
                            position[across] < first[across] < position[across]+size[across]), (edge, room)
        for endpoint, point in (("from", points[0]), ("to", points[-1])):
            door = doors[edge[endpoint]]
            left, top = atlas["layout"]["positions"][door["id"][:2]]
            card = atlas["layout"]["card"]
            x = left + (door["x"] + 6) * card["width"] / 160
            y = top + card["imageY"] + (door["y"] + 12) * card["imageHeight"] / 144
            assert abs(point[0] - x) <= .051 and abs(point[1] - y) <= .051
        assert doors[edge["from"]]["targetDoor"] == edge["to"]
        if edge["bidirectional"]:
            assert doors[edge["to"]]["targetDoor"] == edge["from"]
    for room in rooms:
        assert (root / "rooms" / f"{room['id']}.png").read_bytes().startswith(PNG_HEADER)
    print("Verified 75 rooms, 75 images, 193 door records, 97 paths and 8 prisoner records.")


def browser_test(browser, scratch, source, checks, reopen=False):
    instrument = """
<script>
const check = (condition, label) => { if (!condition) throw Error(label); };
(async()=>{
try {
%s
document.body.dataset.atlasQa = 'PASS';
} catch (error) { document.body.dataset.atlasQa = 'FAIL ' + error; }
})();
</script>
""" % checks
    html = source.read_text(encoding="utf-8")
    page = scratch / source.name
    page.write_text(html.replace("</html>", instrument + "</html>"), encoding="utf-8")
    args = [str(browser), "--headless", "--disable-gpu", "--disable-background-networking", "--no-first-run",
            f"--user-data-dir={scratch / 'browser-profile'}", "--window-size=1440,900"]
    if not reopen:
        subprocess.run([*args, f"--screenshot={scratch / (page.stem + '.png')}", source.as_uri()],
                       capture_output=True, check=True, timeout=35)
    result = subprocess.run([*args, "--virtual-time-budget=1500", "--dump-dom", page.as_uri()], capture_output=True, check=True, timeout=35)
    assert b'data-atlas-qa="PASS"' in result.stdout, result.stdout.split(b'data-atlas-qa="')[-1][:120]
    print(f"Offline controls passed in {source.name}.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--browser", type=Path)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--scratch", type=Path)
    args = parser.parse_args()
    root = args.data_dir.resolve()
    verify_data(root)
    if args.browser:
        scratch = args.scratch.resolve() if args.scratch else root / "verification"
        scratch.mkdir(parents=True, exist_ok=True)
        pages = root / "deliverable"
        browser_test(args.browser, scratch, pages / "Room-Atlas.html", ATLAS_CHECKS)
        browser_test(args.browser, scratch, pages / "Room-Atlas.html", NOTES_REOPEN_CHECKS, reopen=True)
        browser_test(args.browser, scratch, pages / "Room-Guide.html", GUIDE_CHECKS)


if __name__ == "__main__":
    main()
