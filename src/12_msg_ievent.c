/***********************************************************************
 *          MSG_IEVENT.C
 *
 *          Common of Inter-Events messages
 *
 *          Copyright (c) 2016 Niyamaka.
 *          All Rights Reserved.
***********************************************************************/
#include <sys/types.h>
#if defined(WIN32) || defined(_WINDOWS)
    #include <io.h>
#else
    #include <unistd.h>
#endif

#include "12_msg_ievent.h"

/****************************************************************
 *         Constants
 ****************************************************************/
#define MAX_MESSAGE_AREA_FILTERS 120


/****************************************************************
 *         Structures
 ****************************************************************/
typedef struct {
    const char *field_name;
    answer_filter_fn_t answer_filter_fn;
} message_area_filters_t;


/****************************************************************
 *         Data
 ****************************************************************/
PRIVATE int max_slot = 0;
PRIVATE message_area_filters_t message_area_filters[MAX_MESSAGE_AREA_FILTERS+1];

/***************************************************************
 *              Prototypes
 ***************************************************************/
PRIVATE int ievent_answer_filter(
    hgobj gobj,
    json_t *kw_answer,
    const char *area_key,
    json_t *jn_area_key_value
);

/***************************************************************
 *              Data
 ***************************************************************/

/***************************************************************************
 *
 ***************************************************************************/
// PRIVATE int ievent_answer_filter(
//     hgobj gobj,
//     json_t* kw_answer,
//     const char* area_key,
//     json_t* jn_ievent_gate_stack)
// {
//     PRIVATE_DATA *priv = gobj_priv_data(gobj);
//     json_t * jn_ievent = json_array_get(jn_ievent_gate_stack, 0);
//
//     /*
//      *  Dale la vuelta src->dst dst->src
//      */
//     const char *iev_src_service = kw_get_str(jn_ievent, "src_service", "", 0);
//
//     json_object_set_new(jn_ievent, "dst_yuno", json_string(priv->remote_yuno_name));
//     json_object_set_new(jn_ievent, "dst_role", json_string(priv->remote_yuno_role));
//     json_object_set_new(jn_ievent, "dst_service", json_string(iev_src_service));
//     json_object_set_new(jn_ievent, "src_yuno", json_string(gobj_yuno_name()));
//     json_object_set_new(jn_ievent, "src_role", json_string(gobj_yuno_role()));
//     json_object_set_new(jn_ievent, "src_service", json_string(gobj_name(gobj)));
//
//     return 0;
// }

/***************************************************************************
 *  Key: IEVENT_MESSAGE_AREA_ID "ievent_gate_stack"
 *  Dale la vuelta src->dst dst->src
 ***************************************************************************/
PRIVATE int ievent_answer_filter(
    hgobj gobj,
    json_t* kw_answer,
    const char* area_key,
    json_t* jn_ievent_gate_stack)
{
    // WARNING no puedo usar este priv,
    // es del primer gobj que registra el filtro, es una funcion asquerosa super-global,
    // PRIVATE_DATA *priv = gobj_priv_data(gobj);
    json_t * jn_ievent = json_array_get(jn_ievent_gate_stack, 0);

    /*
     *  Dale la vuelta src->dst dst->src
     */
    const char *iev_src_yuno = kw_get_str(jn_ievent, "src_yuno", "", 0);
    const char *iev_src_role = kw_get_str(jn_ievent, "src_role", "", 0);
    const char *iev_src_service = kw_get_str(jn_ievent, "src_service", "", 0);

    json_object_set_new(jn_ievent, "dst_yuno", json_string(iev_src_yuno));
    json_object_set_new(jn_ievent, "dst_role", json_string(iev_src_role));
    json_object_set_new(jn_ievent, "dst_service", json_string(iev_src_service));
    json_object_set_new(jn_ievent, "src_yuno", json_string(gobj_yuno_name()));
    json_object_set_new(jn_ievent, "src_role", json_string(gobj_yuno_role()));
    json_object_set_new(jn_ievent, "src_service", json_string(gobj_name(gobj)));

    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int register_ievent_answer_filter(void)
{
    msg_iev_add_answer_filter(IEVENT_MESSAGE_AREA_ID, ievent_answer_filter);
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int msg_iev_add_answer_filter(
    const char* field_name,
    answer_filter_fn_t answer_filter_fn)
{
    message_area_filters_t *pf = message_area_filters;
    for(int i=0; i<max_slot; i++, pf++) {
        if(strcasecmp(pf->field_name, field_name)==0) {
            // already registered
            return 0;
        }
    }

    if(max_slot >= MAX_MESSAGE_AREA_FILTERS) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "table answer filters FULL",
            NULL
        );
        return -1;
    }
    pf = message_area_filters + max_slot;
    pf->field_name = field_name;
    pf->answer_filter_fn = answer_filter_fn;
    max_slot++;
    return 0;
}

