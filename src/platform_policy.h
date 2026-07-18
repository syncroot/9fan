#ifndef NINEFAN_PLATFORM_POLICY_H
#define NINEFAN_PLATFORM_POLICY_H

#include <stddef.h>

typedef struct {
    const char *model;
    const char *chip;
    const char *os_build;
    int fan_count;
    const char *mode_key_format;
    const char *schema;
} ninefan_platform_identity;

typedef struct {
    ninefan_platform_identity identity;
    const char *evidence;
} ninefan_platform_profile;

const ninefan_platform_profile *ninefan_platform_profile_match(
    const ninefan_platform_identity *identity);

int ninefan_platform_policy_explain(
    const ninefan_platform_identity *identity,
    char *message,
    size_t message_size);

#endif
