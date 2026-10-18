/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_session.h - the Steam co-op session, and the seam co-op plugs into.
 *
 * A SESSION is a Steam lobby whose members are in direct peer-to-peer contact
 * over Valve's relay. It is a different thing from the master-server world in
 * sh_net.h, and deliberately so:
 *
 *   master server   strangers, ambient, no accounts, anyone can host one,
 *                   ghosts and messages only
 *   session         friends, invited through the Steam overlay, direct P2P,
 *                   and the place actual co-op will live
 *
 * They coexist. A player can be wandering a master server's fogged town and be
 * in a two-person session at the same time; the two systems share only the
 * datagram format.
 *
 * WHAT WORKS TODAY: create or join a lobby, invite through the overlay, join
 * from an invite or from +connect_lobby, see who is in the session, and a live
 * round-trip measurement to each member over the real P2P link.
 *
 * WHAT IT IS FOR: that last part is the point. The link check is not a
 * diagnostic bolted on the side, it is the co-op transport running end to end
 * with one trivial message type on it. Adding "here is where I am and what I
 * am doing" is another type in the same range in sh_net_proto.h, sent from the
 * same place, parsed in the same switch.
 *
 * THREADING: everything here runs on the network worker thread, which is the
 * one thread Steam is called from. The accessors are read from the game thread
 * against a copy the worker publishes, the same arrangement the rest of the
 * client uses.
 */
#ifndef SH_NET_SESSION_H
#define SH_NET_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

#define SHSESSION_MAX_MEMBERS 8
#define SHSESSION_NAME_MAX    40
#define SHSESSION_POSE_RING   8

#define SHSESSION_POSE_SFX    6
#define SHSESSION_SFX_Q       16

/* A positional sound the local Harry made (footstep, shot, reload, swing),
 * replayed at the partner's position. kind bit 0 = func_8005DD44 (pitched),
 * else func_8005DC3C; age4 = how long before the carrying pose it was made,
 * in 4 ms units -- each sound rides several poses so one lost packet does not
 * lose a gunshot, and seq lets the receiver play it once. */
typedef struct
{
    unsigned char  seq, vol, kind, age4;
    unsigned short sfx;
    signed char    pitch;
    unsigned char  pad;
} ShSessionSfx;

typedef struct
{
    unsigned int   ms;
    unsigned short sfx;
    unsigned char  vol, kind;
    signed char    pitch;
} ShSessionSfxEv;

/* One co-op pose sample. Harry is posed in two halves (Player_AnimUpdate: legs
 * from player.model, torso and arms from extra.model), and aiming, firing and
 * reloading live only in the upper half -- a pose with one keyframe showed a
 * shooting partner standing still with empty hands. kf/alpha are the exact
 * Anim_BoneUpdate arguments each half was last posed with, so the partner
 * shows the sender's frame, blend included, not the nearest whole keyframe. */
typedef struct
{
    int            x, y, z;           /* Q19.12 world position */
    short          rotY;              /* Q3.12 */
    unsigned short anim, frame;       /* lower clip status + keyframe (other readers) */
    unsigned short kf[2][2];          /* [0] legs, [1] torso: keyframe pair */
    unsigned short alpha[2];          /* Q12 blend between the pair */
    unsigned char  weaponAnm;         /* weapon whose HB_WEP*.ANM fills kf 5xx+, 0xFF none */
    unsigned char  heldItem;          /* InvItemId in hand, 0xFF none */
    unsigned char  handBone;          /* MODEL_BONE last given to WorldGfx_HeldItemAttach(Harry) */
    unsigned char  sfxCount;
    unsigned char  paperMap;          /* sender's paperMapIdx + 1 (its floor), 0 unknown */
    unsigned int   ms;                /* sender: capture time; ring: time on OUR clock */
    ShSessionSfx   sfx[SHSESSION_POSE_SFX];
} ShSessionPose;

/* A member's pose at render time: interpolated between the two samples around
 * it, keyframe pairs plus their Q12 blend for Anim_BoneUpdate. */
