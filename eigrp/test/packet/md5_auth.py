# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
#
# Exercise portable MD5 packet authentication against real stream/MD5 code.
# Source is included so the wire computation is checked without a host adapter.

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"


def test_md5_digest_bounds_coverage_key_and_stream_integrity():
    source = r'''
#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "eigrp_auth.c"

static const char *active_key = "md5-secret";

bool eigrp_sys_auth_key_lookup(const char *name, uint32_t *id,
                               char *key, size_t capacity)
{
    size_t length = strlen(active_key);
    if (strcmp(name, "test-chain") || length >= capacity)
        return false;
    if (id)
        *id = 9;
    memcpy(key, active_key, length + 1);
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

static void expected_digest(const uint8_t *packet, size_t length,
                            uint8_t flags, uint8_t result[16])
{
    eigrp_md5_ctx_t context;
    uint8_t pad[16] = {0};
    size_t key_length = strlen(active_key);

    eigrp_md5_init(&context);
    eigrp_md5_update(&context, packet, 44);
    eigrp_md5_update(&context, active_key, (unsigned int)key_length);
    if (key_length < 16)
        eigrp_md5_update(&context, pad, (unsigned int)(16 - key_length));
    if (flags != EIGRP_AUTH_BASIC_HELLO_FLAG && length > 60)
        eigrp_md5_update(&context, packet + 60, (unsigned int)(length - 60));
    eigrp_md5_final(result, &context);
}

static int run_case(size_t length, uint8_t flags)
{
    eigrp_stream_t *stream = eigrp_stream_create(length);
    eigrp_intf_t ei = {0};
    eigrp_nbr_t nbr = {0};
    struct TLV_MD5_Authentication_Type *tlv;
    struct eigrp_header *header;
    uint8_t expected[16];
    uint8_t bytes[256];
    size_t index;
    int result = 0;

    if (!stream || length > sizeof(bytes))
        return 1;
    for (index = 0; index < length; index++)
        bytes[index] = (uint8_t)(index * 17 + 5);
    if (eigrp_stream_put(stream, bytes, length) != length)
        return 2;

    ei.name = "md5-unit-test";
    ei.params.auth_type = EIGRP_AUTH_TYPE_MD5;
    ei.params.auth_keychain = "test-chain";
    nbr.ei = &ei;
    tlv = (struct TLV_MD5_Authentication_Type *)(stream->data + 20);
    header = (struct eigrp_header *)stream->data;
    header->checksum = 0;
    tlv->type = htons(EIGRP_TLV_AUTH);
    tlv->length = htons(EIGRP_AUTH_MD5_TLV_SIZE);
    tlv->auth_type = htons(EIGRP_AUTH_TYPE_MD5);
    tlv->auth_length = htons(EIGRP_AUTH_TYPE_MD5_LEN);
    tlv->key_sequence = htonl(1);
    tlv->key_id = htonl(9);
    memset(tlv->digest, 0, sizeof(tlv->digest));

    eigrp_stream_set_getp(stream, 7);
    if (eigrp_make_md5_digest(&ei, stream, flags) != 16) {
        result = 3;
        goto done;
    }
    if (stream->getp != 7 || stream->endp != length) {
        result = 4;
        goto done;
    }
    expected_digest(stream->data, length, flags, expected);
    if (memcmp(tlv->digest, expected, 16)) {
        result = 5;
        goto done;
    }

    /* Verification must restore both the wire checksum and digest. */
    header->checksum = htons(0x1234);
    if (!eigrp_check_md5_digest(stream, tlv, &nbr, flags)) {
        result = 6;
        goto done;
    }
    if (header->checksum != htons(0x1234)
        || memcmp(tlv->digest, expected, 16)
        || nbr.crypt_seqnum != htonl(1)) {
        result = 7;
        goto done;
    }
    /* A repeated packet must not pass the unchanged replay guard. */
    if (eigrp_check_md5_digest(stream, tlv, &nbr, flags)) {
        result = 8;
        goto done;
    }

    /* For non-HELLO packets every byte after the auth TLV is protected. */
    if (flags != EIGRP_AUTH_BASIC_HELLO_FLAG && length > 60) {
        nbr.crypt_seqnum = 0;
        stream->data[length - 1] ^= 0xff;
        if (eigrp_check_md5_digest(stream, tlv, &nbr, flags)
            || nbr.crypt_seqnum != 0) {
            result = 9;
            goto done;
        }
        stream->data[length - 1] ^= 0xff;
    }

    /* An INIT digest signed with one key cannot verify with another. */
    if (flags == EIGRP_AUTH_UPDATE_INIT_FLAG) {
        nbr.crypt_seqnum = 0;
        active_key = "wrong-secret";
        if (eigrp_check_md5_digest(stream, tlv, &nbr, flags)
            || nbr.crypt_seqnum != 0) {
            result = 10;
            goto done;
        }
        active_key = "md5-secret";
    }

done:
    eigrp_stream_free(stream);
    return result;
}

int main(void)
{
    uint8_t short_packet[60] = {0};
    uint8_t digest[16] = {0};
    const size_t lengths[] = {60, 61, 79, 80, 81, 140, 256};
    size_t index;
    int failure;

    /* Short or oversized advertised lengths cannot reach MD5 update. */
    if (eigrp_auth_md5_compute(short_packet, 59, "secret",
                               EIGRP_AUTH_UPDATE_FLAG, digest))
        return 1;
#if SIZE_MAX > UINT_MAX
    if (eigrp_auth_md5_compute(short_packet, (size_t)UINT_MAX + 1,
                               "secret", EIGRP_AUTH_UPDATE_FLAG, digest))
        return 2;
#endif

    {
        eigrp_stream_t *short_stream = eigrp_stream_create(59);
        eigrp_intf_t ei = {0};
        eigrp_nbr_t nbr = {0};
        struct TLV_MD5_Authentication_Type *short_tlv;

        if (!short_stream)
            return 3;
        short_stream->endp = 59;
        ei.params.auth_keychain = "test-chain";
        nbr.ei = &ei;
        short_tlv = (struct TLV_MD5_Authentication_Type *)(short_stream->data + 20);
        if (eigrp_make_md5_digest(&ei, short_stream, EIGRP_AUTH_UPDATE_FLAG)
            || eigrp_check_md5_digest(short_stream, short_tlv, &nbr,
                                      EIGRP_AUTH_UPDATE_FLAG))
            return 4;
        /* A corrupt stream boundary must also be rejected. */
        short_stream->endp = short_stream->size + 1;
        if (eigrp_make_md5_digest(&ei, short_stream, EIGRP_AUTH_UPDATE_FLAG)
            || eigrp_check_md5_digest(short_stream, short_tlv, &nbr,
                                      EIGRP_AUTH_UPDATE_FLAG))
            return 5;
        eigrp_stream_free(short_stream);
    }

    for (index = 0; index < sizeof(lengths)/sizeof(lengths[0]); index++) {
        failure = run_case(lengths[index], EIGRP_AUTH_UPDATE_FLAG);
        if (failure) {
            fprintf(stderr, "UPDATE length %zu: error %d\n", lengths[index], failure);
            return 10 + failure;
        }
        failure = run_case(lengths[index], EIGRP_AUTH_UPDATE_INIT_FLAG);
        if (failure) {
            fprintf(stderr, "INIT length %zu: error %d\n", lengths[index], failure);
            return 30 + failure;
        }
    }
    active_key = "this-is-a-key-longer-than-sixteen";
    failure = run_case(100, EIGRP_AUTH_UPDATE_FLAG);
    if (failure)
        return 45 + failure;
    active_key = "md5-secret";
    failure = run_case(60, EIGRP_AUTH_BASIC_HELLO_FLAG);
    if (failure)
        return 50 + failure;
    failure = run_case(80, EIGRP_AUTH_BASIC_HELLO_FLAG);
    if (failure)
        return 60 + failure;
    return 0;
}
'''
    linker_gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "md5_test.c"
        exe = Path(tmp) / "md5_test"
        src.write_text(source)
        subprocess.run(
            [
                "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L",
                "-DEIGRP_DISABLE_SNMP", "-Wall", "-Wextra",
                "-Wno-unused-parameter", "-ffunction-sections", "-fdata-sections",
                f"-I{CODE}", str(src), str(CODE / "eigrp_md5.c"),
                str(CODE / "eigrp_stream.c"), linker_gc, "-pthread",
                "-o", str(exe),
            ],
            check=True,
            cwd=ROOT,
        )
        subprocess.run([str(exe)], check=True, cwd=ROOT)
