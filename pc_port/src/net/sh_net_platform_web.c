/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_net_platform_web.c - the browser build's "UDP" for the online client.
 *
 * A page cannot send datagrams, so each ShNetSock is a WebSocket to the
 * server, and every datagram travels as one binary message (sh_master --ws
 * takes them, behind a TLS proxy). The server end answers on the same
 * connection, so what the caller sees is exactly the UDP contract: sends
 * that may be lost, receives that never block, one peer per address.
 *
 * An address is the WebSocket URL itself. ShNetPlat_Resolve makes one from
 * the server name: wss://<host>/, since a page served over https may only
 * open wss:// and the proxy sits on 443. A full ws:// or wss:// URL typed as
 * the server is used as it stands.
 *
 * The browser delivers socket events between frames (the game loop yields
 * to it every vsync), so the callbacks below and the game thread that calls
 * Send and Recv never run at the same time.
 */
#if defined(__EMSCRIPTEN__)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten/emscripten.h>
#include <emscripten/websocket.h>

#include "sh_net_platform.h"

#define WSP_MAX_MSG   1500   /* above SHNET_MTU; anything larger is not a datagram */
#define WSP_QUEUE     256
#define WSP_RETRY_MS  3000u  /* a refused or dropped connection is retried this often */

typedef struct
{
    int           len;
    unsigned char data[WSP_MAX_MSG];
} WspMsg;

struct ShNetSock
{
    EMSCRIPTEN_WEBSOCKET_T ws;      /* 0 = no connection */
    int                    open;
    int                    dead;    /* closed by the browser; deleted on the next call */
    unsigned int           retryAt;
    ShNetAddr              peer;
    int                    head, tail;
    WspMsg                 q[WSP_QUEUE];
};

static int    s_initRefs;
static double s_epochMs;

int ShNetPlat_Init(void)
{
    if (s_initRefs++ == 0)
    {
        s_epochMs = emscripten_get_now();
    }
    return emscripten_websocket_is_supported() ? 1 : 0;
}

void ShNetPlat_Shutdown(void)
{
    if (s_initRefs > 0)
    {
        s_initRefs--;
    }
}

unsigned int ShNetPlat_Millis(void)
{
    return (unsigned int)(emscripten_get_now() - s_epochMs);
}

static EM_BOOL Wsp_OnOpen(int type, const EmscriptenWebSocketOpenEvent* e, void* user)
{
    ShNetSock* s = (ShNetSock*)user;
    (void)type;
    if (s->ws == e->socket)
    {
        s->open = 1;
    }
    return EM_TRUE;
}

static EM_BOOL Wsp_OnMessage(int type, const EmscriptenWebSocketMessageEvent* e, void* user)
{
    ShNetSock* s = (ShNetSock*)user;
    int        next;
    (void)type;
    if (s->ws != e->socket || e->isText || e->numBytes == 0 || e->numBytes > WSP_MAX_MSG)
    {
        return EM_TRUE;
    }
    next = (s->head + 1) % WSP_QUEUE;
    if (next == s->tail)
    {
        return EM_TRUE; /* full: lost, like a datagram the socket buffer had no room for */
    }
    memcpy(s->q[s->head].data, e->data, e->numBytes);
    s->q[s->head].len = (int)e->numBytes;
    s->head           = next;
    return EM_TRUE;
}

static EM_BOOL Wsp_OnClose(int type, const EmscriptenWebSocketCloseEvent* e, void* user)
{
    ShNetSock* s = (ShNetSock*)user;
    (void)type;
    if (s->ws == e->socket)
    {
        s->open = 0;
        s->dead = 1;
    }
    return EM_TRUE;
}

static EM_BOOL Wsp_OnError(int type, const EmscriptenWebSocketErrorEvent* e, void* user)
{
    ShNetSock* s = (ShNetSock*)user;
    (void)type;
    if (s->ws == e->socket)
    {
        s->open = 0;
        s->dead = 1;
    }
    return EM_TRUE;
}

static void Wsp_Drop(ShNetSock* s)
{
    if (s->ws)
    {
        if (!s->dead)
        {
            emscripten_websocket_close(s->ws, 1000, "");
        }
        emscripten_websocket_delete(s->ws);
    }
    s->ws   = 0;
    s->open = 0;
    s->dead = 0;
}

