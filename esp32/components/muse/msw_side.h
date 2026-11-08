/*
 * museStopWatch: side chats. A side chat is a conversation of its own on
 * Muse, apart from the main chat: the device makes a UUID and sends it as
 * session_id with each message (muse_chat_session.cpp); Muse makes the
 * conversation on its first message. The device keeps the ones it made
 * (up to MSW_SIDE_MAX, in NVS) and which one is in use. Muse has no list of
 * them to read back, so this list is the only record of their IDs.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSW_SIDE_MAX 8
#define MSW_SIDE_ID_LEN 36
#define MSW_SIDE_NAME_MAX 23

typedef struct {
    char id[MSW_SIDE_ID_LEN + 1];       /* lowercase UUID */
    char name[MSW_SIDE_NAME_MAX + 1];   /* "Side 3" */
} msw_side_t;

/* Loads the list and points the chat at the one in use. Call once at boot. */
void msw_side_init(void);

int msw_side_count(void);
bool msw_side_get(int i, msw_side_t *out);
/* The side chat in use, or -1 for the main chat. */
int msw_side_current(void);
/* The index of the side chat with this ID, or -1. */
int msw_side_find(const char *id);
/* i = -1 goes back to the main chat. */
bool msw_side_select(int i);
#define MSW_SIDE_FULL (-1)
#define MSW_SIDE_BAD_ID (-2)
/* Adds a side chat and switches to it. id is one Muse already has (ask Muse
 * in that side chat for the side chat's own ID, not the agent's session ID,
 * which it may also give), any case; NULL or "" makes a new
 * one, whose conversation starts with the next message. An ID already listed
 * is just chosen. Returns its index, MSW_SIDE_FULL or MSW_SIDE_BAD_ID. */
int msw_side_add(const char *id);
/* Forgets the ID here (the conversation stays on Muse). Deleting the one in
 * use goes back to the main chat. */
bool msw_side_delete(int i);

/* muse_chat_session.cpp: "" for the main chat. */
bool muse_hatch_set_session_id(const char *id);

#ifdef __cplusplus
}
#endif
