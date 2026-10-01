// reset.c — вынос "reset" (kernel/shell/commands/colors.c,
// command_reset(), мёртвый код): цвета по умолчанию (белый на чёрном).

#include <stdio.h>
#include "../common/console.h"

int main(void) {
    con_set_fg(CON_WHITE);
    con_set_bg(CON_BLACK);
    return 0;
}
