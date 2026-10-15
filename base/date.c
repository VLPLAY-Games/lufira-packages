// date.c — текущее время по CMOS RTC (SYS_RTC_GETTIME, обёртка над
// rtc_get_time() в kernel/system/timer/rtc.c) — тот же вынос команды из
// ядра в отдельную userspace-программу, что и у free.c/whoami.c.
#include <stdio.h>
#include <lufira/syscall.h>

// Этот printf() не умеет ширину/нулевое дополнение (%02u) — см. комментарий
// в libc/src/printf.c, "простой printf" без флагов нарочно. Дополняем сами.
static void print2(unsigned v) {
    printf("%u%u", (v / 10) % 10, v % 10);
}

int main(void) {
    struct lufira_rtc_time t;
    long r = sys_rtc_gettime(&t);
    if (r != 0) {
        printf("date: SYS_RTC_GETTIME failed (%ld)\n", r);
        return 1;
    }

    printf("%u", t.year);
    printf("-"); print2(t.month);
    printf("-"); print2(t.day);
    printf(" "); print2(t.hour);
    printf(":"); print2(t.minute);
    printf(":"); print2(t.second);
    printf(" UTC\n");
    return 0;
}