ShNetSock* ShNetPlat_Open(void)
{
    return (ShNetSock*)calloc(1, sizeof(ShNetSock));
}

void ShNetPlat_Close(ShNetSock* s)
{
    if (!s)
    {
        return;
    }
    Wsp_Drop(s);
    free(s);
}

int ShNetPlat_Resolve(const char* host, unsigned short port, ShNetAddr* out)
{
    char url[SHNET_ADDR_BYTES];
    int  n;
    (void)port;
    if (!host || !host[0] || !out)
    {
        return 0;
    }
    if (!strncmp(host, "ws://", 5) || !strncmp(host, "wss://", 6))
    {
        n = snprintf(url, sizeof(url), "%s", host);
    }
    else
    {
        n = snprintf(url, sizeof(url), "wss://%s/", host);
    }
    if (n <= 0 || n >= (int)sizeof(url))
    {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->bytes, url, (size_t)n + 1);
    out->len = n + 1;
    return 1;
}

/* Opens (or reopens) the connection to `to`. Returns 1 once it is open. */
static int Wsp_Connect(ShNetSock* s, const ShNetAddr* to)
{
    EmscriptenWebSocketCreateAttributes attr;

    if (s->ws && !ShNetPlat_AddrEqual(&s->peer, to))
    {
        Wsp_Drop(s);
    }
    if (s->dead)
    {
        Wsp_Drop(s);
    }
    if (s->ws)
    {
        return s->open;
    }
    if ((int)(ShNetPlat_Millis() - s->retryAt) < 0)
    {
        return 0;
    }
    s->retryAt = ShNetPlat_Millis() + WSP_RETRY_MS;

    emscripten_websocket_init_create_attributes(&attr);
    attr.url = (const char*)to->bytes;
    s->ws    = emscripten_websocket_new(&attr);
    if (s->ws <= 0)
    {
        s->ws = 0;
        return 0;
    }
    s->peer = *to;
    s->head = s->tail = 0;
    emscripten_websocket_set_onopen_callback(s->ws, s, Wsp_OnOpen);
    emscripten_websocket_set_onmessage_callback(s->ws, s, Wsp_OnMessage);
    emscripten_websocket_set_onclose_callback(s->ws, s, Wsp_OnClose);
    emscripten_websocket_set_onerror_callback(s->ws, s, Wsp_OnError);
    return 0;
}

int ShNetPlat_Send(ShNetSock* s, const ShNetAddr* to, const void* buf, int len)
{
    if (!s || !to || to->len <= 0 || !buf || len <= 0 || len > WSP_MAX_MSG)
    {
        return -1;
    }
    /* Until the connection is open a send is simply lost; everything the
     * client sends first (HELLO) is retried on a timer anyway. */
    if (!Wsp_Connect(s, to))
    {
        return -1;
    }
    if (emscripten_websocket_send_binary(s->ws, (void*)buf, (uint32_t)len) != EMSCRIPTEN_RESULT_SUCCESS)
    {
        return -1;
    }
    return len;
}

int ShNetPlat_Recv(ShNetSock* s, ShNetAddr* from, void* buf, int cap)
{
    int n;
    if (!s || !buf || cap <= 0)
    {
        return -1;
    }
    if (s->tail == s->head)
    {
        return 0;
    }
    n = s->q[s->tail].len;
    if (n > cap)
    {
        n = cap;
    }
    memcpy(buf, s->q[s->tail].data, (size_t)n);
    s->tail = (s->tail + 1) % WSP_QUEUE;
    if (from)
    {
        *from = s->peer;
    }
    return n;
}

void ShNetPlat_AddrToString(const ShNetAddr* a, char* out, int cap)
{
    if (!out || cap <= 0)
    {
        return;
    }
    snprintf(out, (size_t)cap, "%s", (a && a->len > 0) ? (const char*)a->bytes : "(unset)");
}

int ShNetPlat_AddrEqual(const ShNetAddr* a, const ShNetAddr* b)
{
    if (!a || !b || a->len <= 0 || a->len != b->len)
    {
        return 0;
    }
    return memcmp(a->bytes, b->bytes, (size_t)a->len) == 0;
}

#endif /* __EMSCRIPTEN__ */