/***************************************************************************
 *  Apply answer filters
 ***************************************************************************/
PRIVATE int msg_apply_answer_filters(void *user_data, json_t *__request_msg_area__, json_t *kw_answer)
{
    message_area_filters_t * pf = message_area_filters;
    while(pf->field_name) {
        if(kw_has_key(__request_msg_area__, pf->field_name)) {
            /*
             *  Filter
             */
            if(pf->answer_filter_fn) {
                json_t *jn_value = kw_get_dict_value(__request_msg_area__, pf->field_name, 0, 0);
                (pf->answer_filter_fn)(user_data, kw_answer, pf->field_name, jn_value);
            }
        }
        pf++;
    }
    return 0;
}

/***************************************************************************
 *  Return a new kw with all minus this keys:
 ***************************************************************************/
PUBLIC json_t *msg_iev_pure_clone(
    json_t *kw  // NOT owned
)
{
    json_t *kw_clone = kw_duplicate(kw); // not owned
    json_object_del(kw_clone, "__md_iev__");
    json_object_del(kw_clone, "__temp__");
    json_object_del(kw_clone, "__md_tranger__");
    return kw_clone;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *msg_iev_build_webix( // DEPRECATED but son muchas!
    hgobj gobj,
    json_int_t result,
    json_t *jn_comment, // owned
    json_t *jn_schema,  // owned
    json_t *jn_data,    // owned
    json_t *kw_request // owned, used to extract ONLY __md_iev__.
)
{
    json_t *webix = build_webix(result, jn_comment, jn_schema, jn_data);
    json_t *webix_answer = msg_iev_answer(gobj, kw_request, webix, 0);

    return webix_answer;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *msg_iev_build_webix2(
    hgobj gobj,
    json_int_t result,
    json_t *jn_comment, // owned
    json_t *jn_schema,  // owned
    json_t *jn_data,    // owned
    json_t *kw_request, // owned, used to extract ONLY __md_iev__.
    const char *msg_type)
{
    json_t *webix = build_webix(result, jn_comment, jn_schema, jn_data);
    json_t *webix_answer = msg_iev_answer(gobj, kw_request, webix, msg_type);

    return webix_answer;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *msg_iev_build_webix2_without_answer_filter(
    hgobj gobj,
    json_int_t result,
    json_t *jn_comment, // owned
    json_t *jn_schema,  // owned
    json_t *jn_data,    // owned
    json_t *kw_request, // owned, used to extract ONLY __md_iev__.
    const char *msg_type
)
{
    json_t *webix = build_webix(result, jn_comment, jn_schema, jn_data);
    json_t *webix_answer = msg_iev_answer_without_answer_filter(gobj, kw_request, webix, msg_type);

    return webix_answer;
}

/***************************************************************************
 *  server: build the answer of kw
 *  Return a new kw only with the identification message area.
 ***************************************************************************/
PUBLIC json_t *msg_iev_answer(
    hgobj gobj,
    json_t *kw_request,     // owned, kw request, used to extract ONLY __md_iev__.
    json_t *kw_answer,      // like owned, is returned!, created if null, body of answer message.
    const char *msg_type
)
{
    if(!kw_answer) {
        kw_answer = json_object();
    }
    json_t *__md_iev__ = kw_get_dict(kw_request, "__md_iev__", 0, 0);
    if(__md_iev__) {
        json_t *request_msg_area = kw_duplicate(__md_iev__);
        json_object_set_new(kw_answer, "__md_iev__", request_msg_area);
        msg_set_msg_type(kw_answer, msg_type);
        if(!kw_has_key(request_msg_area, "__md_yuno__")) {
            json_t *jn_metadata = json_pack("{s:s, s:s, s:s, s:s, s:I, s:s}",
                "realm_name", gobj_yuno_realm_name(),
                "yuno_role", gobj_yuno_role(),
                "yuno_name", gobj_yuno_name(),
                "yuno_id", gobj_yuno_id(),
                "pid", (json_int_t)getpid(),
                "user", get_user_name()
            );
            json_object_set_new(request_msg_area, "__md_yuno__", jn_metadata);
        }

        msg_apply_answer_filters(gobj, request_msg_area, kw_answer);
    }
    json_object_del(kw_answer, "__temp__");

    KW_DECREF(kw_request);
    return kw_answer;
}

/***************************************************************************
 *  server: build the answer of kw
 *  Return a new kw only with the identification message area.
 ***************************************************************************/
PUBLIC json_t *msg_iev_answer_without_answer_filter(
    hgobj gobj,
    json_t *kw_request,         // kw request, owned
    json_t *kw_answer,          // like owned, is returned!, created if null, body of answer message.
    const char *msg_type
)
{
    if(!kw_answer) {
        kw_answer = json_object();
    }
    json_t *__md_iev__ = kw_get_dict(kw_request, "__md_iev__", 0, 0);
    if(__md_iev__) {
        json_t *request_msg_area = kw_duplicate(__md_iev__);
        json_object_set_new(kw_answer, "__md_iev__", request_msg_area);
        msg_set_msg_type(kw_answer, msg_type);

        if(!kw_has_key(request_msg_area, "__md_yuno__")) {
            json_t *jn_metadata = json_pack("{s:s, s:s, s:s, s:s, s:I, s:s}",
                "realm_name", gobj_yuno_realm_name(),
                "yuno_role", gobj_yuno_role(),
                "yuno_name", gobj_yuno_name(),
                "yuno_id", gobj_yuno_id(),
                "pid", (json_int_t)getpid(),
                "user", get_user_name()
            );
            json_object_set_new(request_msg_area, "__md_yuno__", jn_metadata);
        }
    }
    json_object_del(kw_answer, "__temp__");

    KW_DECREF(kw_request);
    return kw_answer;
}

/***************************************************************************
 *
 *  __your_stack__ is a LIFO queue, ultimo en entrar, primero en salir
 *
 {
    '__md_iev__': {
        '__your_stack__': [
            request-msg_id-last,
            ...
            request-msg_id-first
        ],
        ...
    }
 }

 ***************************************************************************/
PUBLIC int msg_iev_push_stack(
    json_t *kw,             // not owned
    const char *stack,
    json_t *jn_user_info         // owned
)
{
    if(!json_is_object(kw)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "kw is not a dict.",
            NULL
        );
        return -1;
    }
    json_t *jn_stack = kw_get_subdict_value(kw, "__md_iev__", stack, 0, 0);
    if(!jn_stack) {
        jn_stack = json_array();
        kw_set_subdict_value(kw, "__md_iev__", stack, jn_stack);
    }
    return json_array_insert_new(jn_stack, 0, jn_user_info);
}

/***************************************************************************
 *  Get current item from stack, without poping.
 ***************************************************************************/
PUBLIC json_t *msg_iev_get_stack( // return is not yours!
    json_t *kw,
    const char *stack,
    BOOL print_not_found
)
{
    json_t *jn_stack = kw_get_subdict_value(kw, "__md_iev__", stack, 0, 0);
    if(!jn_stack) {
        if(print_not_found) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                "msg",          "%s", "stack NOT EXIST.",
                "stack",        "%s", stack,
                NULL
            );
        }
        return 0;
    }
    json_t *jn_user_info = json_array_get(jn_stack, 0);
    return jn_user_info;
}

