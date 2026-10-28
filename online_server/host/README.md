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
| `Start-Server.bat` | runs the server on this Windows PC |
| `install-server.sh` | the installer that runs on the Linux server itself |
| `sh_master.exe` | the server for Windows |
| `sh_master-linux-x64`, `sh_master-linux-arm64` | the server for Linux, static, no dependencies |
| `sh_master.c`, `sh_net_proto.h` | the source, built on the server when it has gcc |

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
   editing `server.txt`. It replaces the program and restarts; the service
   file is kept, so change the command line in
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
| `players` | who is on, with ids, addresses, rooms and builds |
| `kick <id or name> [reason]` | removes a player; they can reconnect |
| `ban <id, name or IP> [minutes] [reason]` | no minutes means forever |
| `unban <IP or uid>`, `bans` | lift a ban, list them |
| `memos [name or uid]` | marker counts per player, or one player's markers |
| `prune old <days>`, `prune user <name>`, `prune id <id>`, `prune deaths [days]` | delete markers |
| `say <text>` | a message to everyone online |
| `save` | write the marker store now |

The console only listens on the machine itself (127.0.0.1:27891), so it needs
the ssh login. The log is `journalctl -u sh1pc -f`. Markers live in `memos.db`
and bans in `bans.txt` in the service's working directory (`/root` by default).

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
runs (leave `host=` empty). Browser players cannot connect to a plain Windows
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
| `--no-ghosts` / `--no-memos` / `--no-deaths` | serve a subset |
| `-v` | log every query |

Updating the server drops the co-op rooms in progress (players re-host; the
living world reconnects by itself), so update at a quiet hour. Server and game
only have to be updated together when the wire protocol changes, which the
release notes say.
