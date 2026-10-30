# Silent Hill Online

Silent Hill Online (SH:O) is the multiplayer edition of the Silent Hill native PC port. It plays the same game as the single-player port and adds two ways to play with other people: a shared **living world** you see while you play on your own, and **co-op** through the story for 2 to 4 players.

Project website: https://sh1pc.com/ <br/>
Discord: https://discord.gg/JWuNzVsQbr

> **Early days.** Online is new, so expect bugs, the odd jarring moment and the occasional soft lock. If you get stuck, the host can load a save and bring everyone along. Every platform gets realtime updates, so fixes usually land within hours. Most don't need the server restarted, and when one does, you can get back in quickly.

## Get it

| Platform | Where |
|---|---|
| Windows, Android, iOS | [Realtime builds](https://github.com/SlickAmogus/silenthill-online/releases/tag/realtime), updated with every change |
| Web browser | [sh1pc.com/playonline](https://sh1pc.com/playonline/) |
| Mac, Linux | Not yet |

The browser version can join the living world and host or join rooms on the server. Browsers can't open direct connections or Steam lobbies, so those two options are desktop and phone only.

You need your own copy of Silent Hill for the PlayStation. Put your disc image (`.bin`) in the app's `gamedata` folder. Nothing from the game is included here.

## Living world

Play on your own and you're connected to the public SH1PC community server by default. Other players in the same area show up as ghosts. You can chat with them, leave messages for people who pass through later, and invite the nearest player into a co-op game. They can't affect your game otherwise. They're company, not a hazard. More is planned over time.

Chat has four channels: **Global**, **Level** (your area), **Coop** (your co-op room) and **All**, which shows everything together. On a keyboard, `Y` opens the chat box and `Tab` switches channel. On a touchscreen, use the chat button at the top of the screen. You can hide the chat and join messages in **Multiplayer Settings**.

## Co-op (2 to 4 players)

Co-op plays the story together, start to finish. Expect quirks. Cutscenes place your partners in the scene where they can, but in some scenes the other players can still walk around freely.

### Hosting

Open **Multiplayer** from the main menu, then **Host Game**. Pick one of three ways to host with the **Host on** option:

- **Server** (easiest). Opens a room on the community server, so nobody needs to forward a port. Choose 2 to 4 players, whether the room is listed publicly, and whether the game also shows in the living world. For a private game, leave the room unlisted and give your friends its room code.
- **This PC** (direct connection). Your PC hosts the game. Forward UDP port **27888** on your router. Friends join by IP with your address and port, for example `55.55.55.55:27888`. A hostname works too. The living world and room settings don't apply in this mode.
- **Steam**. Add `SilentHillPC.exe` to Steam as a non-Steam game and start it from Steam. Steam will show you as playing Spacewar, which is expected. Choose Steam under **Host on**, then invite friends through the Steam overlay.

The host also sets **Enemy Damage** and **Enemy Health**, each from 0.25x to 4x in steps of 0.25. Both default to 1.5x.

### Joining

Open **Multiplayer**, then **Join Game**, and use the same three routes from the other side:

1. **Join by IP** with your friend's address and port.
2. **Browse Server Rooms**, or enter the room code your friend gave you.
3. Accept your friend's Steam invite while you're in the game.

### How co-op plays

- **Shared progress.** Everyone starts with a copy of the host's inventory, and anything one player picks up goes to everyone. Ammo and items are your own: each player has their own ammo count and uses their own items.
- **Story moments.** You can't run ahead into a major story event alone. The game either waits for the others or brings everyone along.
- **Saving and loading.** Anyone can save. The host can also load a save from the multiplayer menu (`M` on a keyboard), and everyone starts again from it. That's the way out of a soft lock.
- **Dying.** A player who dies respawns next to a partner who is alive and not in a fight. If everyone dies, everyone sees the game over, and the host chooses to continue, load a save or quit.
- **Voice lines.** Hold `B`, or use the voice button on a touchscreen, for a wheel of short lines your character says to the room.
- **No pausing** while others are in your game. The world keeps running even with your inventory or map open.

More detail is in the [co-op guide](pc_port/docs/Online_Coop.md), the [living world guide](pc_port/docs/Online.md) and the [Steam guide](pc_port/docs/Online_Steam.md).

## Additional characters

Partners see your custom character if they have the same character installed. Anyone who doesn't sees you as Harry. A set of extra characters is ready to install:

- **PC:** the launcher's Mod Manager, under Essential QoL Mods.
- **Android, iOS and web:** Essential QoL Mods in the game's options.

Character packs update themselves when a new build comes out.

## Hosting your own server

The living world doubles as the master server that hosts rooms, which is why rooms need no port forwarding. Anyone can run their own, even just to give their group easy rooms without the living world. The default server is `server.sh1pc.com`. You can point the game at any other server with **Living World IP** in Multiplayer Settings, and hostnames work.

The [server release](https://github.com/SlickAmogus/silenthill-online/releases/tag/server) always has the current build for Windows and Linux. It includes `Redeploy-Server.bat`, which installs or updates the server on a Linux machine over ssh as a service that restarts itself. `Server-Console.bat` opens the admin console, and `Start-Server.bat` runs the server on a Windows PC. Full instructions are in [online_server/host/README.md](online_server/host/README.md).

This is the server for rooms and the living world. For a direct co-op game you don't need one: host on **This PC** instead.

## Reporting bugs

Bug reports are very welcome, on the Discord or as GitHub issues. Please attach your `SilentHill.log`. Logging is on by default, and the log is usually the fastest way to find a problem.

This is a passion project, not a job, and it has been built mostly non-stop. Please be patient if a fix takes a little while.

## Building

The build is the same as the base port: MSYS2 / MinGW64, CMake + Ninja, and the PsyCross submodule. The one online-specific requirement is `steam_api64.dll` (Steamworks SDK, `redistributable_bin/win64`) next to the game exe for Steam co-op. Without it the game still runs, and the Multiplayer menu reports Steam as unavailable.

This is a 100% free project funded from my own pocket. If you'd like to support me, it is greatly appreciated, and you can do so on Ko-fi:<br/> [![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/F1F3K8V3B)

Silent Hill is © Konami and this does not contain any game assets. You must provide a legally obtained dump of Silent Hill for PSX to use.
