#include <stdio.h>
#include "dumpster.h"

int main(void) {
        int i;

        dumpster_init();

        int *v = dumpster_alloc(sizeof(*v));

        if (v == NULL) {
                perror("Memory allocation failed");
                return -1;
        }

        while (scanf("%d", v)) {
                printf("--- Read: %d ---\n", *v);
                v = dumpster_alloc(sizeof(*v));
                if (v == NULL) {
                        perror("Memory allocation failed");
                        return -1;
                }

                printf("--- Fragmentation: %f ---\n", compute_fragmentation());
                print_statistics(0);
        }

        dumpster_collect_incremental();

        return 0;
}
