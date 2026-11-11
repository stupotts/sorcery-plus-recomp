function roomDetails(room, byId, number, separateSprites = false) {
  const readable = text => text === text.toUpperCase()
    ? text.toLowerCase().replace(/(^|[ .-])([a-z])/g, (_, before, letter) => before + letter.toUpperCase()) : text;
  const place = point => `${point.y<48?'upper':point.y>95?'lower':'middle'} ${point.x<54?'left':point.x>101?'right':'centre'}`;
  function groupCounts(entries) {
    if (separateSprites) return entries.map(text=>({text}));
    const counts = new Map();
    for (const entry of entries) counts.set(entry,(counts.get(entry)||0)+1);
    return [...counts].map(([entry,count])=>({text:count===1?entry:`${count} × ${entry}`}));
  }

  const items = groupCounts(room.items.map(item=>readable(item.name)+(item.collectible?'':' (display only)')));
  const monsters = groupCounts(room.monsters.map(monster=>monster.name+(monster.contactWeapon?` · use ${readable(monster.contactWeapon)} at close range`:'')));
  const cauldrons = room.cauldrons.map(cauldron=>({
    text:`${cauldron.effect} · ${place(cauldron)}`, css:cauldron.effect.startsWith('Restores')?'heal':'drain'
  }));
  const doors = room.doors.filter(door=>door.requires).map(door=>({
    text:`${door.id.at(-1)} → ${number(byId.get(door.targetRoom))} ${door.targetDoor.at(-1)} · ${readable(door.requires)}`, css:'lock'
  }));
  const barriers = room.barriers.map(barrier=>({text:`Barrier (${place(barrier)}) · ${readable(barrier.requires)}`,css:'lock'}));
  const notes = [...room.notes];
  if (room.prisoner) notes.unshift(`Captured sorcerer · ${place(room.prisoner)}`);
  return [items, monsters, cauldrons, [...doors, ...barriers], notes.map(text=>({text}))];
}
