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
| Server | they browse the room list, or enter the 4-digit room code | nothing |
| Steam | a Steam invite from the overlay | Steam running on both PCs |

Then **Max Players**, whether the room is **Listed** or hidden (code only),
**Living World** (Private by default: just you and your friends, no
strangers' ghosts or messages; Shown makes the game part of the living
world), the **FPS Lock** (60 is the default, and what the sync is tuned for),
and **Start From**: a New Game or any of your single-player saves. **Open
Room** opens it; **Start Game** boots everyone into the host's map.

**Join a game.** Browse Server Rooms lists every open game ("Name's Game",
where it is, how many are in it), server rooms and Steam lobbies alike; or
Join by IP, Enter Room Code, or accept a Steam invite (the friends list's
Join Game works too). A guest who joins a game in progress is placed next to
the host, with the host's game: the same inventory, health and progress. It
is one playthrough, the host's, and guests follow it.

**Saves.** Multiplayer uses your normal single-player saves; there are no
separate multiplayer saves. The host picks the save to start from (or a New
Game); guests start from wherever the host is. Everyone saves on their own
machine: a save point, the quick save key or **Save** in the `M` menu opens
the usual save screen and writes to your memory card like any other save,
host and guests alike. A guest's save is the shared game as it stood, so it
loads in single player and can host the next multiplayer game. Saving is
refused while a boss is alive, the same as the quick save.

When the host loads a save (after a game over, or from the load screen or the
quick load key), everyone starts from it: each guest's game restarts at that
save point beside the host.

**Everyone needs the same build.** A room or Steam lobby only lets in players
on its host's build; anyone else is told the host is on a different build.
New builds come out every time a change lands, so update before you play:
the launcher checks for the newest build each time you press Play and offers
it, and installs only the files that changed. The builds are on the
[Realtime release](https://github.com/SlickAmogus/silenthill-online/releases/tag/realtime).

**In game**, `M` opens the multiplayer menu: leave a message, save, change
your **Character** (any of the fourteen playable characters; the body swaps
in place and everyone sees the new one), toggle nameplates and **Health
Bars** (yours, your partners', both or none; neither shows during a scene),
see the players (with pings), leave. A guest also has **Teleport to Host**.
The host can also turn **Friendly Fire** on or off for everyone, allow
**Guest Cheats** for all guests (the quick menu's cheat and debug pages, the
console and binds are off for guests otherwise), and from the Players list
allow cheats for one guest, bring them to where the host stands, or remove
them. The lobby has the Character row too, so you can pick before the game
starts.

**Pausing** is the host's: the host's pause pauses everyone, and a guest's
pause button asks the host instead. Reading a note, a locked door message or
an item does not stop the world for anyone. The quick menu (F10) opens
without pausing.

**Voice lines.** Hold `B` (key_vocalize) for a wheel of eight short lines of
your character, point at one or press its number, let go: everyone on your
map hears it from where you stand. A folder under gamedata/voices named by
voice_pack can replace them with your own WAVs on your machine.

**Split screen.** Two players on one PC: Multiplayer, **Split Screen**.
Pick which controller is **2P Pad** (Player 1 keeps the keyboard, the mouse
and every other controller), **2P Character**, the **Layout** (Player 2
below, or side by side), what Player 2's half plays (**2P Sound**: Effects
Only by default, so the music, the voices and the movies are not heard
twice; or Everything, or Muted), **Start From** (a New Game or one of your
saves), and **Online**:

| Online | What happens |
|---|---|
| Off | The game starts as soon as Player 2's window is in. The room still lives on this PC, so a friend can join by IP, before or during the game. |
| This PC | The room lives on this PC and waits in the lobby until you press Start Game. Friends join by IP; **Online Settings** sets the player count, the room list, the living world and the FPS lock. |
| Server | The room is opened on your configured server; Player 2's window joins it there by its code, as do your friends (room list, code), and the lobby waits for Start Game. |

Then **Start Split Screen**. The game launches a second copy of itself for
Player 2 in the other half of the screen; no port forwarding is needed for
Player 2. Both halves are a normal co-op game from then on: Player 1 is the
host, Player 1's pause pauses both, and the second window closes when
Player 1 leaves to the title or quits. Player 2's window runs on Player 1's
config.cfg and never writes to it, and it stays out of the living world,
Steam, Discord and RetroAchievements, which are per PC rather than per
player. A split-screen game cannot be hosted on Steam: Player 2's window has
no Steam identity of its own.

**Lobby music.** The title screen is silent, so the multiplayer menu and a
room being waited in play the piece heard over the results screen after
the credits. It stops when the menu closes or the game starts.

**Mouse and keyboard in menus.** As soon as a key or a controller is used,
the pointer hides and stops selecting the row under it until the mouse
moves again, so a pointer left resting on a menu never drags the
selection back.

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
  Twinfeeler) and the scripted ones with a spawn slot. Grabs are real grabs:
  a Grey Child, dog, Romper or Hanged Scratcher that gets hold of a guest
  holds them until they mash free, and the host's monster lets go when they
  do.
