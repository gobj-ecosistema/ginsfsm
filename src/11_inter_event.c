/***********************************************************************
 *          INTER_EVENT.C
 *          Eventos entre yunos
 *          Copyright (c) 2013-2014 Niyamaka.
 *          All Rights Reserved.
 ***********************************************************************/
#include <string.h>
#include "11_inter_event.h"

/****************************************************************
 *         Constants
 ****************************************************************/

/****************************************************************
 *         Structures
 ****************************************************************/

/****************************************************************
 *         Data
 ****************************************************************/


/****************************************************************
 *         Prototypes
 ****************************************************************/
PRIVATE int iev_create_from_json(iev_msg_t *iev_msg, json_t *jn_msg);


/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t* iev_create(
    const char *event,
    json_t *kw // owned
)
{
    if(empty_string(event)) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "event NULL",
            "process",      "%s", get_process_name(),
            "hostname",     "%s", get_host_name(),
            "pid",          "%d", get_pid(),
            NULL
        );
        return 0;
    }

    if(!kw) {
        kw = json_object();
    }
    kw = kw_serialize(
        kw  // owned
    );
    json_t *jn_iev = json_pack("{s:s, s:o}",
        "event", event,
        "kw", kw
    );
    if(!jn_iev) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "json_pack() FAILED",
            "process",      "%s", get_process_name(),
            "hostname",     "%s", get_host_name(),
            "pid",          "%d", get_pid(),
            NULL
        );
    }
    return jn_iev;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC void trace_inter_event(const char *prefix, const char *event, json_t *kw)
{
    json_t * kw_compact = json_object();
    if(kw_has_key(kw, "result")) {
        json_object_set(kw_compact, "result", kw_get_dict_value(kw, "result", 0, 0));
    }
    if(kw_has_key(kw, "__md_iev__")) {
        json_object_set(kw_compact, "__md_iev__", kw_get_dict_value(kw, "__md_iev__", 0, 0));
    }

    json_t *jn_iev = json_pack("{s:s, s:o}",
        "event", event?event:"???",
        "kw", kw_compact
    );

    log_debug_json(0, jn_iev, prefix);
    json_decref(jn_iev);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC void trace_inter_event2(const char *prefix, const char *event, json_t *kw)
{
    json_t *jn_iev = json_pack("{s:s, s:O}",
        "event", event?event:"???",
        "kw", kw
    );

    log_debug_json(0, jn_iev, prefix);
    json_decref(jn_iev);
}

/***************************************************************************
 *  Get a inter-event from json
 *  Inter-event from world outside, deserialize!
 ***************************************************************************/
PRIVATE int iev_create_from_json(iev_msg_t *iev_msg, json_t *jn_msg)
{
    /*
     *  Get fields
     */
    const char *event = kw_get_str(jn_msg, "event", "", KW_REQUIRED);
    json_t *kw = kw_get_dict(jn_msg, "kw", 0, KW_REQUIRED);

    if(empty_string(event)) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "event EMPTY",
            "process",      "%s", get_process_name(),
            "hostname",     "%s", get_host_name(),
            "pid",          "%d", get_pid(),
            NULL
        );
        JSON_DECREF(jn_msg);
        iev_msg->event[0] = 0;
        iev_msg->kw = 0;
        return -1;
    }
    if(!kw) { // WARNING cannot be null!
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "kw EMPTY",
            "process",      "%s", get_process_name(),
            "hostname",     "%s", get_host_name(),
            "pid",          "%d", get_pid(),
            NULL
        );
        JSON_DECREF(jn_msg);
        iev_msg->event[0] = 0;
        iev_msg->kw = 0;
        return -1;
    }
    /*
     *  Aquí se tendría que tracear el inter-evento de entrada
     */
    /*
     *  Inter-event from world outside, deserialize!
     */
    json_incref(kw);
    json_t *new_kw = kw_deserialize(kw);

    if(strlen(event) >= sizeof(iev_msg->event)) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "Event name TOO LARGE",
            "process",      "%s", get_process_name(),
            "hostname",     "%s", get_host_name(),
            "pid",          "%d", get_pid(),
            "len",          "%d", strlen(event),
            "maxlen",       "%d", sizeof(iev_msg->event)-1,
            NULL
        );
    }
    snprintf(iev_msg->event, sizeof(iev_msg->event), "%s", event);
    iev_msg->kw = new_kw;
    JSON_DECREF(jn_msg);

    return 0;
}

/***************************************************************************
 *  Load intra event from disk
 ***************************************************************************/
PUBLIC int iev_create_from_file(iev_msg_t *iev_msg, const char *path)
{
    /*---------------------------------------*
     *  Load the event from file
     *---------------------------------------*/
    json_error_t error;
    size_t flags = 0;
    json_t *jn_msg = json_load_file(path, flags, &error);
    if(!jn_msg) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_JSON_ERROR,
            "msg",          "%s", "json_load_file() FAILED",
            "process",      "%s", get_process_name(),
            "hostname",     "%s", get_host_name(),
            "pid",          "%d", get_pid(),
            "error",        "%s", error.text,
            "path",         "%s", path,
            NULL
        );
        iev_msg->event[0] = 0;
        iev_msg->kw = 0;
        return -1;
    }
    return iev_create_from_json(iev_msg, jn_msg);
}


/***************************************************************************
 *  Incorporate event's messages from outside world.
 *  gbuf decref
 ***************************************************************************/
PUBLIC int iev_create_from_gbuffer(
    iev_msg_t *iev_msg,
    GBUFFER *gbuf,  // WARNING gbuf own and data consumed
    int verbose     // 1 log, 2 log+dump
)
{
    /*---------------------------------------*
     *  Convert gbuf msg in json
     *---------------------------------------*/
    json_t *jn_msg = gbuf2json(gbuf, verbose); // gbuf stolen: decref and data consumed
    if(!jn_msg) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_JSON_ERROR,
            "msg",          "%s", "gbuf2json() FAILED",
            "process",      "%s", get_process_name(),
            "hostname",     "%s", get_host_name(),
            "pid",          "%d", get_pid(),
            NULL
        );
        iev_msg->event[0] = 0;
        iev_msg->kw = 0;
        return -1;
    }
    return iev_create_from_json(iev_msg, jn_msg);
}

/***************************************************************************
 *  Useful to send event's messages to outside world.
 ***************************************************************************/
PUBLIC GBUFFER *iev2gbuffer(
    json_t *jn_iev,
    BOOL pretty
)
{
    size_t flags = 0; //JSON_SORT_KEYS;
    if(pretty) {
        flags |= JSON_INDENT(4);
    } else {
        flags |= JSON_COMPACT;
    }
    return json2gbuf(0, jn_iev, flags);
}
