// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * In-memory Unix keychain provider for standalone EIGRP development.
 *
 * No secrets are built in. Unix configuration or the UUT must explicitly
 * register a key before portable EIGRP can authenticate using that keychain.
 * The host-facing eigrp_sys_auth_key_lookup() symbol is implemented here.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp_const.h"
#include "eigrp_sys.h"
#include "eigrp_unix_keychain.h"

typedef struct eigrp_unix_keychain {
    struct eigrp_unix_keychain *next;
    char *name;
    uint32_t id;
    char secret[PLAINTEXT_LENGTH + 1];
} eigrp_unix_keychain_t;

static pthread_mutex_t keychain_lock = PTHREAD_MUTEX_INITIALIZER;
static eigrp_unix_keychain_t *keychains;

/* Use explicit writes when releasing secrets to prevent dead-store removal. */
static void eigrp_unix_keychain_scrub(void *memory, size_t length)
{
    volatile unsigned char *ptr = memory;
    while (length--)
        *ptr++ = 0;
}

eigrp_result_t eigrp_unix_keychain_add(const char *name, uint32_t key_id,
                                       const char *secret)
{
    eigrp_unix_keychain_t *entry;
    size_t secret_length, name_length;
    eigrp_result_t result = EIGRP_RESULT_SUCCESS;

    if (!name || !secret || !key_id)
        return EIGRP_RESULT_INVALID_ARGUMENT;
    name_length = strlen(name);
    secret_length = strlen(secret);
    if (!name_length || name_length >= 64 || !secret_length
        || secret_length > PLAINTEXT_LENGTH)
        return EIGRP_RESULT_INVALID_ARGUMENT;

    pthread_mutex_lock(&keychain_lock);
    for (entry = keychains; entry; entry = entry->next) {
        if (strcmp(entry->name, name) != 0)
            continue;
        if (entry->id != key_id || strcmp(entry->secret, secret) != 0)
            result = EIGRP_RESULT_INVALID_ARGUMENT;
        goto out;
    }

    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        result = EIGRP_RESULT_INTERNAL_FAILURE;
        goto out;
    }
    entry->name = malloc(name_length + 1);
    if (!entry->name) {
        free(entry);
        result = EIGRP_RESULT_INTERNAL_FAILURE;
        goto out;
    }
    memcpy(entry->name, name, name_length + 1);
    memcpy(entry->secret, secret, secret_length + 1);
    entry->id = key_id;
    entry->next = keychains;
    keychains = entry;
out:
    pthread_mutex_unlock(&keychain_lock);
    return result;
}

bool eigrp_sys_auth_key_lookup(const char *name, uint32_t *key_id,
                               char *key, size_t size)
{
    eigrp_unix_keychain_t *entry;
    bool found = false;

    if (!name || !key || !size)
        return false;

    pthread_mutex_lock(&keychain_lock);
    for (entry = keychains; entry; entry = entry->next) {
        size_t length;
        if (strcmp(entry->name, name) != 0)
            continue;
        length = strlen(entry->secret);
        if (length >= size)
            break;
        memcpy(key, entry->secret, length + 1);
        if (key_id)
            *key_id = entry->id;
        found = true;
        break;
    }
    pthread_mutex_unlock(&keychain_lock);
    return found;
}

void eigrp_unix_keychain_reset(void)
{
    eigrp_unix_keychain_t *entry, *next;

    pthread_mutex_lock(&keychain_lock);
    entry = keychains;
    keychains = NULL;
    while (entry) {
        next = entry->next;
        eigrp_unix_keychain_scrub(entry->secret, sizeof(entry->secret));
        free(entry->name);
        free(entry);
        entry = next;
    }
    pthread_mutex_unlock(&keychain_lock);
}
