# Hosting your own Silent Hill Online server

These files run the living world server (`sh_master`) yourself: outlines of
other players, their markers and messages, and the room list that Join by
Code and Join by IP use. Players point the game at your server in the
Multiplayer menu's **Living World IP** field. The newest copy of this folder
is always on the [server release](https://github.com/SlickAmogus/silenthill-online/releases/tag/server),
rebuilt by CI whenever the server changes.

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

## On a Linux VPS, from Windows

1. Unzip this folder anywhere. Open `server.txt` and set `host=` to the
   machine, as `root@1.2.3.4`. Give the server a name in `args=`.
2. Double-click **Redeploy-Server.bat** and type the machine's password. It
   uploads the files, builds or installs `sh_master`, creates the `sh1pc`
   service (starts with the machine, restarts itself two seconds after a
   crash), opens the UDP port in ufw when ufw is on, and starts it. The last
   lines show whether the service and its admin console answer, and the
   address players enter.
3. Double-click it again whenever a new server release comes out, or after
   editing `server.txt`. The new program is built first, beside the running
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
| `memos [name or uid]` | marker counts per player, or one player's markers |
| `prune old <days>`, `prune user <name>`, `prune id <id>`, `prune deaths [days]` | delete markers |
| `prune all` | delete every marker: a clean world |
| `say <text>` | a message to everyone online |
| `save` | write the marker store now |
| `watch` | the log since the server last started, then each new line as it happens, until you press Enter. Scroll up in the window for the history |
| `watch all`, `watch <N>` | the whole log file, or its last N lines, then live |
| `logfile` | where the log file is |

The console only listens on the machine itself (127.0.0.1:27891), so it needs
the ssh login. When the connection drops, press a key in the window to log in
again. Markers live in `memos.db` and bans in `bans.txt` in the service's
working directory (`/root` by default).

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

Updating the server drops the co-op rooms in progress (players re-host; the
living world reconnects by itself), so update at a quiet hour. Server and game
only have to be updated together when the wire protocol changes, which the
release notes say.
