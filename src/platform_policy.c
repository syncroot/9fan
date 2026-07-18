#include "platform_policy.h"

#include <stdio.h>
#include <string.h>

static const ninefan_platform_profile supported_profiles[] = {
    {
        .identity = {
            .model = "Mac17,9",
            .chip = "Apple M5 Pro",
            .os_build = "25F84",
            .fan_count = 2,
            .mode_key_format = "F%dmd",
            .schema = "651d1eadd3e88f2a",
        },
        .evidence = "locally validated 2026-07-18",
    },
};

static int field_equal(const char *left, const char *right) {
    return left && right && strcmp(left, right) == 0;
}

const ninefan_platform_profile *ninefan_platform_profile_match(
    const ninefan_platform_identity *identity) {
    if (!identity) return NULL;
    const size_t count =
        sizeof(supported_profiles) / sizeof(supported_profiles[0]);
    for (size_t index = 0; index < count; index++) {
        const ninefan_platform_identity *known =
            &supported_profiles[index].identity;
        if (field_equal(identity->model, known->model)
            && field_equal(identity->chip, known->chip)
            && field_equal(identity->os_build, known->os_build)
            && identity->fan_count == known->fan_count
            && field_equal(identity->mode_key_format, known->mode_key_format)
            && field_equal(identity->schema, known->schema)) {
            return &supported_profiles[index];
        }
    }
    return NULL;
}

int ninefan_platform_policy_explain(
    const ninefan_platform_identity *identity,
    char *message,
    size_t message_size) {
    if (ninefan_platform_profile_match(identity)) {
        if (message && message_size > 0) {
            snprintf(message, message_size, "verified platform profile");
        }
        return 1;
    }
    if (message && message_size > 0) {
        snprintf(message, message_size,
            "unsupported platform profile; status and Apple-default recovery "
            "remain available");
    }
    return 0;
}
