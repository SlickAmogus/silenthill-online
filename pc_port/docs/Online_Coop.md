# Co-op

Two to eight players in one Silent Hill. The host's world is the world:
its monsters, its progress and its rules. Everyone else plays in it.

> The living world (ghosts and messages from strangers) is a separate system
> and keeps running underneath. See [Online.md](Online.md).

---

## Playing

Open **Multiplayer** on the title screen.

**Host a game.** Pick where to host:

| Host on | How friends join | Needs |
|---|---|---|
| This PC | your IP address (Join by IP) | UDP port 27888 forwarded |
| My Server | they browse the room list, or enter the 4-digit room code | nothing |
| Steam | a Steam invite from the overlay | Steam running on both PCs |

Then **Max Players**, whether the room is **Listed** or hidden (code only),
the **FPS Lock** (60 is the default, and what the sync is tuned for), and
whether to start a **New Game** or load a co-op save. **Open Room** opens it;
**Start Game** boots everyone into the host's map.

**Join a game.** Browse Server Rooms, Join by IP, Enter Room Code, or accept a
Steam invite. A guest who joins a game in progress is placed next to the host.

**In game**, `M` opens the multiplayer menu: leave a message, save, toggle
nameplates, see the players (with pings), leave. The host can also turn
**Friendly Fire** on or off for everyone, and remove a player from the
Players list (select them twice).

---

## What is shared

- **Players.** Everyone sees everyone as their real character, posed exactly
  as on their own screen, with their weapon, 60 times a second. Footsteps,
  shots, reloads and swings play from where they stand. Partners show on the
  minimap and the paper map, on the right floor.
- **Monsters.** The host runs them. Everyone sees the same monsters in the
  same places doing the same things, hears them, can shoot and hit them, and
  can be attacked by them. A kill is a kill for everyone. This covers every
  monster from the map's spawn table (including Split Head, Floatstinger and
  Twinfeeler) and the scripted ones with a spawn slot.
- **Progress.** Doors unlocked, puzzles solved and pickups taken by anyone
  are taken for everyone. The item itself goes into every player's inventory.
- **Friendly fire.** Off by default. When the host turns it on, a hit from
  another player's weapon counts, and a dead player respawns next to a
  partner after a few seconds.

**What stays yours.** Monster Cybil, the Incubus and cutscene actors run on
each machine. A monster grab on a guest lands as a hard hit instead of a
grab. Boss intros play on each machine; the boss is shared once yours is
over.

---

## How it works

Each player's game runs the full simulation for the player's own character.
The host's game is the only one that runs monster AI: it streams map chunks
and keeps collision around every partner, spawns monsters near them as it
would near itself, and sends each monster's position, pose and health about
30 times a second. A guest's game shows those as puppets in its own NPC
slots, so lighting, shadows, the radio and bullet hits all work through the
game's own code. A guest's hit on a puppet is sent to the host, which applies
it to the real monster. When a monster goes for a guest, the host runs that
monster's AI against a stand-in placed where the guest is, and sends the
guest what landed.

Messages travel over the community server relay or Steam's relay, so no
player learns another's IP address (Join by IP and This PC hosting excepted,
by nature).

---

## Logs

`SilentHill.log` tags: `[SESSION]` the room, `[COOP]` the menu and shared
items, `[FF]` friendly fire, `[NETNPC]` monster sharing (a few lines per
session: how many the host shares, when a guest makes a puppet, hits in each
direction), `[RESPAWN]`.
