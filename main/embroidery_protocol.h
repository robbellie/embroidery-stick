/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>

/*
 * Embroidery Stick binary protocol over persistent TCP connection.
 *
 * Every message:  [cmd:1][payload_len:4 LE][payload:N]
 * All integers little-endian.
 */

#define EMBROIDERY_PROTO_VERSION    1
#define EMBROIDERY_DEFAULT_PORT     7892
#define EMBROIDERY_MAX_FILES        512
#define EMBROIDERY_CHUNK_SIZE       (8 * 1024)    /* bytes per READ_FILE request */

/*
 * Backend auto-discovery: one-shot UDP broadcast/reply, separate from the
 * TCP control connection above. Request payload is just the magic string;
 * reply payload is the magic string followed by a uint16_t LE TCP port
 * (the IP is taken from the reply packet's source address, not the wire).
 * backend/main.go duplicates these as matching literal constants (same
 * cross-language pattern already used for EMBROIDERY_DEFAULT_PORT).
 */
#define EMBROIDERY_DISCOVERY_MAGIC         "EMBROIDERY_DISCOVER_V1"
#define EMBROIDERY_DISCOVERY_DEFAULT_PORT  7891

/* Command bytes */
#define CMD_HELLO                   0x01
#define CMD_VERSION                 0x02
#define CMD_LIST_FILES              0x03
#define CMD_READ_FILE               0x04

/* Frame header (5 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t  cmd;
    uint32_t payload_len;
} proto_frame_t;

/* CMD_HELLO request payload */
typedef struct __attribute__((packed)) {
    uint16_t proto_version;
} proto_hello_req_t;

/* CMD_HELLO response payload */
typedef struct __attribute__((packed)) {
    uint16_t proto_version;
    uint64_t disk_version;
} proto_hello_resp_t;

/* CMD_VERSION response payload */
typedef struct __attribute__((packed)) {
    uint64_t disk_version;
} proto_version_resp_t;

/*
 * One file entry in CMD_LIST_FILES response.
 * name: null-terminated, 8.3 format, e.g. "LOGO.DST"
 */
typedef struct __attribute__((packed)) {
    char     name[13];
    uint32_t size;
    uint32_t mtime;
    uint16_t file_id;
} proto_file_info_t;
/* LIST_FILES response: [uint16_t count][proto_file_info_t × count] */

/* CMD_READ_FILE request payload */
typedef struct __attribute__((packed)) {
    uint16_t file_id;
    uint32_t offset;
    uint32_t length;
} proto_read_req_t;
/* READ_FILE response: [uint32_t actual_length][data × actual_length] */
