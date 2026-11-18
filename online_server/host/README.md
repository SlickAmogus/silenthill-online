# Hosting your own Silent Hill Online server

These files run the living world server (`sh_master`) yourself: outlines of
other players, their markers and messages, and the room list that Join by
Code and Join by IP use. Players point the game at your server in the
Multiplayer menu's **Living World IP** field. The newest copy of this folder
is always on the [server release](https://github.com/SlickAmogus/silenthill-online/releases/tag/server),
rebuilt by CI whenever the server changes. Every build has a version number,
and a server you have installed tells you when a newer one is out and installs
it when you say so ([Updates](#updates)).

Nobody has to run one: the game ships pointing at the project's own server.
Run your own for a private group, a LAN party, or as a spare when the main one
is full. Each server is its own world, with its own messages and rooms.

| file | what it is |
|---|---|
| `server.txt` | your settings: where the server is, its command line, an optional domain |
| `Redeploy-Server.bat` | installs or updates the server on a Linux machine over ssh (Windows) |
| `Server-Console.bat` | opens the running server's admin console (Windows) |
| `Server-Monitor.bat` | a live view of the server's log, from its last start (Windows) |
| `Server-Log.bat` | opens the server's log file in Notepad, copied from the Linux server first (Windows) |
| `Start-Server.bat` | runs the server on this Windows PC |
| `install-server.sh` | the installer that runs on the Linux server itself |
| `sh_master.exe` | the server for Windows |
| `sh_master-linux-x64`, `sh_master-linux-arm64` | the server for Linux, static, no dependencies |
| `sh_master.c`, `sh_net_proto.h`, `sh_net_memo_text.h`, `sh_net_chat_filter.h` | the source, built on the server when it has gcc |
| `sh_master_version.h`, `version.txt` | which version these files are |
| `sh_master.cfg` | not in the zip: the server writes it beside itself the first time it starts. Update settings ([Updates](#updates)) |

## On a Linux VPS, from Windows

1. Unzip this folder anywhere. Open `server.txt` and set `host=` to the
   machine, as `root@1.2.3.4`. Give the server a name in `args=`.
2. Double-click **Redeploy-Server.bat** and type the machine's password. It
   uploads the files, builds or installs `sh_master`, creates the `sh1pc`
   service (starts with the machine, restarts itself two seconds after a
   crash), opens the UDP port in ufw when ufw is on, and starts it. The last
   lines show whether the service and its admin console answer, and the
   address players enter.
3. To update later, open **Server-Console.bat** and type `update`
   ([Updates](#updates)). Double-click Redeploy-Server.bat again after editing
   `server.txt`, or to install from a newer copy of this folder; it never puts
   an older server over a newer one. The new program is built first, beside the running
   one. When players are online it lists them and asks: **continue** gives
   them a toast and a chat line saying the server restarts in 30 seconds and
   restarts after that, **stop** changes nothing and the server keeps running
   the version it has. (Run on the server itself, `install-server.sh` asks the
   same; `--yes` answers continue, `--no-warn` restarts at once without
   warning.) The service file is kept, so change the command line in
   `/etc/systemd/system/sh1pc.service` once it exists.

Players outside the machine's network need UDP 27888 open to it. Cloud
providers often have a second firewall in their control panel; open the port
there too.

**Browser players** can only reach a server over `wss://`, which needs a
domain name and a certificate. Point a DNS record at the machine, put the
name in `domain=`, and run Redeploy-Server.bat: it installs Caddy, which
fetches the certificate itself and proxies `wss://your.domain` to the server.
Ports 80 and 443 must be free. Browser players then type `your.domain` as the
Living World IP.

### Admin console

**Server-Console.bat** logs in and runs `sh_master --console`. `help` lists
the commands:

| command | does |
|---|---|
| `players` | who is on, with ids, addresses, rooms and builds, and how long the server has been up |
| `kick <id or name> [reason]` | removes a player; they can reconnect |
| `ban <id, name or IP> [minutes] [reason]` | no minutes means forever |
| `unban <IP or uid>`, `bans` | lift a ban, list them |
| `memos [name or uid]` | marker counts per player, or one player's markers with their likes, dislikes, days left and typed word |
| `adminkey new`, `adminkey show`, `adminkey off` | the key that lets its holder remove markers from inside the game ([Messages](#messages-lifetimes-likes-and-removing-them)) |
| `prune old <days>`, `prune user <name>`, `prune id <id>`, `prune deaths [days]` | delete markers |
| `prune all` | delete every marker: a clean world |
| `say <text>` | a message to everyone online |
| `save` | write the marker store now |
| `watch` | the log since the server last started, then each new line as it happens, until you press Enter. Scroll up in the window for the history |
| `watch all`, `watch <N>` | the whole log file, or its last N lines, then live |
| `logfile` | where the log file is |
| `version` | this server's version, and whether a newer one is out |
| `update` | installs the newest server: it asks first, then offers to restart |
| `update off`, `update ask`, `update check` | stop looking for new versions, look again, look right now |
| `restart [seconds]` | warns the players, then starts the server again (30 seconds; at once when nobody is on). `restart cancel` calls it off |

The console only listens on the machine itself (127.0.0.1:27891), so it needs
the ssh login. When the connection drops, press a key in the window to log in
again. Markers live in `memos.db`, who has voted on them in `memos.db.votes`,
bans in `bans.txt`, the admin key in `admin.key` and the key behind the
address tokens in `addrkey.bin`, all in the service's working directory
(`/root` by default). Keep `addrkey.bin`: a new one makes every player's saved
blocks point at nobody, and lets everyone vote again on messages they already
voted on.

### Messages: lifetimes, likes and removing them

A message a player leaves stays for seven days, and one day longer for every
like it gets. Players like or dislike a message by standing on it and
pressing accept or cancel; the server keeps one vote per address on each
message and shows the totals to everyone standing there. A dislike is counted
and takes no time away. Death markers go after a day, as before. The server
does all of this by itself: there is nothing to run.

Nothing of this is lost when the server restarts or is updated. A message's
likes, dislikes and the day its seven days began are written into `memos.db`
with it, so it has exactly the days it had. Who voted is kept beside it in
`memos.db.votes`, so a restart does not let a player like the same message a
second time. That file holds the server's own tokens for addresses, never the
addresses themselves.

The word in a message can be typed by the player. The server masks racial
slurs in it, the same way it does in chat and in names, before it stores the
message or shows it to anyone. `memos <name>` lists a player's messages with
the typed word, and `prune id <marker id>` removes one; players standing
there see it go at once.

**The first start of this version** takes in the marker store of an older
server once. Every marker in it starts its seven days at that moment, as if it
had been left that day, and the old file is kept beside it as `memos.db.v1`.
An older server cannot read the new `memos.db`, so keep that file if you may
ever go back.

**Removing markers from inside the game.** You do not have to open the
console to take down a message you come across while playing:

1. In the console, type `adminkey new`. It prints a key and keeps it in
   `admin.key`.
2. In the game, open chat and type `/admin KEY` once. The key is saved in the
   game's settings.
3. From then on `/remove` takes down the marker you are standing on, and
   `/remove ID` any marker by its id. The server logs who removed what.

The key is never sent, to this server or anyone else. Each remove carries a
proof made from the key, that session and that one marker, so using your
name, your player id or a copy of your traffic gets nobody anywhere. That is
why it is a key and not a Steam id: a server cannot check a Steam id, so
anyone could claim yours. Whoever has the key can remove markers, so keep it
to yourself. `adminkey new` makes a new key and the old one stops working at
once; `adminkey off` leaves the server with none.

### Log

The server writes everything it reports to `sh_master.log` beside `memos.db`
(`/root/sh_master.log` by default), with the date and time on every line, from
the moment it starts. The log is there whether or not anyone is watching, so a
console opened hours later can still show what happened. **Server-Monitor.bat**
opens it live (the same as `watch`), **Server-Log.bat** opens the whole file in
Notepad, and on the server itself any viewer can read it while the server writes
(`less /root/sh_master.log`, `tail -f /root/sh_master.log`). The journal gets
the same lines (`journalctl -u sh1pc -f`).

What goes in:

- players joining (address, id, build) and leaving or timing out (how long they
  were on, their ping), kicks and bans by the admin
- chat in the living world, Global and Level. What players say to each other
  inside a co-op room is theirs and is neither shown nor logged
- messages left, in the words players see, deaths and save marks, each with its
  marker id for `prune id`
- co-op rooms opening and closing, who joins them, refused joins, invites
- network trouble, at most once a minute per player or address: a player heard
  from again after 4 s or more of silence (lag, packet loss, a frozen game), a
  ping of 300 ms or more, a player sending faster than the limit, an address
  sending datagrams for no live session or unreadable ones (after a server
  restart the games still on their old session send a few of these until they
  reconnect; a steady stream from one address is a probe or a flood), banned
  addresses trying to connect, a full server, browser connections that break
  the WebSocket protocol
- `[USER REPORT]` entries from players' `/report`: who reported whom, both
  names with their tags and addresses, the reason, and the private messages
  between the two that the server still had in memory. Private messages are
  never logged otherwise, and live in memory only
- every admin command that changes something (kick, ban, say, prune...; looking at
  players, bans or markers is not logged), and a `stats:` line every 15 minutes with who is on and
  what the network did

At 16 MB the log moves to `sh_master.log.1` (the older ones to `.2` up to
`.5`) and a new one starts, so it never fills the disk. `--log FILE`,
`--log-max MB` and `--no-log` change that.

## On a Linux machine, directly

```
unzip SHOnline-server.zip && cd SHOnline-server
sudo bash install-server.sh --args '-n "Fog" -m "Welcome"' [--domain server.example.com]
```

Same result as the batch file. Without systemd, run the binary by hand:
`./sh_master-linux-x64 -n "Fog"`.

## On this Windows PC

Set `args=` in `server.txt`, double-click **Start-Server.bat**, and allow the
program through the Windows firewall when asked. Forward UDP 27888 to this PC
for players outside your network. **Server-Console.bat** talks to it while it
runs (leave `host=` empty), **Server-Monitor.bat** shows its log live and
**Server-Log.bat** opens `sh_master.log` from this folder. Browser players cannot connect to a plain Windows
host, because they need `wss://`.

## Updates

Every server build has a version number, which goes up by one or more each
time the server changes. `sh_master --version` prints it, the log has it on
the line the server starts with, and `version` in the console shows it.

A running server looks for a newer version when it starts and every six hours
after that. When there is one it writes a line in the log
(`update available: version 95 ...`) and every console you open shows it at
the top. That is all it does by itself. **Nothing is ever installed unless you
say yes**, and a server that is not updated keeps running as it is.

To install the newer version:

- **A server on a Linux machine:** open **Server-Console.bat** and type
  `update`, or run `sh_master --update` on the machine. It shows the version,
  asks, downloads the ready-made server for that machine, checks that it
  starts, and puts it in place. Then it asks whether to restart the running
  server now; yes gives the players a 30 second warning first.
  `sh_master --update --yes` does all of it without asking, for a script.
- **A server on this Windows PC:** **Start-Server.bat** asks when you start
  it, before the server opens:

  ```
  A newer server is available: version 95 (built 2026-11-18 03:10:00 UTC). This is version 93.
    Y  update now
    N  not now
    A  never ask again
  ```

  Y installs it and starts the new version. N, or no answer for 30 seconds,
  starts the server as it is. A turns the checks off for good.
- **By hand, as before:** download the current `SHOnline-server.zip`, unzip it
  over this folder (keep your `server.txt`), and run Redeploy-Server.bat or
  `install-server.sh`.

An update replaces the server program and nothing else. The one it replaced
stays beside it as `sh_master.old` (`sh_master.exe.old` on Windows): rename
it back to undo an update. Your markers, bans, log, `server.txt` and
`sh_master.cfg` are never touched, and neither are the scripts in this folder.
Whether the game has to be updated too depends on the protocol number: when a
new version speaks a different one, the notice says so, because players on
the newest game cannot join until the server is updated.

### Settings: sh_master.cfg

The server writes `sh_master.cfg` beside its own program file the first time
it starts: in this folder on a Windows PC, `/root/sh_master.cfg` on a Linux
machine set up by the installer. Two settings:

| setting | meaning |
|---|---|
| `update_check=ask` | the default: look for newer versions, report them, ask before installing |
| `update_check=off` | never look and never ask. This is what **never ask again** and `update off` write. `update ask` in the console, or editing the file, turns it back on. `sh_master --update` still works whenever you run it |
| `update_url=...` | where new versions come from (below) |

`--no-update-check` on the command line turns the check off for one run
without changing the file. A server you compiled from a clone of the
repository has no version number and does not look for updates.

Looking for an update is one download of a small text file from the address
in `update_url`. Nothing about your server or its players is sent. It needs
`curl`, which Windows 10 and later and nearly every Linux machine have.

### Running your own update channel

To decide yourself which versions your servers install, point `update_url` at
a folder of your own: on a web server, in a storage bucket, or a release on
your own fork. It must be an `https://` or `http://` address without a
trailing slash, holding the same file names as the project's release:

| file | what it is |
|---|---|
| `version.txt` | must contain a line `version N`. A server installs only a number higher than its own. `proto N` and `built DATE` lines are shown in the notice when present |
| `sh_master-linux-x64`, `sh_master-linux-arm64`, `sh_master.exe` | the server programs; only the ones your machines need |

Copy a release into it when you have tried it and want your servers to have
it: the programs first and `version.txt` last, so that no server sees the new
number before the program is there. Before a download is put in place, the
server runs it once and checks that it really is the version `version.txt`
named; if not, nothing changes.

`update_url` can only be changed in the file. The admin console can turn the
check on and off but cannot set the address, so someone who can only reach
the console cannot choose what your server installs.

## Server options

| flag | meaning |
|---|---|
| `-n NAME` | server name players see |
| `-m TEXT` | message of the day |
| `-w WORD` | require a password |
| `-p PORT` | UDP port (default 27888) |
| `--ws PORT` | also take browser players over WebSocket on TCP PORT (behind a TLS proxy) |
| `--max N` | player cap (default 512, up to 1024); living world and co-op rooms share it |
| `-i N` | players allowed from one IP address (default 8; raise it for a LAN party behind one router) |
| `-d FILE` | marker store path (default `memos.db`) |
| `-b ID`, `--builds FILE` | only admit the listed build ids (optional; rooms already match builds) |
| `--admin PORT` | admin console port on 127.0.0.1 (default 27891, 0 = off) |
| `--log FILE` | the log file (default `sh_master.log` beside the marker store) |
| `--log-max MB` | start a new log at this size (default 16; the last five are kept) |
| `--no-log` | no log file |
| `--no-ghosts` / `--no-memos` / `--no-deaths` | serve a subset |
| `-v` | log every query |
| `--version` | print which server this is |
| `--check-update` | say whether a newer server is out (exit code 10 when one is) |
| `--update [--yes]` | install the newest server and offer to restart the running one; `--yes` asks nothing |
| `--no-update-check` | do not look for new versions during this run |

Restarting the server, for an update or otherwise, drops the co-op rooms in
progress (players re-host; the living world reconnects by itself), so do it
at a quiet hour. Server and game only have to be updated together when the
wire protocol changes, which the update notice and the release notes say.

The server ends with exit code 75 when it wants to be started again (the
`restart` command, or an update on Windows). On Linux it starts itself again;
Start-Server.bat does it on Windows. If you run `sh_master.exe` under a
service manager of your own, have it restart the program on that code.