- **Progress.** Doors unlocked, puzzles solved and pickups taken by anyone
  are taken for everyone: every player's event flags reach every other
  player, so whoever gets there first, host or guest, moves the story for
  all. The item itself goes into every player's inventory. Puzzles and
  story events inside a room are each player's to do; the result is shared.
- **Items.** Everyone carries the same things: a guest starts with the host's
  inventory and every pickup goes to all. Using an item is your own: a health
  drink heals you, a bullet is yours. Keys and puzzle items are the
  exception: using one up uses up everyone's copy, and the door it opened is
  open for all.
- **Difficulty.** With partners to lean on, monsters have 1.5 times their
  health and hit twice as hard.
- **Dying.** A death is not game over while a partner is alive. After the
  respawn delay you come back next to a living partner who is out of combat:
  no monster on them and no hit taken for a few seconds, including a couple
  of seconds after they run through a door. Until there is one the screen
  says "Waiting to respawn (players in combat)". A partner on another map is
  reached the same way, by a reboot into their map. A story scene that starts
  brings a dead player back on their feet in it. When nobody is left alive
  the host gets the game's own game over and loads a save, and everyone
  starts from that save point.
- **Friendly fire.** Off by default. When the host turns it on, a hit from
  another player's weapon counts (and bleeds where it landed), at its normal
  strength.
- **Weapons.** Partners are drawn with the weapon they hold and heard with
  its sounds, whatever you hold yourself.
- **Cutscenes.** When a player starts a scene, everyone else on the map is
  brought into it: you are placed where the scene began and it plays for
  you too, once (a scene already played on this map never pulls you back).
  A scene that starts the moment a map loads (the opening of the game,
  waking in the cafe) starts for everyone on the host's frame: the host
  waits for a partner who is still loading or still in a movie, and a guest
  waits for the host's go. Scenes end together as well: the first player
  out of one (finished, or skipped) waits on a black "Waiting for
  cutscene..." screen that lists the players on the map, grey while they
  are still in the scene or about to join it, and the game goes on when
  none is. Someone who joins the game during a scene plays it too, and the
  others wait for them at its end. In every shared scene the players sit or
  stand in the same arrangement on every screen: the host where the script
  put them, each guest a step aside, and in the cafe the first guest sits
  deeper on Harry's bench with the rest across the table. A partner on
  another map is not pulled across; they play the scene when they get
  there.
- **Story doors.** A door with a point of no return behind it (the two
  gates in the alley, the school's Door of Eclipse) belongs to the host. A
  guest who uses one stands at it under "Waiting for other players..." until
  the host goes through, and partners get a note that someone is waiting at
  a gate. The host goes through once every partner on the map is close by,
  and they all go through together in the same moment, so nobody is left a
  step short on the wrong side. If someone is far away the host waits too;
  Action takes everyone through at once, and after a minute and a half the
  host takes everyone through anyway. Cancel turns back. A guest who waits
  that long turns back on their own.
- **Level doors.** A guest cannot leave for another map while the host is
  on theirs and not beside them: the door waits under "Waiting for the
  host..." (Cancel turns back). A host who went on ahead is followed
  freely, so a guest left behind plays on in their own part of the world
  until they catch up. The host walks through every ordinary door alone.
  The alley's own progress (which gate you are past, the dark closing in)
  is each player's, so a guest keeps their cameras and their gate until
  they walk through it themselves.
- **Teleport.** A guest's multiplayer menu (`M`) has **Teleport to Host**,
  and the host's Players list has **Bring Here** for each guest. On the
  same map the player is moved at once; across maps the guest's game
  reboots into the host's map with their inventory and progress intact and
  lands beside the host.
- **Flashlights.** A partner's flashlight lights the world on your screen
  too, as a second beam from their chest along the way they face (per-pixel
  flashlight on, in the dark rooms where your own beam shows).
- **The alley.** Harry's death at the end of the opening is what takes him
  to the cafe, so in the alley one player's death is everyone's: when a
  partner falls there, you fall too, and the cafe scene starts for the whole
  room. Everywhere else a death is a respawn.

**What stays yours.** Monster Cybil, the Incubus and cutscene actors run on
each machine. Boss intros play on each machine; the boss is shared once yours
is over. A grab in a map that has no reaction animations for that monster
(one spawned from the console, for instance) lands as a hard hit.

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
guest what landed. While a monster holds a guest, each pose the guest sends
carries its state and the attack it is reacting to, so the host's monster
sees exactly what a local victim would show it.

Messages travel over the community server relay or Steam's relay, so no
player learns another's IP address (Join by IP and This PC hosting excepted,
by nature).

---

## Logs

`SilentHill.log` tags: `[SESSION]` the room, `[COOP]` the menu and shared
items, `[FF]` friendly fire, `[NETNPC]` monster sharing (a few lines per
session: how many the host shares, when a guest makes a puppet, hits in each
direction), `[RESPAWN]`.
