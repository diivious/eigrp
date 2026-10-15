# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
#
# Check transmit and receive cryptographic sequence behavior using the
# portable authentication, MD5, SHA-256 and stream implementations.

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'eigrp' / 'code').is_dir())
CODE = ROOT / 'eigrp' / 'code'


def test_md5_sha256_cryptographic_sequence_transmit_and_receive():
    source = r'''
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "eigrp_auth.c"

bool eigrp_sys_auth_key_lookup(const char *name, uint32_t *id,
                               char *key, size_t capacity)
{
    static const char secret[] = "md5-sequence-secret";
    if (strcmp(name, "sequence-test") != 0 || sizeof(secret) > capacity)
        return false;
    if (id)
        *id = 11;
    memcpy(key, secret, sizeof(secret));
    return true;
}

void eigrp_log(eigrp_log_level_t level, const char *format, ...)
{
    (void)level;
    (void)format;
}

const char *eigrp_intf_name_string(eigrp_intf_t *ei)
{
    return ei->name;
}

static eigrp_stream_t *packet_create(void)
{
    static const uint8_t header[EIGRP_HEADER_LEN] = {0};
    eigrp_stream_t *packet = eigrp_stream_create(128);
    if (!packet || eigrp_stream_put(packet, header, sizeof(header)) != sizeof(header))
        return NULL;
    return packet;
}

static void interface_init(eigrp_intf_t *ei, eigrp_nbr_t *nbr)
{
    memset(ei, 0, sizeof(*ei));
    memset(nbr, 0, sizeof(*nbr));
    ei->name = "auth-sequence-test";
    ei->params.auth_type = EIGRP_AUTH_TYPE_MD5;
    ei->params.auth_keychain = "sequence-test";
    ei->address.address.afi = EIGRP_AFI_IPV4;
    inet_pton(AF_INET, "192.0.2.10", ei->address.address.bytes);
    nbr->ei = ei;
    nbr->src.afi = AF_INET;
    inet_pton(AF_INET, "192.0.2.10", &nbr->src.ip.v4);
}

static int md5_packet(eigrp_intf_t *ei, eigrp_nbr_t *nbr,
                      uint32_t sequence, uint8_t flags)
{
    eigrp_stream_t *s = packet_create();
    struct TLV_MD5_Authentication_Type *tlv;
    uint32_t replay_state;
    if (!s)
        return 1;
    if (eigrp_auth_tlv_md5_encode(s, ei) != EIGRP_AUTH_MD5_TLV_SIZE)
        return 2;
    tlv = (struct TLV_MD5_Authentication_Type *)(s->data + EIGRP_HEADER_LEN);
    if (ntohl(tlv->key_sequence) != sequence || tlv->key_id != htonl(11)
        || ei->crypt_seqnum != sequence)
        return 3;
    if (eigrp_make_md5_digest(ei, s, flags) != EIGRP_AUTH_TYPE_MD5_LEN)
        return 4;
    if (!eigrp_check_md5_digest(s, tlv, nbr, flags)
        || ntohl(nbr->crypt_seqnum) != sequence)
        return 5;
    replay_state = nbr->crypt_seqnum;
    if (eigrp_check_md5_digest(s, tlv, nbr, flags)
        || nbr->crypt_seqnum != replay_state)
        return 6;
    /* A retransmit or digest refresh must not advance the sequence. */
    if (eigrp_make_md5_digest(ei, s, flags) != EIGRP_AUTH_TYPE_MD5_LEN
        || ntohl(tlv->key_sequence) != sequence || ei->crypt_seqnum != sequence)
        return 7;
    eigrp_stream_free(s);
    return 0;
}

static int sha256_packet(eigrp_intf_t *ei, eigrp_nbr_t *nbr,
                         uint32_t sequence)
{
    eigrp_stream_t *s = packet_create();
    struct TLV_SHA256_Authentication_Type *tlv;
    uint32_t replay_state;
    if (!s)
        return 1;
    if (eigrp_auth_tlv_sha256_encode(s, ei) != EIGRP_AUTH_SHA256_TLV_SIZE)
        return 2;
    tlv = (struct TLV_SHA256_Authentication_Type *)(s->data + EIGRP_HEADER_LEN);
    if (ntohl(tlv->key_sequence) != sequence || ei->crypt_seqnum != sequence)
        return 3;
    if (eigrp_make_sha256_digest(ei, s, EIGRP_AUTH_BASIC_HELLO_FLAG)
        != EIGRP_AUTH_TYPE_SHA256_LEN)
        return 4;
    if (!eigrp_check_sha256_digest(s, tlv, nbr, EIGRP_AUTH_BASIC_HELLO_FLAG)
        || ntohl(nbr->crypt_seqnum) != sequence)
        return 5;
    replay_state = nbr->crypt_seqnum;
    if (eigrp_check_sha256_digest(s, tlv, nbr, EIGRP_AUTH_BASIC_HELLO_FLAG)
        || nbr->crypt_seqnum != replay_state)
        return 6;
    /* Recomputing a digest must preserve the already encoded sequence. */
    if (eigrp_make_sha256_digest(ei, s, EIGRP_AUTH_BASIC_HELLO_FLAG)
        != EIGRP_AUTH_TYPE_SHA256_LEN
        || ntohl(tlv->key_sequence) != sequence || ei->crypt_seqnum != sequence)
        return 7;
    eigrp_stream_free(s);
    return 0;
}

int main(void)
{
    eigrp_intf_t ei;
    eigrp_nbr_t nbr;
    eigrp_stream_t *s;
    int result;

    /* A time-derived starting sequence can have the high bit set. */
    interface_init(&ei, &nbr);
    ei.crypt_seqnum = 0xefffffffU;
    result = md5_packet(&ei, &nbr, 0xf0000000U, EIGRP_AUTH_BASIC_HELLO_FLAG);
    if (result)
        return 10 + result;
    result = md5_packet(&ei, &nbr, 0xf0000001U, EIGRP_AUTH_UPDATE_INIT_FLAG);
    if (result)
        return 20 + result;

    /* An interface counter is shared across both authentication methods. */
    ei.params.auth_type = EIGRP_AUTH_TYPE_SHA256;
    ei.params.auth_password = "sha256-sequence-secret";
    result = sha256_packet(&ei, &nbr, 0xf0000002U);
    if (result)
        return 30 + result;
    result = sha256_packet(&ei, &nbr, 0xf0000003U);
    if (result)
        return 40 + result;

    /* A 32-bit counter wraps without transmitting reserved sequence zero. */
    interface_init(&ei, &nbr);
    ei.crypt_seqnum = UINT32_MAX - 1;
    result = md5_packet(&ei, &nbr, UINT32_MAX, EIGRP_AUTH_BASIC_HELLO_FLAG);
    if (result)
        return 50 + result;
    result = md5_packet(&ei, &nbr, 1U, EIGRP_AUTH_BASIC_HELLO_FLAG);
    if (result)
        return 60 + result;
    if (eigrp_auth_sequence_newer(htonl(0), htonl(1))
        || eigrp_auth_sequence_newer(htonl(UINT32_MAX), htonl(1))
        || !eigrp_auth_sequence_newer(htonl(UINT32_MAX), 0))
        return 70;

    /* Missing keys cannot consume a transmit sequence. */
    interface_init(&ei, &nbr);
    ei.crypt_seqnum = 123;
    ei.params.auth_keychain = "missing-chain";
    s = packet_create();
    if (!s || eigrp_auth_tlv_md5_encode(s, &ei) != 0
        || ei.crypt_seqnum != 123)
        return 71;
    eigrp_stream_free(s);
    return 0;
}
'''
    linker_gc = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    with tempfile.TemporaryDirectory() as tmp:
        source_path = Path(tmp) / 'auth_sequence_test.c'
        binary_path = Path(tmp) / 'auth_sequence_test'
        source_path.write_text(source)
        subprocess.run([
            'cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
            '-DEIGRP_DISABLE_SNMP', '-Wall', '-Wextra',
            '-Wno-unused-parameter', '-ffunction-sections', '-fdata-sections',
            f'-I{CODE}', str(source_path), str(CODE / 'eigrp_md5.c'),
            str(CODE / 'eigrp_sha256.c'), str(CODE / 'eigrp_memory.c'),
            str(CODE / 'eigrp_stream.c'),
            linker_gc, '-pthread', '-o', str(binary_path),
        ], check=True, cwd=ROOT)
        subprocess.run([str(binary_path)], check=True, cwd=ROOT)
