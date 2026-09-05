#ifndef CSI_TEST_STUBS_H
#define CSI_TEST_STUBS_H

#include "../src/state.h"

/* Initialises g_state / g_config to daemon-startup values. */
void stub_reset(void);

/* Last event published per subtopic, and how many times. */
const char *stub_last_event(const char *subtopic);
int         stub_event_count(const char *subtopic);
void        stub_clear_events(void);

#endif /* CSI_TEST_STUBS_H */
