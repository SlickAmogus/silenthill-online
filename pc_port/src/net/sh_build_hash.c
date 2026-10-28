/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sh_build_hash.c - SHA-256 of the running executable (see the header). */

#include "sh_build_hash.h"
#include "sh_net_proto.h" /* SHNET_BUILD_MAX */

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ---- compact SHA-256 (FIPS 180-4) ------------------------------------- */

typedef struct
{
    unsigned int       h[8];
    unsigned long long len;    /* bytes hashed so far */
    unsigned char      buf[64];
    int                bufLen;
} Sha256;

static const unsigned int K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void Sha256_Block(Sha256* s, const unsigned char* p)
{
    unsigned int w[64];
    unsigned int a, b, c, d, e, f, g, h;
    int          i;

    for (i = 0; i < 16; i++)
    {
        w[i] = ((unsigned int)p[i * 4] << 24) | ((unsigned int)p[i * 4 + 1] << 16) |
               ((unsigned int)p[i * 4 + 2] << 8) | (unsigned int)p[i * 4 + 3];
    }
    for (i = 16; i < 64; i++)
    {
        unsigned int s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned int s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];

    for (i = 0; i < 64; i++)
    {
        unsigned int S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = h + S1 + ch + K[i] + w[i];
        unsigned int S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        unsigned int mj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void Sha256_Init(Sha256* s)
{
    static const unsigned int iv[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    memcpy(s->h, iv, sizeof(iv));
    s->len    = 0;
    s->bufLen = 0;
}

static void Sha256_Update(Sha256* s, const unsigned char* data, size_t n)
{
    s->len += n;
    while (n > 0)
    {
        size_t take = 64 - (size_t)s->bufLen;
        if (take > n) take = n;
        memcpy(s->buf + s->bufLen, data, take);
        s->bufLen += (int)take;
        data      += take;
        n         -= take;
        if (s->bufLen == 64)
        {
            Sha256_Block(s, s->buf);
            s->bufLen = 0;
        }
    }
}

static void Sha256_Final(Sha256* s, unsigned char out[32])
{
    unsigned long long bits = s->len * 8ull;
    unsigned char      pad  = 0x80;
    unsigned char      z    = 0;
    unsigned char      lenb[8];
    int                i;

    Sha256_Update(s, &pad, 1);
    while (s->bufLen != 56)
    {
        Sha256_Update(s, &z, 1);
    }
    for (i = 0; i < 8; i++)
    {
        lenb[i] = (unsigned char)(bits >> (56 - i * 8));
    }
    Sha256_Update(s, lenb, 8);

    for (i = 0; i < 8; i++)
    {
        out[i * 4]     = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(s->h[i]);
    }
}

/* ---- the executable's hash ------------------------------------------- */

static char s_hex[65];
static char s_short[SHNET_BUILD_MAX];
static int  s_done;

static void ShBuild_Compute(void)
{
    char          path[1024];
    FILE*         f;
    Sha256        sha;
    unsigned char out[32];
    static unsigned char chunk[65536];
    size_t        n;
    int           i;

    if (s_done) return;
    s_done = 1;
    strcpy(s_hex, "unknown");

#if defined(_WIN32)
    if (GetModuleFileNameA(NULL, path, (DWORD)sizeof(path)) == 0)
    {
        goto out;
    }
#else
    {
        ssize_t r = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (r <= 0) goto out;
        path[r] = '\0';
    }
#endif

    f = fopen(path, "rb");
    if (!f) goto out;

    Sha256_Init(&sha);
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
    {
        Sha256_Update(&sha, chunk, n);
    }
    fclose(f);
    Sha256_Final(&sha, out);

    for (i = 0; i < 32; i++)
    {
        static const char hx[] = "0123456789abcdef";
        s_hex[i * 2]     = hx[out[i] >> 4];
        s_hex[i * 2 + 1] = hx[out[i] & 15];
    }
    s_hex[64] = '\0';

out:
    memcpy(s_short, s_hex, SHNET_BUILD_MAX - 1);
    s_short[SHNET_BUILD_MAX - 1] = '\0';
}

const char* ShBuild_Hash(void)
{
    ShBuild_Compute();
    return s_hex;
}

const char* ShBuild_Short(void)
{
    return SHNET_COOP_BUILD;
}

const char* ShBuild_ExeShort(void)
{
    ShBuild_Compute();
    return s_short;
}