typedef struct
{
    int   x, y, z;
    short rotY;
    int   lowKf0, lowKf1, lowAlpha;
    int   upKf0, upKf1, upAlpha;
    int   weaponAnm, heldItem, handBone;
} ShSessionPoseNow;

typedef struct
{
    unsigned long long steamId;
    char               name[SHSESSION_NAME_MAX];
    int                pingMs;     /* -1 until a pong comes back */
    int                linked;     /* 1 once they have answered us */
    int                mapIdx;     /* -1 unknown */

    /* Co-op presence: this member's last reported pose (SHNET_MSG_S_POS). Valid
     * only while poseMs is recent and the member is on our map. */
    int                charaId;
    int                flags;      /* SHNET_PF_* */
    int                x, y, z;    /* Q19.12 world position */
    short              rotY;       /* Q3.12 */
    unsigned short     anim;
    unsigned short     frame;
    unsigned int       poseMs;     /* wall clock of the last pose, 0 = none */
    int                paperMapIdx; /* their paper-map page (floor), -1 unknown */

    /* Recent samples, oldest first, for ShSession_MemberPoseNow. Arrival times
     * jitter by the network plus the worker's sleep, so samples are placed by
     * the SENDER's clock shifted by the smallest delay seen (clockOff). */
    ShSessionPose      ring[SHSESSION_POSE_RING];
    int                ringCount;
    int                clockOff;
    int                clockOk;
    int                clockCreep;
    int                jitterQ4;   /* mean delay above the minimum, Q4 ms */
    int                gapQ4;      /* mean interval between samples, Q4 ms */
    int                delayMs;    /* how far behind real time this member renders */

    /* Sounds waiting for the render clock to reach them; serial only grows,
     * so the game thread can tell which ones it already played. */
    ShSessionSfxEv     sfxQ[SHSESSION_SFX_Q];
    unsigned int       sfxSerial[SHSESSION_SFX_Q];
    int                sfxQn;
    unsigned int       sfxNextSerial;
    int                sfxSeqOk;
    unsigned char      sfxLastSeq;
} ShSessionMember;

/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

/* Brings Steam up when online_steam is set. Safe and cheap when it is not. */
void ShSession_Init(void);
void ShSession_Shutdown(void);

/* Once per worker iteration: pump Steam, service what the game thread asked
 * for, exchange session traffic, and refresh the published copy. */
void ShSession_Tick(unsigned int nowMs);

/* ------------------------------------------------------------------ */
/* Game thread                                                         */
/* ------------------------------------------------------------------ */

/* Requests. Each sets a flag the worker services on its next tick, because
 * Steam must only ever be called from that thread. */
void ShSession_RequestHost(void);
void ShSession_RequestJoin(unsigned long long lobbyId);

/* Co-op over a relay server (vs Steam). Host opens a room, join enters one by its
 * code, list asks the server for open rooms. host is an IP or name; port 0 means
 * the default. These switch the session to the UDP backend. */
/* useWorld: ride the living-world connection when it is up (the co-op server is
 * the same server), instead of opening a second link. Falls back to a direct
 * link when the living world is not connected. */
void ShSession_RequestHostServer(const char* host, int port, int maxPlayers, int hidden, int useWorld);
void ShSession_RequestHostListen(int port, int maxPlayers); /* host the relay in-process (this PC) */
void ShSession_RequestJoinServer(const char* host, int port, int code, int useWorld);
void ShSession_RequestListServer(const char* host, int port, int useWorld);

/* Open server rooms from the last list request (for the Join browser). */
int  ShSession_RoomCount(void);
int  ShSession_RoomGet(int i, int* code, int* players, int* max, char* host, int hostCap);
void ShSession_RequestLeave(void);
void ShSession_RequestInvite(void);

