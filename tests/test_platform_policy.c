#include "../src/platform_policy.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    ninefan_platform_identity identity = {
        .model = "Mac17,9",
        .chip = "Apple M5 Pro",
        .os_build = "25F84",
        .fan_count = 2,
        .mode_key_format = "F%dmd",
        .schema = "651d1eadd3e88f2a",
    };
    assert(ninefan_platform_profile_match(&identity) != NULL);
    char message[128] = {0};
    assert(ninefan_platform_policy_explain(
        &identity, message, sizeof(message)));
    assert(strstr(message, "verified") != NULL);

    identity.os_build = "different";
    assert(ninefan_platform_profile_match(&identity) == NULL);
    assert(!ninefan_platform_policy_explain(
        &identity, message, sizeof(message)));
    assert(strstr(message, "unsupported") != NULL);

    identity.os_build = "25F84";
    identity.fan_count = 1;
    assert(ninefan_platform_profile_match(&identity) == NULL);
    identity.fan_count = 2;
    identity.model = "Mac17,10";
    assert(ninefan_platform_profile_match(&identity) == NULL);
    identity.model = "Mac17,9";
    identity.chip = "Apple M5 Max";
    assert(ninefan_platform_profile_match(&identity) == NULL);
    identity.chip = "Apple M5 Pro";
    identity.mode_key_format = "F%dMd";
    assert(ninefan_platform_profile_match(&identity) == NULL);
    identity.mode_key_format = "F%dmd";
    identity.schema = "0000000000000000";
    assert(ninefan_platform_profile_match(&identity) == NULL);
    assert(ninefan_platform_profile_match(NULL) == NULL);
    puts("platform policy tests passed");
    return 0;
}
