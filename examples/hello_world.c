#include "dumpster.h"
#include <stdio.h>
#include <string.h>

int main(void) {
        char *message;

        dumpster_init();

        message = dumpster_alloc(32);

        if (message == NULL) {
                printf("Something went wrong and memory couldn't be allocated...\n");
                return 1;
        } else {
                strcpy(message, "Hello, world!");
                printf("%s\n", message);
        }

        dumpster_collect_incremental();
        return 0;
}
