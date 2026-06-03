#define STRINGBUF_IMPLEMENTATION
#include "stringbuf.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void)
{
    int failures = 0;

    /* --- Basic integer and string scan --- */
    {
        StringBuf sb;
        stringbuf_init_str(&sb, "answer=42 name=test", strlen("answer=42 name=test"), 0);
        int num = 0;
        char name[32] = {0};
        int n = stringbuf_sscanf(&sb, "answer=%d name=%31s", &num, name);
        if (n != 2 || num != 42 || strcmp(name, "test") != 0) {
            fprintf(stderr, "FAIL basic scan: n=%d num=%d name=%s\n", n, num, name);
            failures++;
        } else {
            printf("PASS basic scan\n");
        }
        stringbuf_free(&sb);
    }

    /* --- URL with IPv6 literal (the motivating case) --- */
    {
        StringBuf sb;
        stringbuf_init_str(&sb, "https://[::]:8080/", strlen("https://[::]:8080/"), 0);
        char scheme[16] = {0};
        char host[64] = {0};
        int port = 0;
        int n = stringbuf_sscanf(&sb, "%15[^:]://[%63[^]]]:%d/", scheme, host, &port);
        if (n != 3 || strcmp(scheme, "https") != 0 || strcmp(host, "::") != 0 || port != 8080) {
            fprintf(stderr, "FAIL URL scan: n=%d scheme=%s host=%s port=%d\n",
                    n, scheme, host, port);
            failures++;
        } else {
            printf("PASS URL IPv6 scan\n");
        }
        stringbuf_free(&sb);
    }

    /* --- Failure case (no match) --- */
    {
        StringBuf sb;
        stringbuf_init_str(&sb, "not-a-url", strlen("not-a-url"), 0);
        int dummy = 0;
        int n = stringbuf_sscanf(&sb, "https://%d", &dummy);
        if (n != 0) {
            fprintf(stderr, "FAIL failure case: expected 0 conversions, got %d\n", n);
            failures++;
        } else {
            printf("PASS failure case\n");
        }
        stringbuf_free(&sb);
    }

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        printf("%d test(s) failed.\n", failures);
        return 1;
    }
}