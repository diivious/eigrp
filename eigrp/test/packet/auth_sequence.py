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
    static const char secret[] = "eigrprocks";
    if (strcmp(name, "sequence-test") != 0 || sizeof(secret) > capacity)
        return false;
    if (id)
        *id = 11;
    memcpy(key, secret, sizeof(secret));
    return true;
}
void eigrp_log(eigrp_log_level_t level, const char *format, ...)
{
    (void)level; (void)format;
}
const char *eigrp_intf_name_string(eigrp_intf_t *ei)
{
    return ei->name;
}
static eigrp_stream_t *packet_create(uint8_t opcode, uint32_t rtp_seq,
                                    uint32_t flags)
{
    eigrp_stream_t *s = eigrp_stream_create(128);
    struct eigrp_header header = {0};
    header.opcode = opcode;
    header.sequence = htonl(rtp_seq);
    header.flags = htonl(flags);
    if (!s || eigrp_stream_put(s, &header, EIGRP_HEADER_LEN) != EIGRP_HEADER_LEN)
        return NULL;
    return s;
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
static int test_wire_packet(eigrp_intf_t *ei, eigrp_nbr_t *nbr,
                            uint32_t crypto_seq, uint32_t rtp_seq,
                            uint8_t opcode, uint32_t flags)
{
    eigrp_stream_t *s = packet_create(opcode, rtp_seq, flags);
    struct TLV_MD5_Authentication_Type *md5;
    struct TLV_SHA256_Authentication_Type *sha;
    bool is_md5 = ei->params.auth_type == EIGRP_AUTH_TYPE_MD5;
    uint32_t previous = ei->crypt_seqnum;
    int valid;

    if (!s)
        return 1;
    if (is_md5 ? eigrp_auth_tlv_md5_encode(s, ei) != EIGRP_AUTH_MD5_TLV_SIZE
               : eigrp_auth_tlv_sha256_encode(s, ei) != EIGRP_AUTH_SHA256_TLV_SIZE)
        return 2;
    md5 = (void *)(s->data + EIGRP_HEADER_LEN);
    sha = (void *)md5;
    if (md5->key_sequence != 0 || ei->crypt_seqnum != previous)
        return 3; /* Packet construction cannot consume crypto sequences. */
    if (!eigrp_auth_transmit_finalize(ei, s, (uint16_t)s->endp))
        return 4;
    if (ntohl(md5->key_sequence) != crypto_seq || ei->crypt_seqnum != crypto_seq)
        return 5;
    if (ntohl(((struct eigrp_header *)s->data)->sequence) != rtp_seq
        || eigrp_checksum(s->data, s->endp) != 0)
        return 6;
    valid = is_md5 ? eigrp_check_md5_digest(s, md5, nbr,
                            opcode == EIGRP_OPC_HELLO ? EIGRP_AUTH_BASIC_HELLO_FLAG
                            : flags & EIGRP_INIT_FLAG ? EIGRP_AUTH_UPDATE_INIT_FLAG
                            : EIGRP_AUTH_UPDATE_FLAG)
                   : eigrp_check_sha256_digest(s, sha, nbr, EIGRP_AUTH_UPDATE_FLAG);
    if (!valid || ntohl(nbr->crypt_seqnum) != crypto_seq)
        return 7;
    if ((is_md5 ? eigrp_check_md5_digest(s, md5, nbr, EIGRP_AUTH_UPDATE_FLAG)
                : eigrp_check_sha256_digest(s, sha, nbr, EIGRP_AUTH_UPDATE_FLAG)))
        return 8; /* Received duplicates must be rejected. */
    eigrp_stream_free(s);
    return 0;
}
int main(void)
{
    eigrp_intf_t ei;
    eigrp_nbr_t nbr;
    eigrp_stream_t *s;
    int r;

    interface_init(&ei, &nbr);
    ei.crypt_seqnum = 0xefffffffU;
    r = test_wire_packet(&ei, &nbr, 0xf0000000U, 0, EIGRP_OPC_HELLO, 0);
    if (r) return 10 + r;
    r = test_wire_packet(&ei, &nbr, 0xf0000001U, 100, EIGRP_OPC_UPDATE, EIGRP_INIT_FLAG);
    if (r) return 30 + r;
    /* Same RTP sequence retransmitted, fresh authenticated transmission. */
    r = test_wire_packet(&ei, &nbr, 0xf0000002U, 100, EIGRP_OPC_UPDATE, EIGRP_INIT_FLAG);
    if (r) return 50 + r;

    ei.params.auth_type = EIGRP_AUTH_TYPE_SHA256;
    ei.params.auth_password = "sha256-sequence-secret";
    r = test_wire_packet(&ei, &nbr, 0xf0000003U, 100, EIGRP_OPC_UPDATE, 0);
    if (r) return 70 + r;
    r = test_wire_packet(&ei, &nbr, 0xf0000004U, 100, EIGRP_OPC_UPDATE, 0);
    if (r) return 90 + r;

    interface_init(&ei, &nbr);
    ei.crypt_seqnum = UINT32_MAX - 1;
    r = test_wire_packet(&ei, &nbr, UINT32_MAX, 0, EIGRP_OPC_HELLO, 0);
    if (r) return 110 + r;
    r = test_wire_packet(&ei, &nbr, 1, 0, EIGRP_OPC_HELLO, 0);
    if (r) return 130 + r;

    interface_init(&ei, &nbr);
    ei.crypt_seqnum = 123;
    ei.params.auth_keychain = "missing-chain";
    s = packet_create(EIGRP_OPC_HELLO, 0, 0);
    if (!s || eigrp_auth_tlv_md5_encode(s, &ei) != 0
        || ei.crypt_seqnum != 123)
        return 151;
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
            str(CODE / 'eigrp_checksum.c'),
            linker_gc, '-pthread', '-o', str(binary_path),
        ], check=True, cwd=ROOT)
        subprocess.run([str(binary_path)], check=True, cwd=ROOT)
