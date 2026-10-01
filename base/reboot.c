// reboot.c — вынос "reboot" (kernel/shell/commands/system.c,
// command_reboot(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение). Вся логика (синк LufiraFS + 8042-сброс)
// теперь в SYS_REBOOT (root-only) — см. комментарий там.

#include <stdio.h>
#include <lufira/syscall.h>

int main(void) {
    printf("Rebooting...\n");
    long r = sys_reboot();
    if (r == -EPERM) printf("reboot: permission denied (root only)\n");
    else printf("reboot: failed\n");
    return 1;
}
