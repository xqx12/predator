#include "../sl.h"
#include <stdlib.h>

struct item {
    struct item *next;
};

struct item* alloc_or_die(void)
{
    struct item *pi = malloc(sizeof(*pi));
    if (pi)
        return pi;
    else
        abort();
}

struct item* create_sll(void)
{
    struct item *sll = alloc_or_die();
    struct item *now = sll;

    // NOTE: running this on bare metal may cause the machine to swap a bit
    int i;
    for (i = 1; i; ++i) {
        now->next = alloc_or_die();
        now->next->next = NULL;
        now = now->next;
        ___sl_plot_by_ptr(sll, "01-sll-append-done");
    }

    return sll;
}

int main()
{
    struct item *sll = create_sll();
    ___sl_plot_by_ptr(sll, "02-sll-ready");
    return 0;
}
