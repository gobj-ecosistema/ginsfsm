/****************************************************************************
 *          INTER_EVENT.H
 *          Eventos entre yunos
 *          Copyright (c) 2013-2016 Niyamaka.
 *          All Rights Reserved.
 ****************************************************************************/
#pragma once

#include <ghelpers.h>
#include "10_gobj.h"

#ifdef __cplusplus
extern "C"{
#endif

/*********************************************************************
 *      Structures
 *********************************************************************/
typedef struct {
    char event[64];  // WARNING changed in version 3.5.1, recompile all
    json_t *kw;
} iev_msg_t;

/*********************************************************************
 *      Prototypes
 *********************************************************************/
/*
 *  Return an inter-event object
{
    "event": event,
    "kw": kw
}
 */
PUBLIC json_t *iev_create(
    const char *event,
    json_t *kw // owned
);

PUBLIC json_t *iev_create2(
    const char *event,
    json_t *webix_msg, // owned
    json_t *kw // owned
);

/*
 *  Trace inter-events with metadata of kw
 */
PUBLIC void trace_inter_event(const char *prefix, const char *event, json_t *kw);
/*
 *  Trace inter-events with full kw
 */
PUBLIC void trace_inter_event2(const char *prefix, const char *event, json_t *kw);

/*-----------------------------------------------------*
 *  Incorporate event's messages FROM outside world.
 *-----------------------------------------------------*/
PUBLIC int iev_create_from_gbuffer(
    iev_msg_t *iev_msg,
    GBUFFER *gbuf,  // WARNING gbuf own and data consumed
    int verbose     // 1 log, 2 log+dump
);
PUBLIC int iev_create_from_file(iev_msg_t *iev_msg, const char *path);

/*-----------------------------------------------------*
 *  Useful to send event's messages TO outside world.
 *-----------------------------------------------------*/
PUBLIC GBUFFER *iev2gbuffer(
    json_t *jn_iev,
    BOOL pretty
);


#ifdef __cplusplus
}
#endif
