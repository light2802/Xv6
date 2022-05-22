#include "param.h"
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fs.h"
#include "fcntl.h"
#include "syscall.h"
#include "traps.h"
#include "memlayout.h"

//static char  buf[8192];
//static char  name[3];
//static char *echoargv[] = {"echo", "ALL", "TESTS", "PASSED", 0};
static int   stdout     = 1;
#define TOTAL_MEMORY (1 << 21) + (1 << 18) + (1 << 17)

void test_1(void) {
    void *m1    = 0, *m2, *start;
    uint  cur   = 0;
    uint  count = 0;
    uint  total_count;

    printf(stdout, "test 1\n");

    m1 = malloc(4096);
    if (m1 == 0) goto failed;
    start = m1;

    while (cur < TOTAL_MEMORY) {
        m2 = malloc(4096);
        if (m2 == 0) goto failed;
        *(char **)m1   = m2;
        ((int *)m1)[2] = count++;
        m1             = m2;
        cur += 4096;
    }
    ((int *)m1)[2] = count;
    total_count    = count;

    count = 0;
    m1    = start;

    while (count != total_count) {
        if (((int *)m1)[2] != count) goto failed;
        m1 = *(char **)m1;
        count++;
    }

    printf(stdout, "test 1 ok\n");
    exit();
failed:
    printf(stdout, "test 1 failed!\n");
    exit();
}