/***************************************************************************
 *  Ppo current item from stack, poping
 *  WARNING free the return
 ***************************************************************************/
PUBLIC json_t * msg_iev_pop_stack(
    json_t *kw,
    const char *stack
)
{
    /*-----------------------------------*
     *  Recover the current item
     *-----------------------------------*/
    json_t *jn_stack = kw_get_subdict_value(kw, "__md_iev__", stack, 0, 0);
    if(!jn_stack) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "stack NOT EXIST.",
            "stack",        "%s", stack,
            NULL
        );
        return 0;
    }
    json_t *jn_user_info = json_array_get(jn_stack, 0);
    json_incref(jn_user_info);
    json_array_remove(jn_stack, 0);
    if(json_array_size(jn_stack)==0) {
        kw_delete_subkey(kw, "__md_iev__", stack);
    }
    return jn_user_info;
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE const char *msg_type_list[] = {
    "__command__",
    "__publishing__",
    "__subscribing__",
    "__unsubscribing__",
    "__query__",
    "__response__",
    "__order__",
    "__first_shot__",
    0
};

PUBLIC int msg_set_msg_type(
    json_t *kw,
    const char *msg_type
)
{
    if(!empty_string(msg_type)) {
        if(is_metadata_key(msg_type) && !str_in_list(msg_type_list, msg_type, TRUE)) {
            // HACK If it's a metadata key then only admit our message inter-event msg_type_list
            return kw_delete(kw, "__md_iev__`__msg_type__");
        }
        return kw_set_subdict_value(kw, "__md_iev__", "__msg_type__", json_string(msg_type));
    }
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *msg_get_msg_type(
    json_t *kw
)
{
    return kw_get_str(kw, "__md_iev__`__msg_type__", "", 0);
}
