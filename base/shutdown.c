// shutdown.c — вынос "shutdown" (kernel/shell/commands/system.c,
// command_shutdown(), мёртвый код) в отдельную userspace-программу (v0.7
// план, этап 5, продолжение). Вся логика (синк LufiraFS + ACPI shutdown)
// теперь в SYS_SHUTDOWN (root-only) — см. комментарий там.

#include <stdio.h>
#include <lufira/syscall.h>

int main(void) {
    printf("Shutting down...\n");
    long r = sys_shutdown();
    if (r == -EPERM) printf("shutdown: permission denied (root only)\n");
    else printf("shutdown: ACPI shutdown failed, system may require manual power off\n");
    return 1;
}
