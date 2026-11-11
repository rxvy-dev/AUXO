#include <stdio.h>
#include <stdlib.h>

#include <auxo/string.h>

static int tests_passed;
static int tests_failed;

#define CHECK(name, condition) do {                 \
    if (condition) {                                \
        printf("[PASS] %s\n", name);                \
        tests_passed++;                             \
    } else {                                        \
        printf("[FAIL] %s\n", name);                \
        tests_failed++;                             \
    }                                               \
} while (0)

int main(void)
{
    char buffer[32];
    char overlap[] = "abcdef";

    CHECK("strlen", auxo_strlen("AuxoBSD") == 7);
    CHECK("strcmp equal", auxo_strcmp("auxo", "auxo") == 0);
    CHECK("strcmp different", auxo_strcmp("abc", "abd") < 0);
    CHECK("strncmp", auxo_strncmp("auxobsd", "auxo", 4) == 0);

    auxo_memset(buffer, 0, sizeof(buffer));
    CHECK("memset", buffer[0] == 0 && buffer[31] == 0);

    auxo_memcpy(buffer, "AuxoBSD", 8);
    CHECK("memcpy", auxo_strcmp(buffer, "AuxoBSD") == 0);

    CHECK("memcmp equal", auxo_memcmp("abc", "abc", 3) == 0);
    CHECK("memcmp different", auxo_memcmp("abc", "abd", 3) < 0);

    auxo_memmove(overlap + 2, overlap, 4);
    CHECK("memmove overlap", auxo_memcmp(overlap, "ababcd", 6) == 0);

    printf("\nResults: %d passed, %d failed\n",
           tests_passed, tests_failed);

    return tests_failed != 0;
}