/* Published state, safe to read from the game thread. */
int                ShSession_Active(void);      /* 1 while in a lobby */
int                ShSession_MemberCount(void);
const ShSessionMember* ShSession_Member(int i);
unsigned long long ShSession_LobbyId(void);
int                ShSession_IsHost(void);
/* The member who owns the world (lobby owner / room host), 0 = none. */
unsigned long long ShSession_HostId(void);
/* One line for the console and the player list panel. */
void               ShSession_StatusLine(char* out, int cap);

/* The area name the session advertises as rich presence, so friends see
 * "Alchemilla Hospital" rather than "In Game". Called from the game thread with
 * whatever the player is looking at; the worker forwards it to Steam. */
void ShSession_PublishPresence(const char* area, int mapIdx);

/* Co-op presence. The game thread publishes the local player's pose once a
 * frame; the worker broadcasts it to the other members as SHNET_MSG_S_POS, and
 * each member's incoming pose lands in its ShSessionMember for the renderer to
 * read. map < 0 means "not in a map" (menus) -- no pose is sent. */
void ShSession_PublishLocalPos(int mapIdx, int charaId, int flags, const ShSessionPose* pose);

/* Game thread: member i's pose a fixed delay behind real time, so it is almost
 * always BETWEEN two received samples rather than waiting on the next one.
 * 0 when the member has no pose. */
int  ShSession_MemberPoseNow(int i, ShSessionPoseNow* out);

/* Game thread: member i's sounds that are due on the same delayed clock and
 * newer than *serial (updated). Returns how many were written to out. */
int  ShSession_MemberTakeSfx(int i, unsigned int* serial, ShSessionSfxEv* out, int max);

/* Host -> guests: everyone boot into this map for a co-op game. Sent when the
 * host presses Start Game. */
void ShSession_RequestWorld(int mapIdx);

/* Host: remove a member from the game. The relay drops them from the room
 * and refuses their address; on Steam they are told to leave and are
 * ignored from then on either way. */
void ShSession_RequestKick(unsigned long long id);

/* Game messages (SHNET_S_IS_GAME types) carried for game-thread code such as
 * sh_net_npc.c. SendGame queues a payload for `to` (0 = every member);
 * TakeGame pops the oldest received one with its sender and returns its
 * length, -1 when none is waiting. Host-only types
 * from anyone but the host never reach TakeGame. */
int  ShSession_SendGame(int type, const void* payload, int len, unsigned long long to, int reliable);
int  ShSession_TakeGame(int* type, unsigned long long* from, void* buf, int cap);

/* Guest poll: a map the host told us to boot into since the last call, or -1.
 * Clears the request. The game thread consumes this to launch the co-op game. */
int  ShSession_TakeWorldRequest(void);

/* Co-op shared items. The game thread notices a local pickup and queues it here;
 * the worker broadcasts it (reliable) to every other member. On the other end the
 * worker queues what it receives, and the game thread drains it with TakeItem and
 * grants it to the local inventory -- so each player keeps their own pool and
 * everyone ends up with what anyone picked up. */
void ShSession_QueueItem(int itemId, int count);
int  ShSession_TakeItem(int* itemId, int* count);

/* Co-op debug/cheat permission for JOINED players. The host sets it (off by
 * default); it rides the Steam lobby data so every guest reads the same answer.
 * The host's own debug controls are never gated by this -- only guests'. */
void ShSession_SetGuestDebug(int on);
int  ShSession_GuestDebugGranted(void);

/* Friendly fire (pc_port/src/net/sh_net_hit.c). The attacker queues a hit for
 * ONE victim; the victim drains it and applies the damage to its own player.
 * amount is Q12 HP, pre-difficulty, already PvP-scaled. */
unsigned long long ShSession_SelfId(void);
void ShSession_QueueDamage(unsigned long long toId, int amount, int kbx, int kby, int kbz,
                           int reaction, short yaw);
int  ShSession_TakeDamage(int* amount, int* kbx, int* kby, int* kbz, int* reaction, short* yaw);

#ifdef __cplusplus
}
#endif

#endif /* SH_NET_SESSION_H */
