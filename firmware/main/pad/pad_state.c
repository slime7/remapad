#include "pad_state.h"

#include <string.h>

void pad_state_defaults(pad_state_t *state)
{
  memset(state, 0, sizeof(*state));
  for (size_t i = 0; i < PAD_AXIS_COUNT; i++) {
    state->axis[i] = PAD_AXIS_CENTER;
  }
  state->family = PAD_FAMILY_UNKNOWN;
  state->conn = PAD_CONN_UNKNOWN;
}

void pad_feedback_defaults(pad_feedback_t *feedback)
{
  memset(feedback, 0, sizeof(*feedback));
}
