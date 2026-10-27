/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * sh_build_hash.h - identify THIS build to the online server.
 *
 * The client hashes its own executable (SHA-256) once at startup and sends a
 * short form of it in the HELLO "build" field. A server configured with an
 * allow-list (sh_master -b <id>, or --builds <file>) refuses any client whose
 * id is not listed, so a casually modified/rebuilt client is turned away.
 *
 * Honest limit: this is a deterrent, not a guarantee. The source is open, so a
 * deliberately malicious client can send any id it likes; the real protection
 * is keeping the server/host authoritative for things that matter. The server
 * logs each joining client's id, which is how an admin learns the id of the
 * approved build (run it once, read the log, put it in the allow-list).
 */
#ifndef SH_BUILD_HASH_H
#define SH_BUILD_HASH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Full 64-hex SHA-256 of the running executable (computed on first call,
 * cached). "unknown" if the file could not be read. */
const char* ShBuild_Hash(void);

/* The short id sent on the wire: the first SHNET_BUILD_MAX-1 hex chars. */
const char* ShBuild_Short(void);

#ifdef __cplusplus
}
#endif

#endif /* SH_BUILD_HASH_H */
