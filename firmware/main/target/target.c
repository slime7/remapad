#include "target.h"

#include <stddef.h>

static const pad_target_t *s_target;

void target_set(const pad_target_t *target)
{
    s_target = target;
}

const pad_target_t *target_get(void)
{
    return s_target;
}

const char *target_name(void)
{
    return s_target != NULL ? s_target->name : "none";
}

void target_set_facts(const pad_target_facts_t *facts)
{
    if (s_target != NULL && s_target->set_facts != NULL && facts != NULL) {
        s_target->set_facts(facts);
    }
}

void target_send_pad(const pad_state_t *pad)
{
    if (s_target != NULL && s_target->send_pad != NULL && pad != NULL) {
        s_target->send_pad(pad);
    }
}
