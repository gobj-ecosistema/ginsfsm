/***********************************************************************
 *              GOBJ.C
 *              Simple Finite State Machine
 *              Copyright (c) 1996-2016 Niyamaka.
 *              All Rights Reserved.
 *
 *  NOTICE: how implement fault tolerance with Yuneta.
 *      - mark the input messages with a unique __msg_reference__ keyword.
 *      - send the message throug several channels
 *              (several yunos over several servers)
 *      - the destination yuno save all the __msg_reference__ of processed messages.
 *              (for example during a hour, day, etc, could be done with a age mark).
 *      - if a processed message arrives, simply remove it.
 *      - Now you can shutdown any of channels without problems.
 *
 *  Ideas sobre seguridad:
 *      1.- listas de acceso (ACL) definidas a nivel de clase (gclass).
 *      2.- permisos en metodos de objecto (internal method)
 *      3.- permisos en eventos de entrada y salida.
 *
 *  El método 2 lo usarán los clientes "http".
 *  El método 3 lo usarán los clientes "ws".
 *
 *  A los clientes "http" además de poder recorrer los gobj y ejectuar sus métodos,
 *  les podemos dar a nivel de root (/) o de un gobj especializado (/api)
 *  a las funciones de envio de eventos. Incluso a las funciones de subcripción,
 *  que se pueden implementar con SourceEvent.
 *
 *  Estas ideas nacen una puta noche sin dormir, que parece que los duendes
 *  tienen prisa por terminar, y me tienen aquí exprimiéndome.
 *
 ***********************************************************************/
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <stdint.h>
#include <regex.h>
#include <limits.h>
#include <ctype.h>
#include <string.h>
#if defined(WIN32) || defined(_WINDOWS)
	#include <io.h>
#else
	#include <unistd.h>
	#include <strings.h>
	#include <sys/utsname.h>
#endif
#include "10_gobj.h"
#include "12_msg_ievent.h"

#include "treedb_schema_gobjs.c"

/****************************************************************
 *         Constants
 ****************************************************************/
typedef enum { // WARNING add new values to opt2json()
    obflag_destroying       = 0x0001,
    obflag_destroyed        = 0x0002,
    obflag_created          = 0x0004,
    obflag_unique_name      = 0x0008,
    obflag_autoplay         = 0x0010,
    obflag_autostart        = 0x0020,
    obflag_imminent_destroy = 0x0040,
    obflag_yuno             = 0x1000,
    obflag_default_service  = 0x2000,
    obflag_service          = 0x4000,
    obflag_volatil          = 0x8000,
} obflag_t;

/*
 *  Monitor enums.
 */
typedef enum { // Used by monitor_gobj
    MTOR_GOBJ_YUNO_CREATED = 1,
    MTOR_GOBJ_DEFAULT_SERVICE_CREATED,
    MTOR_GOBJ_SERVICE_CREATED,
    MTOR_GOBJ_UNIQUE_CREATED,
    MTOR_GOBJ_CREATED,
    MTOR_GOBJ_DESTROYED,
    MTOR_GOBJ_START,
    MTOR_GOBJ_STOP,
    MTOR_GOBJ_PLAY,
    MTOR_GOBJ_PAUSE,
} monitor_gobj_t;

typedef enum { // Used by monitor_event
    MTOR_EVENT_ACCEPTED = 1,
    MTOR_EVENT_REFUSED,
} monitor_event_t;

#define MAX_GOBJ_NAME 48

/****************************************************************
 *         Structures
 ****************************************************************/
typedef struct _gclass_register_t {
    DL_ITEM_FIELDS

    GCLASS *gclass;
    const char *role;       // cannot be NULL for yuno's gclass
    int to_free;            // marca los gclass que hay que liberar (el del yuno, que es inherited)
} gclass_register_t;

typedef struct _service_register_t {
    DL_ITEM_FIELDS

    const char *service;
    struct _GObj_t *gobj;
} service_register_t;

typedef struct _trans_filter_t {
    DL_ITEM_FIELDS

    const char *name;
    json_t * (*transformation_fn)(json_t *);
} trans_filter_t;

typedef struct {
    /*
     *  There may be millions of this structure. Please keep straw free.
     */

    /*
     *  Input parameters
     */
    const FSM *fsm;
    void *self;     // hgobj

    /*
     * Internal
     */
    int8_t current_state;
    int8_t last_state;
} SMachine_t;

/*
 *
 */
PRIVATE sdata_desc_t subscription_desc[] = {
/*-ATTR-type------------name----------------flag----------------default---------description---------- */
SDATA (ASN_POINTER,     "publisher",        0,                  0,              "publisher gobj"),
SDATA (ASN_POINTER,     "subscriber",       0,                  0,              "subscriber gobj"),
SDATA (ASN_OCTET_STR,   "event",            0,                  0,              "event name subscribed"),
SDATA (ASN_OCTET_STR,   "renamed_event",    0,                  0,              "rename event name"),
SDATA (ASN_UNSIGNED64,  "subs_flag",        0,                  0,              "subscripton flag"),
SDATA (ASN_JSON,        "__config__",       0,                  0,              "subscription config kw"),
SDATA (ASN_JSON,        "__global__",       0,                  0,              "global event kw"),
SDATA (ASN_JSON,        "__local__",        0,                  0,              "local event kw"),
SDATA (ASN_JSON,        "__filter__",       0,                  0,              "filter event kw"),
SDATA (ASN_OCTET_STR,   "__service__",      0,                  0,              "subscription service"),
SDATA_END()
};

typedef struct _GObj_t {
    /*
     *  There may be millions of this structure. Please keep straw free.
     */

    /*
     *  RC_RESOURCE_HEADER
     */
    struct _GObj_t *__parent__;
    dl_list_t dl_instances;
    dl_list_t dl_childs;
    size_t refcount;

    /*
     *  Input parameters
     */
    char name[MAX_GOBJ_NAME+1];
    GCLASS *gclass;

    /*
     * Internal
     */
    obflag_t obflag;
    SMachine_t * mach;

    dl_list_t dl_subscriptions; // external subscriptions to events of this gobj.
    dl_list_t dl_subscribings;  // subscriptions of this gobj to events of others gobj.

    json_t *jn_user_data;
    json_t *jn_stats;
    void *priv;
    hsdata hsdata_attr;
    hgobj yuno;             // yuno belongs this gobj
    char running;           // set by gobj_start/gobj_stop
    char playing;           // set by gobj_play/gobj_pause
    char disabled;          // set by gobj_enable/gobj_disable
    hgobj bottom_gobj;
    const char *full_name;
    const char *short_name;
    const char *escaped_short_name;
    const char *error_message;
    const char *poid;
    size_t oid;
    char oid_changed;
    uint32_t __gobj_trace_level__;
    uint32_t __gobj_no_trace_level__;
} GObj_t;


/****************************************************************
 *         Data
 ****************************************************************/
#ifdef __linux__
    PRIVATE struct utsname sys;
#endif

PRIVATE json_t *jn_treedb_schema_gobjs = 0;
PRIVATE volatile int  __shutdowning__ = 0;
PRIVATE volatile BOOL __yuno_must_die__ = FALSE;
PRIVATE int  __exit_code__ = 0;
PRIVATE json_t * (*__global_command_parser_fn__)(
    hgobj gobj,
    const char *command,
    json_t *kw,
    hgobj src
) = 0;
PRIVATE json_t * (*__global_stats_parser_fn__)(
    hgobj gobj,
    const char *stats,
    json_t *kw,
    hgobj src
) = 0;
PRIVATE authz_checker_fn __global_authz_checker_fn__ = 0;
PRIVATE authenticate_parser_fn __global_authenticate_parser_fn__ = 0;

PRIVATE int (*__audit_command_cb__)(
    const char *audit_command,
    json_t *kw,
    void *user_data
) = 0;
PRIVATE void *__audit_command_user_data__ = 0;
PRIVATE GObj_t * __yuno__ = 0;
PRIVATE GObj_t * __default_service__ = 0;

PRIVATE char __node_owner__[NAME_MAX];
PRIVATE char __realm_id__[NAME_MAX];
PRIVATE char __realm_owner__[NAME_MAX];
PRIVATE char __realm_role__[NAME_MAX];
PRIVATE char __realm_name__[NAME_MAX];
PRIVATE char __realm_env__[NAME_MAX];
PRIVATE char __yuno_role__[NAME_MAX];
PRIVATE char __yuno_id__[NAME_MAX];
PRIVATE char __yuno_name__[NAME_MAX];
PRIVATE char __yuno_tag__[NAME_MAX];
PRIVATE char __yuno_role_plus_name__[NAME_MAX];
PRIVATE char __yuno_role_plus_id__[NAME_MAX];


PRIVATE json_t *__jn_global_settings__ = 0;
PRIVATE void *(*__global_startup_persistent_attrs_fn__)(void) = 0;
PRIVATE void (*__global_end_persistent_attrs_fn__)(void) = 0;
PRIVATE int (*__global_load_persistent_attrs_fn__)(hgobj gobj, json_t *attrs) = 0;
PRIVATE int (*__global_save_persistent_attrs_fn__)(hgobj gobj, json_t *attrs) = 0;
PRIVATE int (*__global_remove_persistent_attrs_fn__)(hgobj gobj, json_t *attrs) = 0;
PRIVATE json_t * (*__global_list_persistent_attrs_fn__)(hgobj gobj, json_t *attrs) = 0;

PRIVATE char __initialized__ = 0;
PRIVATE int atexit_registered = 0; /* Register atexit just 1 time. */

PRIVATE int  __inside__ = 0;  // it's a counter
PRIVATE json_t *__jn_unique_named_dict__ = 0;  // Dict unique_name:(json_int_t)(size_t)gobj

PRIVATE dl_list_t dl_gclass = {0};
PRIVATE dl_list_t dl_service = {0};
PRIVATE dl_list_t dl_trans_filter = {0};

PRIVATE kw_match_fn __publish_event_match__ = kw_match_simple;

/*
 *  Global trace levels
 */
PRIVATE const trace_level_t s_global_trace_level[16] = {
{"machine",         "Trace machine"},
{"create_delete",   "Trace create/delete of gobjs"},
{"create_delete2",  "Trace create/delete of gobjs level 2: with kw"},
{"subscriptions",   "Trace subscriptions of gobjs"},
{"start_stop",      "Trace start/stop of gobjs"},
{"monitor",         "Monitor activity of gobjs"},
{"event_monitor",   "Monitor events of gobjs"},
{"libuv",           "Trace libuv mixins"},
{"ev_kw",           "Trace event keywords"},
{"authzs",          "Trace authorizations"},
{"subscriptions2",  "Trace subscriptions of gobjs with every send event"},
{"states",          "Trace change of states"},
{0, 0},
};
#define __trace_gobj_create_delete__(gobj)  (gobj_trace_level(gobj) & TRACE_CREATE_DELETE)
#define __trace_gobj_create_delete2__(gobj) (gobj_trace_level(gobj) & TRACE_CREATE_DELETE2)
#define __trace_gobj_subscriptions__(gobj)  (gobj_trace_level(gobj) & TRACE_SUBSCRIPTIONS)
#define __trace_gobj_start_stop__(gobj)     (gobj_trace_level(gobj) & TRACE_START_STOP)
#define __trace_gobj_oids__(gobj)           (gobj_trace_level(gobj) & TRACE_OIDS)
#define __trace_gobj_uv__(gobj)             (gobj_trace_level(gobj) & TRACE_UV)
#define __trace_gobj_ev_kw__(gobj)          (gobj_trace_level(gobj) & TRACE_EV_KW)
#define __trace_gobj_authzs__(gobj)         (gobj_trace_level(gobj) & TRACE_AUTHZS)
#define __trace_gobj_subscriptions2__(gobj) (gobj_trace_level(gobj) & TRACE_SUBSCRIPTIONS2)
#define __trace_gobj_states__(gobj)         (gobj_trace_level(gobj) & TRACE_STATES)

//#define __trace_gobj_monitor__(gobj)        (gobj_trace_level(gobj) & TRACE_MONITOR)
//#define __trace_gobj_event_monitor__(gobj)  (gobj_trace_level(gobj) & TRACE_EVENT_MONITOR)
#define __trace_gobj_monitor__(gobj)        (0)
#define __trace_gobj_event_monitor__(gobj)  (0)

PRIVATE uint32_t __global_trace_level__ = 0;
PRIVATE volatile uint32_t __panic_trace__ = 0;
PRIVATE uint32_t __deep_trace__ = 0;

/*
 *  Strings of enum gcflag_t gcflag
 */
typedef struct {
    const char *name;
    const char *description;
} s_gcflag_t;

PRIVATE const s_gcflag_t s_gcflag[] = {
{"manual_start",            "gobj_start_tree() don't start gobjs of this /gclass"},
{"no_check_ouput_events",   "When publishing don't check events in output_event_list"},
{"ignore_unkwnow_attrs",    "When creating a gobj, ignore not existing attrs"},
{"required_start_to_play",  "Require start before play"},
{0, 0},
};

/*
 *  Strings of enum event_flag_t flag
 */
typedef struct {
    const char *name;
    const char *description;
} event_flag_names_t;

PRIVATE const event_flag_names_t event_flag_names[] = {
{"EVF_KW_WRITING",      "kw is not owned by functions, the changes in kw remains through sent events"},
{"EVF_PUBLIC_EVENT",    "Public event, can be executed from outside of yuno"},
{"EVF_NO_WARN_SUBS",    "Don't warning about publication without subscribers"},
{0, 0}
};

/*
 *  System (gobj) output events
 */
PRIVATE const EVENT global_output_events[] = {
    {__EV_STATE_CHANGED__,  EVF_SYSTEM_EVENT|EVF_NO_WARN_SUBS,  0,  "gobj's state has changed"},
    {NULL, 0, 0, ""}
};

/*---------------------------------------------*
 *      Global authz levels
 *---------------------------------------------*/
PRIVATE sdata_desc_t pm_read_attr[] = {
/*-PM----type-----------name------------flag------------default-----description---------- */
SDATAPM (ASN_OCTET_STR, "path",         0,              0,          "Attribute path"),
SDATA_END()
};
PRIVATE sdata_desc_t pm_write_attr[] = {
/*-PM----type-----------name------------flag------------default-----description---------- */
SDATAPM (ASN_OCTET_STR, "path",         0,              0,          "Attribute path"),
SDATA_END()
};
PRIVATE sdata_desc_t pm_exec_cmd[] = {
/*-PM----type-----------name------------flag------------default-----description---------- */
SDATAPM (ASN_OCTET_STR, "command",      0,              0,          "Command name"),
SDATAPM (ASN_JSON,      "kw",           0,              0,          "command's kw"),
SDATA_END()
};
PRIVATE sdata_desc_t pm_inject_event[] = {
/*-PM----type-----------name------------flag------------default-----description---------- */
SDATAPM (ASN_OCTET_STR, "event",        0,              0,          "Event name"),
SDATAPM (ASN_JSON,      "kw",           0,              0,          "event's kw"),
SDATA_END()
};
PRIVATE sdata_desc_t pm_subs_event[] = {
/*-PM----type-----------name------------flag------------default-----description---------- */
SDATAPM (ASN_OCTET_STR, "event",        0,              0,          "Event name"),
SDATAPM (ASN_JSON,      "kw",           0,              0,          "event's kw"),
SDATA_END()
};
PRIVATE sdata_desc_t pm_read_stats[] = {
/*-PM----type-----------name------------flag------------default-----description---------- */
SDATAPM (ASN_OCTET_STR, "stats",        0,              0,          "Stats name"),
SDATAPM (ASN_JSON,      "kw",           0,              0,          "stats's kw"),
SDATA_END()
};
PRIVATE sdata_desc_t pm_reset_stats[] = {
/*-PM----type-----------name------------flag------------default-----description---------- */
SDATAPM (ASN_OCTET_STR, "stats",        0,              0,          "Stats name"),
SDATAPM (ASN_JSON,      "kw",           0,              0,          "stats's kw"),
SDATA_END()
};

PRIVATE sdata_desc_t global_authz_table[] = {
/*-AUTHZ-- type---------name--------------------flag----alias---items---description--*/
SDATAAUTHZ (ASN_SCHEMA, "__read_attribute__",   0,      0,      pm_read_attr, "Authorization to read gobj's attributes"),
SDATAAUTHZ (ASN_SCHEMA, "__write_attribute__",  0,      0,      pm_write_attr, "Authorization to write gobj's attributes"),
SDATAAUTHZ (ASN_SCHEMA, "__execute_command__",  0,      0,      pm_exec_cmd, "Authorization to execute gobj's commands"),
SDATAAUTHZ (ASN_SCHEMA, "__inject_event__",     0,      0,      pm_inject_event, "Authorization to inject events to gobj"),
SDATAAUTHZ (ASN_SCHEMA, "__subscribe_event__",  0,      0,      pm_subs_event, "Authorization to subscribe events of gobj"),
SDATAAUTHZ (ASN_SCHEMA, "__read_stats__",       0,      0,      pm_read_stats, "Authorization to read gobj's stats"),
SDATAAUTHZ (ASN_SCHEMA, "__reset_stats__",      0,      0,      pm_reset_stats, "Authorization to reset gobj's stats"),
SDATA_END()
};

/*
 *  Strings of enum event_authz_t auth
 */
PRIVATE const trace_level_t event_authz_names[] = {
{"AUTHZ_INJECT",        "Event needs '__inject_event__' authorization to be injected to machine"},
{"AUTHZ_SUBSCRIBE",     "Event needs '__subscribe_event__' authorization to be subscribed"},
{"AUTHZ_CREATE",        "Event needs 'create' authorization"},
{"AUTHZ_READ",          "Event needs 'read' authorization"},
{"AUTHZ_UPDATE",        "Event needs 'update' authorization"},
{"AUTHZ_DELETE",        "Event needs 'delete' authorization"},
{"AUTHZ_LINK",          "Event needs 'link' authorization"},
{"AUTHZ_UNLINK",        "Event needs 'unlink' authorization"},
{0, 0}
};

PRIVATE json_t *__2key__ = 0;

/****************************************************************
 *         Prototypes
 ****************************************************************/
PRIVATE BOOL _change_state(GObj_t * gobj, const char *new_state);
PRIVATE void free_gclass_reg(gclass_register_t *gclass_reg);
PRIVATE void free_service_reg(service_register_t *srv_reg);
PRIVATE void free_trans_filter(trans_filter_t *trans_reg);
PRIVATE void gobj_free(hgobj gobj);
PRIVATE int register_unique_gobj(GObj_t * gobj);
PRIVATE int deregister_unique_gobj(GObj_t * gobj);
PRIVATE int register_service(const char *name, hgobj gobj);
PRIVATE int register_transformation_filter(const char *name, json_t * (trans_filter)(json_t *));
PRIVATE json_t *webix_trans_filter(json_t *kw);

PRIVATE hgobj _create_yuno(
    const char *name,
    GCLASS *gclass,
    json_t *kw
);
PRIVATE service_register_t * _find_service(const char *service);

PRIVATE char *tab(char *bf, int bflen);
PRIVATE inline BOOL is_machine_tracing(GObj_t * gobj);
PRIVATE inline BOOL is_machine_not_tracing(GObj_t * gobj);

PRIVATE int gobj_write_json_parameters(
    GObj_t * gobj,
    json_t *kw,     // not own
    json_t *jn_global  // not own
);
PRIVATE SMachine_t * smachine_create(const FSM *fsm, void *self);
PRIVATE int smachine_destroy(SMachine_t * mach);
PRIVATE int smachine_check(GCLASS *gclass);
PRIVATE int on_post_write_it_cb(void *user_data, const char *name);
PRIVATE SData_Value_t on_post_read_it_cb(void *user_data, const char *name, int type, SData_Value_t data);
PRIVATE int on_post_write_stats_it_cb(
    void *user_data,
    const char *name,
    int type,
    SData_Value_t old_v,
    SData_Value_t new_v
);
PRIVATE void monitor_gobj(
    monitor_gobj_t monitor_gobj,
    GObj_t *gobj
);
PRIVATE void monitor_gobj(
    monitor_gobj_t monitor_gobj,
    GObj_t *gobj
);
PRIVATE void monitor_event(
    monitor_event_t monitor_event,
    const char *event,
    GObj_t *src,
    GObj_t *dst
);
PRIVATE void monitor_state(
    GObj_t *gobj
);

PRIVATE int _delete_subscriptions(GObj_t * publisher);
PRIVATE int _delete_subscribings(GObj_t * subscriber);

PRIVATE int print_attr_not_found(void *user_data, const char *attr)
{
    hgobj gobj = user_data;
    if(strcasecmp(attr, "__json_config_variables__")!=0) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "GClass Attribute NOT FOUND",
            "gclass",       "%s", gobj_gclass_name(gobj),
            "attr",         "%s", attr,
            NULL
        );
    }
    return 0;
}
PRIVATE int print_required_attr_not_found(void *user_data, const char *attr)
{
    hgobj gobj = user_data;
    log_error(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "Required Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", attr,
        NULL
    );
    return 0;
}
PRIVATE json_t *extract_all_mine(
    const char *gclass_name,
    const char *gobj_name,
    json_t *kw     // not own
);
PRIVATE json_t *extract_json_config_variables_mine(
    const char *gclass_name,
    const char *gobj_name,
    json_t *kw     // not own
);
PRIVATE int _set_gobj_trace_level(GObj_t * gobj, const char *level, BOOL set);

PRIVATE json_t *bit2level(
    const trace_level_t *internal_trace_level,
    const trace_level_t *user_trace_level,
    uint32_t bit
);
PRIVATE uint32_t level2bit(
    const trace_level_t *internal_trace_level,
    const trace_level_t *user_trace_level,
    const char *level
);




                    /*---------------------------------*
                     *  SECTION: Start up functions
                     *---------------------------------*/




/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_start_up(
    json_t *jn_global_settings,
    void * (*startup_persistent_attrs)(void),
    void (*end_persistent_attrs)(void),
    int (*load_persistent_attrs)(hgobj gobj, json_t *attrs),
    int (*save_persistent_attrs)(hgobj gobj, json_t *attrs),
    int (*remove_persistent_attrs)(hgobj gobj, json_t *attrs),
    json_t * (*list_persistent_attrs)(hgobj gobj, json_t *attrs),
    json_function_t global_command_parser,
    json_function_t global_stats_parser,
    authz_checker_fn global_authz_checker,
    authenticate_parser_fn global_authenticate_parser
)
{
    if(__initialized__) {
        return -1;
    }
    if (!atexit_registered) {
        atexit(gobj_shutdown);
        atexit_registered = 1;
    }
    __shutdowning__ = 0;
    __jn_global_settings__ =  kw_duplicate(jn_global_settings);

    __global_startup_persistent_attrs_fn__ = startup_persistent_attrs;
    __global_end_persistent_attrs_fn__ = end_persistent_attrs;
    __global_load_persistent_attrs_fn__ = load_persistent_attrs;
    __global_save_persistent_attrs_fn__ = save_persistent_attrs;
    __global_remove_persistent_attrs_fn__ = remove_persistent_attrs;
    __global_list_persistent_attrs_fn__ = list_persistent_attrs;
    __global_command_parser_fn__ = global_command_parser;
    __global_stats_parser_fn__ = global_stats_parser;
    __global_authz_checker_fn__ = global_authz_checker;
    __global_authenticate_parser_fn__ = global_authenticate_parser;

    if(__global_startup_persistent_attrs_fn__) {
        __global_startup_persistent_attrs_fn__();
    }

    dl_init(&dl_gclass);
    dl_init(&dl_service);
    dl_init(&dl_trans_filter);

    gobj_add_publication_transformation_filter_fn("webix", webix_trans_filter);

    helper_quote2doublequote(treedb_schema_gobjs);

    /*
     *  Chequea schema treedb, exit si falla.
     */
    jn_treedb_schema_gobjs = legalstring2json(treedb_schema_gobjs, TRUE);
    if(!jn_treedb_schema_gobjs) {
        exit(-1);
    }

    __2key__ = json_object();

#ifdef __linux__
    if(uname(&sys)==0) {
        change_char(sys.machine, '_', '-');
    }
#endif

    __initialized__ = TRUE;
    return 0;

}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC void gobj_shutdown(void)
{
    if(__shutdowning__) {
        return;
    }
    __shutdowning__ = 1;

    if(__yuno__ && !(__yuno__->obflag & obflag_destroying)) {
        if(gobj_is_playing(__yuno__)) {
            gobj_pause(__yuno__);
        }
        if(gobj_is_running(__yuno__)) {
            gobj_stop(__yuno__);
        }
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC void gobj_end(void)
{
    if(!__initialized__) {
        return;
    }
    __initialized__ = FALSE;

    if(__yuno__) {
        gobj_destroy(__yuno__);
        __yuno__ = 0;
    }
    if(__jn_global_settings__) {
        json_decref(__jn_global_settings__);
        __jn_global_settings__ = 0;
    }
    gclass_register_t *gclass_reg;
    while((gclass_reg=dl_first(&dl_gclass))) {
        free_gclass_reg(gclass_reg);
    }

    service_register_t *srv_reg;
    while((srv_reg=dl_first(&dl_service))) {
        free_service_reg(srv_reg);
    }

    trans_filter_t *trans_reg;
    while((trans_reg=dl_first(&dl_trans_filter))) {
        free_trans_filter(trans_reg);
    }
    JSON_DECREF(jn_treedb_schema_gobjs);
    JSON_DECREF(__2key__);

    if(__global_end_persistent_attrs_fn__) {
        __global_end_persistent_attrs_fn__();
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC BOOL gobj_is_shutdowning(void)
{
    return __shutdowning__;
}




                    /*---------------------------------*
                     *  SECTION: Start up functions
                     *---------------------------------*/




/***************************************************************************
 *  Register yuno's gclass
 ***************************************************************************/
PUBLIC int gobj_register_yuno(
    const char *yuno_role,
    GCLASS *gclass,
    BOOL to_free)
{
    if(!__initialized__) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "gobj system not initialized",
            NULL
        );
        return -1;
    }
    gclass_register_t *gclass_reg = gbmem_malloc(sizeof(gclass_register_t));
    if(!gclass_reg) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "no memory for sizeof(gclass_register_t)",
            "role",         "%s", yuno_role,
            NULL
        );
        return -1;
    }
    gclass_reg->role = gbmem_strdup(yuno_role);
    gclass_reg->gclass = gclass;
    gclass_reg->to_free = to_free;

    dl_add(&dl_gclass, gclass_reg);

    return 0;
}

/***************************************************************************
 *  Unregister
 ***************************************************************************/
PRIVATE void free_gclass_reg(gclass_register_t *gclass_reg)
{
    dl_delete(&dl_gclass, gclass_reg, 0);
    if(gclass_reg->to_free) {
        GBMEM_FREE(gclass_reg->gclass);
    }
    GBMEM_FREE(gclass_reg->role);
    GBMEM_FREE(gclass_reg);
}

/***************************************************************************
 *  Unregister
 ***************************************************************************/
PRIVATE void free_service_reg(service_register_t *srv_reg)
{
    dl_delete(&dl_service, srv_reg, 0);
    GBMEM_FREE(srv_reg->service);
    GBMEM_FREE(srv_reg);
}

/***************************************************************************
 *  Unregister
 ***************************************************************************/
PRIVATE void free_trans_filter(trans_filter_t *trans_reg)
{
    dl_delete(&dl_trans_filter, trans_reg, 0);
    GBMEM_FREE(trans_reg->name);
    GBMEM_FREE(trans_reg);
}

/***************************************************************************
 *  Factory to create yuno gobj
 ***************************************************************************/
PUBLIC hgobj gobj_yuno_factory(
    const char *node_owner,
    const char *realm_id,
    const char *realm_owner,
    const char *realm_role,
    const char *realm_name,
    const char *realm_env,
    const char *yuno_id,
    const char *yuno_name,
    const char *yuno_tag,
    json_t *jn_yuno_settings) // own
{
    if(__yuno__) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "Yuno gobj ALREADY CREATED",
            NULL
        );
        JSON_DECREF(jn_yuno_settings);
        return 0;
    }

    /*
     *  Search the first gclass with role
     */
    gclass_register_t *gclass_reg = dl_first(&dl_gclass);
    while(gclass_reg) {
        if(!empty_string(gclass_reg->role)) {
            log_debug(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_STARTUP,
                "msg",          "%s", "Create yuno",
                "realm_owner",  "%s", realm_owner,
                "realm_id",     "%s", realm_id,
                "yuno_role",    "%s", gclass_reg->role,
                "yuno_id",      "%s", yuno_id,
                "yuno_name",    "%s", yuno_name,
                "yuno_tag",     "%s", yuno_tag,
                "gclass",       "%s", gclass_reg->gclass->gclass_name,
                NULL
            );
            snprintf(
                __node_owner__, sizeof(__node_owner__),
                "%s", node_owner?node_owner:""
            );
            snprintf(
                __realm_id__, sizeof(__realm_id__),
                "%s", realm_id?realm_id:""
            );
            snprintf(
                __realm_owner__, sizeof(__realm_owner__),
                "%s", realm_owner?realm_owner:""
            );
            snprintf(
                __realm_role__, sizeof(__realm_role__),
                "%s", realm_role?realm_role:""
            );
            snprintf(
                __realm_name__, sizeof(__realm_name__),
                "%s", realm_name?realm_name:""
            );
            snprintf(
                __realm_env__, sizeof(__realm_env__),
                "%s", realm_env?realm_env:""
            );
            snprintf(
                __yuno_role__, sizeof(__yuno_role__), "%s", gclass_reg->role
            );
            snprintf(
                __yuno_id__, sizeof(__yuno_id__),
                "%s", yuno_id?yuno_id:""
            );
            if(empty_string(yuno_name)) {
                snprintf(__yuno_role_plus_name__, sizeof(__yuno_role_plus_name__),
                    "%s",
                    gclass_reg->role
                );
                __yuno_name__[0] = 0;
            } else {
                snprintf(__yuno_role_plus_name__, sizeof(__yuno_role_plus_name__),
                    "%s^%s",
                    gclass_reg->role,
                    yuno_name
                );
                snprintf(__yuno_name__, sizeof(__yuno_name__), "%s", yuno_name);
            }
            if(empty_string(yuno_id)) {
                snprintf(__yuno_role_plus_id__, sizeof(__yuno_role_plus_id__),
                    "%s",
                    gclass_reg->role
                );
                __yuno_id__[0] = 0;
            } else {
                snprintf(__yuno_role_plus_id__, sizeof(__yuno_role_plus_id__),
                    "%s^%s",
                    gclass_reg->role,
                    yuno_id
                );
                snprintf(__yuno_id__, sizeof(__yuno_id__), "%s", yuno_id);
            }
            snprintf(
                __yuno_tag__, sizeof(__yuno_tag__),
                "%s", yuno_tag?yuno_tag:""
            );

            hgobj gobj = _create_yuno( // it's saved in __yuno__ too
                yuno_name,
                gclass_reg->gclass,
                jn_yuno_settings
            );
            if(__yuno__) {
                register_service("__yuno__", gobj);
#ifdef __linux__
                gobj_write_uint32_attr(__yuno__, "watcher_pid", get_watcher_pid());
#else
                gobj_write_uint32_attr(__yuno__, "watcher_pid", 0);
#endif
            }
            return __yuno__;
        }
        gclass_reg = dl_next(gclass_reg);
    }

    log_error(0,
        "gobj",         "%s", __FILE__,
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "Yuno GCLASS NOT FOUND",
        NULL
    );
    JSON_DECREF(jn_yuno_settings);
    return 0;
}

/***************************************************************************
 *  Register ordinary's gclass
 ***************************************************************************/
PUBLIC int gobj_register_gclass(GCLASS *gclass)
{
    if(!__initialized__) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "gobj system not initialized",
            NULL
        );
        return -1;
    }

    gclass_register_t *gclass_reg = dl_first(&dl_gclass);
    while(gclass_reg) {
        if(gclass_reg->gclass == gclass) {
            // Already registered
            return 0;
        }
        gclass_reg = dl_next(gclass_reg);
    }

    gclass_reg = gbmem_malloc(sizeof(gclass_register_t));
    if(!gclass_reg) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "no memory for sizeof(gclass_register_t)",
            "gclass",       "%s", gclass->gclass_name,
            NULL
        );
        return -1;
    }
    gclass_reg->gclass = gclass;
    dl_insert(&dl_gclass, gclass_reg);

    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC GCLASS * gobj_find_gclass(const char *gclass_name, BOOL verbose)
{
    if(empty_string(gclass_name)) {
        return 0;
    }

    gclass_register_t *gclass_reg = dl_first(&dl_gclass);
    while(gclass_reg) {
        if(strcasecmp(gclass_reg->gclass->gclass_name, gclass_name)==0) {
            return gclass_reg->gclass;
        }
        gclass_reg = dl_next(gclass_reg);
    }
    if(verbose) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gclass NOT FOUND",
            "gclass",       "%s", gclass_name,
            NULL
        );
    }
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_walk_gclass_list(
    int (*cb_walking)(GCLASS *gclass, void *user_data),
    void *user_data)
{
    gclass_register_t *gclass_reg = dl_first(&dl_gclass);
    while(gclass_reg) {
        int ret = cb_walking(gclass_reg->gclass, user_data);
        if(ret < 0) {
            return ret;
        }
        gclass_reg = dl_next(gclass_reg);
    }
    return 0;
}

/***************************************************************************
 *  Debug print opt in json
 ***************************************************************************/
PRIVATE json_t * opt2json(GObj_t *gobj)
{
    int len;
    char temp[1024];

    temp[0] = 0;

    if(gobj->obflag & obflag_unique_name) {
        len = strlen(temp);
        if(sizeof(temp) > len) {
            snprintf(temp+len, sizeof(temp) - len, "%s", "Unique-name ");
        }
    }
    if(gobj->obflag & obflag_autoplay) {
        len = strlen(temp);
        if(sizeof(temp) > len) {
            snprintf(temp+len, sizeof(temp) - len, "%s", "Autoplay ");
        }
    }
    if(gobj->obflag & obflag_autostart) {
        len = strlen(temp);
        if(sizeof(temp) > len) {
            snprintf(temp+len, sizeof(temp) - len, "%s", "Autostart ");
        }
    }
    if(gobj->obflag & obflag_yuno) {
        len = strlen(temp);
        if(sizeof(temp) > len) {
            snprintf(temp+len, sizeof(temp) - len, "%s", "Yuno ");
        }
    }
    if(gobj->obflag & obflag_default_service) {
        len = strlen(temp);
        if(sizeof(temp) > len) {
            snprintf(temp+len, sizeof(temp) - len, "%s", "Default-service ");
        }
    }
    if(gobj->obflag & obflag_service) {
        len = strlen(temp);
        if(sizeof(temp) > len) {
            snprintf(temp+len, sizeof(temp) - len, "%s", "Service ");
        }
    }
    if(gobj->obflag & obflag_volatil) {
        len = strlen(temp);
        if(sizeof(temp) > len) {
            snprintf(temp+len, sizeof(temp) - len, "%s", "Volatil ");
        }
    }

    left_justify(temp);
    return json_string(temp);
}

/***************************************************************************
 *  Debug print gclass register in json
 ***************************************************************************/
PUBLIC json_t * gobj_repr_gclass_register(void)
{
    json_t *jn_register = json_array();

    gclass_register_t *gclass_reg = dl_first(&dl_gclass);
    while(gclass_reg) {
        if(gclass_reg->gclass) {
             json_t *jn_gclass = json_object();
             json_object_set_new(
                 jn_gclass,
                 "gclass",
                 json_string(gclass_reg->gclass->gclass_name)
             );
             if(gclass_reg->role) {
                json_object_set_new(
                    jn_gclass,
                    "role",
                    json_string(gclass_reg->role)
                );
             }

            json_array_append_new(jn_register, jn_gclass);
        }

        gclass_reg = dl_next(gclass_reg);
    }

    return jn_register;
}

/***************************************************************************
 *  Debug print service register in json
 ***************************************************************************/
PUBLIC json_t * gobj_repr_service_register(const char *gclass_name)
{
    json_t *jn_register = json_array();

    service_register_t *srv_reg = dl_first(&dl_service);
    while(srv_reg) {
        hgobj gobj_service = srv_reg->gobj;
        GCLASS *gclass = gobj_gclass(gobj_service);
        if(empty_string(gclass_name) || strcasecmp(gclass->gclass_name, gclass_name)==0) {
            json_t *jn_srv = json_object();
            if(srv_reg->service) {
                json_object_set_new(
                    jn_srv,
                    "id",
                    json_string(srv_reg->service)
                );
                json_object_set_new(
                    jn_srv,
                    "service",
                    json_string(srv_reg->service)
                );
            }
            json_object_set_new(
                jn_srv,
                "opt",
                opt2json(gobj_service)
            );
            json_object_set_new(
                jn_srv,
                "gobj",
                json_string(gobj_full_name(gobj_service))
            );

            json_array_append_new(jn_register, jn_srv);
        }

        srv_reg = dl_next(srv_reg);
    }

    return jn_register;
}

/***************************************************************************
 *  Return json list with the unique gobj names
 ***************************************************************************/
PUBLIC json_t * gobj_repr_unique_register(void)
{
    json_t *jn_register = json_array();

    const char *key;
    json_t *jn_value;

    json_object_foreach(__jn_unique_named_dict__, key, jn_value) {
        json_array_append_new(jn_register, json_string(key));
    }
    return jn_register;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t * gobj_global_variables(void)
{
    return json_pack("{s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s, s:s}",
        "__node_owner__", __node_owner__,
        "__realm_id__", __realm_id__,
        "__realm_owner__", __realm_owner__,
        "__realm_role__", __realm_role__,
        "__realm_name__", __realm_name__,
        "__realm_env__", __realm_env__,
        "__yuno_id__", __yuno_id__,
        "__yuno_role__", __yuno_role__,
        "__yuno_name__", __yuno_name__,
        "__yuno_tag__", __yuno_tag__,
        "__yuno_role_plus_name__", __yuno_role_plus_name__,
        "__yuno_role_plus_id__", __yuno_role_plus_id__,
        "__hostname__", get_host_name(),
#ifdef __linux__
        "__sys_system_name__", sys.sysname,
        "__sys_node_name__", sys.nodename,
        "__sys_version__", sys.version,
        "__sys_release__", sys.release,
        "__sys_machine__", sys.machine
#else
        "__sys_system_name__", "",
        "__sys_node_name__", "",
        "__sys_version__", "",
        "__sys_release__", "",
        "__sys_machine__", ""
#endif
    );
}

/***************************************************************************
 *  Factory to create service gobj
 *  Used in entry_point, to run services
 *  Internally it uses gobj_create_tree()
 ***************************************************************************/
PUBLIC hgobj gobj_service_factory(
    const char *name,
    json_t * jn_service_config  // owned
)
{
    const char *gclass_name = kw_get_str(jn_service_config, "gclass", 0, 0);
    if(empty_string(gclass_name)) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "service gclass EMPTY",
            "service",      "%s", name?name:"",
            NULL
        );
        JSON_DECREF(jn_service_config);
        return 0;
    }

    GCLASS *gclass = gobj_find_gclass(gclass_name, FALSE);
    if(!gclass) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gclass NOT FOUND",
            "service",      "%s", name?name:"",
            "gclass",       "%s", gclass_name,
            NULL
        );
        JSON_DECREF(jn_service_config);
        return 0;
    }
    const char *gobj_name = kw_get_str(jn_service_config, "name", 0, 0);

    json_t *jn_global = extract_json_config_variables_mine(
        gclass_name,
        gobj_name,
        __jn_global_settings__
    );
    json_t *__json_config_variables__ = kw_get_dict(
        jn_global,
        "__json_config_variables__",
        json_object(),
        KW_CREATE
    );
    json_object_update_new(
        __json_config_variables__,
        gobj_global_variables()
    );

    if(__trace_gobj_create_delete2__(__yuno__)) {
        trace_machine("🌞 %s^%s => service global",
            gclass_name,
            gobj_name
        );
        log_debug_json(0, jn_global, "service global");
    }

    json_t *kw_service_config = kw_apply_json_config_variables(
        jn_service_config,  // not owned
        jn_global           // not owned
    );
    json_decref(jn_global);

    json_object_set_new(kw_service_config, "service", json_true()); // Mark as service

    if(__trace_gobj_create_delete2__(__yuno__)) {
        trace_machine("🌞🌞 %s^%s => service final",
            gclass_name,
            gobj_name
        );
        log_debug_json(0, kw_service_config, "service final");
    }

    hgobj gobj = gobj_create_tree0(
        __yuno__,
        kw_service_config,  // owned
        0,
        0
    );

    JSON_DECREF(jn_service_config);
    return gobj;
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE int register_service(const char *name, hgobj gobj)
{
    if(!gobj) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }
    if(!name) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "name NULL",
            NULL
        );
        return 0;
    }

    service_register_t *srv_reg = dl_first(&dl_service);
    while(srv_reg) {
        if(srv_reg->service) {
            if(strcasecmp(srv_reg->service, name)==0) {
                break;
            }
        }
        srv_reg = dl_next(srv_reg);
    }
    if(srv_reg) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", "__yuno__",
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "service ALREADY REGISTERED. Will be UPDATED",
            "service",      "%s", name?name:"",
            "gobj_service", "%s", gobj_full_name(gobj),
            NULL
        );
        free_service_reg(srv_reg);
    }

    srv_reg = gbmem_malloc(sizeof(service_register_t));
    if(!srv_reg) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "no memory for sizeof(service_register_t)",
            "service",      "%s", name?name:"",
            NULL
        );
        return 0;
    }
    log_debug(0,
        "gobj",         "%s", __FILE__,
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_STARTUP,
        "msg",          "%s", "Register service",
        "service",      "%s", name?name:"",
        "gclass",       "%s", gobj_gclass_name(gobj),
        NULL
    );

    srv_reg->service = gbmem_strdup(name);
    srv_reg->gobj = gobj;
    dl_add(&dl_service, srv_reg);

    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE int register_transformation_filter(const char *name, json_t * (trans_filter)(json_t *))
{
    trans_filter_t *trans_reg = dl_first(&dl_trans_filter);
    while(trans_reg) {
        if(trans_reg->name) {
            if(strcasecmp(trans_reg->name, name)==0) {
                break;
            }
        }
        trans_reg = dl_next(trans_reg);
    }
    if(trans_reg) {
        log_error(0,
            "gobj",         "%s", "__yuno__",
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "trans filter ALREADY REGISTERED. Will be UPDATED",
            "name",         "%s", name?name:"",
            NULL
        );
        free_trans_filter(trans_reg);
    }

    trans_reg = gbmem_malloc(sizeof(trans_filter_t));
    if(!trans_reg) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "no memory for sizeof(trans_filter_t)",
            "name",         "%s", name?name:"",
            NULL
        );
        return -1;
    }
    log_debug(0,
        "gobj",         "%s", __FILE__,
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_STARTUP,
        "msg",          "%s", "Register transformation filter",
        "service",      "%s", name?name:"",
        NULL
    );

    trans_reg->name = gbmem_strdup(name);
    trans_reg->transformation_fn = trans_filter;
    dl_add(&dl_trans_filter, trans_reg);

    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE json_t *webix_trans_filter(
    json_t *kw  // owned
)
{
    return build_webix(
        0, // result
        0, // json_t *jn_comment,// owned
        0, // json_t *jn_schema, // owned
        kw // json_t *jn_data    // owned
    );
}




                    /*---------------------------------*
                     *  SECTION: Creation functions
                     *---------------------------------*/




/***************************************************************************
 *  Set subtree as oid_changed
 ***************************************************************************/
PRIVATE int cb_set(rc_instance_t *i_child, hgobj child, void *user_data, void *user_data2, void *user_data3)
{
    ((GObj_t *)child)->oid_changed = 1;
    return 0;
}

PRIVATE void set_subtree_oid_changed(GObj_t *gobj)
{
    gobj->oid_changed = 1;
    gobj_walk_gobj_childs_tree(gobj, WALK_TOP2BOTTOM, cb_set, 0, 0, 0);
}

/***************************************************************************
 *  Add/remove child
 ***************************************************************************/
PRIVATE inline void _add_child(GObj_t *parent, GObj_t *child)
{
    rc_add_child((rc_resource_t *)parent, (rc_resource_t *)child, 0);
    set_subtree_oid_changed(parent);
}

PRIVATE inline void _remove_child(GObj_t *parent, GObj_t *child)
{
    if(parent->bottom_gobj == child) {
        parent->bottom_gobj = 0;
    }
    rc_remove_child((rc_resource_t *)parent, (rc_resource_t *)child, 0);
    set_subtree_oid_changed(parent);
}

/***************************************************************************
 *  Create smachine
 ***************************************************************************/
PRIVATE SMachine_t * smachine_create(
    const FSM *fsm,
    void *self)
{
    SMachine_t *mach;

    mach = gbmem_malloc(sizeof(SMachine_t));
    if(!mach) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "no memory for sizeof(SMachine_t)",
            "sizeof(SMachine_t)",  "%d", sizeof(SMachine_t),
            NULL)
        ;
        return (SMachine_t *)0;
    }
    mach->fsm = fsm;
    mach->self = self;

    return (SMachine_t *) mach;
}

/***************************************************************************
 *  Destroy smachine
 ***************************************************************************/
PRIVATE int smachine_destroy(SMachine_t * mach)
{
    gbmem_free(mach);
    return 0;
}

/***************************************************************************
 *  Check smachine
 ***************************************************************************/
PRIVATE BOOL smachine_event_in_list(const EVENT *event_list, const char *event)
{
    int i;

    for(i=0; event_list[i].event!=0; i++) {
        if(strcasecmp(event, event_list[i].event)==0) {
            return TRUE;
        }
    }
    return FALSE;
}

/***************************************************************************
 *  Check smachine
 ***************************************************************************/
PRIVATE BOOL smachine_event_in_state(const FSM *fsm, const char *event)
{
    int i;

    if(!fsm->states) {
        return FALSE;
    }
    for(i=0; fsm->state_names[i]!=0; i++) {
        EV_ACTION *ev_action = fsm->states[i];
        if(ev_action) {
            while(ev_action->event && *ev_action->event) {
                if(strcasecmp(event, ev_action->event)==0) {
                    return TRUE;
                }
                ev_action++;
            }
        }
    }
    return FALSE;
}

/***************************************************************************
 *  Check smachine
 ***************************************************************************/
PRIVATE BOOL smachine_state_in_state_names(const FSM *fsm, const char *state)
{
    int i;

    if(!fsm->state_names) {
        return FALSE;
    }
    for(i=0; fsm->state_names[i]!=0; i++) {
        if(strcasecmp(state, fsm->state_names[i])==0) {
            return TRUE;
        }
    }
    return FALSE;
}

/***************************************************************************
 *  Check smachine
 ***************************************************************************/
PRIVATE int smachine_check(GCLASS *gclass)
{
    int i;
    const FSM *fsm = gclass->fsm;

    if(0) {
        for(i=0; fsm->input_events[i].event!=0; i++) {
            printf("Event name-> %s", fsm->input_events[i].event);
        }
        for(i=0; fsm->output_events[i].event!=0; i++) {
            printf("output name-> %s", fsm->output_events[i].event);
        }
        for(i=0; fsm->state_names[i]!=0; i++) {
            printf("state name-> %s", fsm->state_names[i]);
        }
        if(fsm->states) {
            for(i=0; fsm->state_names[i]!=0; i++) {
                EV_ACTION *ev_action = fsm->states[i];
                if(ev_action) {
                    printf("state -> %s", fsm->state_names[i]);
                    while(ev_action->event && *ev_action->event) {
                        printf("    event -> %s, nx: %s",
                            ev_action->event,
                            ev_action->next_state
                        );
                        ev_action++;
                    }
                }
            }
        }
    }

    /*
     *  check states
     */
    if(!fsm->states) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "GClass without fsm",
            "gclass",       "%s", gclass->gclass_name,
            NULL
        );
        return -1;
    }
    for(i=0; fsm->state_names[i]!=0; i++) {
        if(!fsm->states[i]) {
            log_error(LOG_OPT_EXIT_ZERO,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                "msg",          "%s", "SMachine: state names NOT MATCH with state list",
                "gclass",       "%s", gclass->gclass_name,
                NULL
            );
            return -1;
        }
    }
    for(i=0; fsm->states[i]!=0; i++) {
        if(!fsm->state_names[i]) {
            log_error(LOG_OPT_EXIT_ZERO,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                "msg",          "%s", "SMachine: state list NOT MATCH with state names",
                "gclass",       "%s", gclass->gclass_name,
                NULL
            );
            return -1;
        }
    }

    /*
     *  check state's event in input_list
     */
    for(i=0; fsm->state_names[i]!=0; i++) {
        EV_ACTION *ev_action = fsm->states[i];
        if(ev_action) {
            while(ev_action->event) {
                if(empty_string(ev_action->event)) {
                    log_error(0,
                        "gobj",         "%s", __FILE__,
                        "function",     "%s", __FUNCTION__,
                        "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                        "msg",          "%s", "SMachine: event name EMPTY",
                        "gclass",       "%s", gclass->gclass_name,
                        "state",        "%s", fsm->state_names[i],
                        NULL
                    );
                }
                if(!smachine_event_in_list(fsm->input_events, ev_action->event)) {
                    log_error(0,
                        "gobj",         "%s", __FILE__,
                        "function",     "%s", __FUNCTION__,
                        "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                        "msg",          "%s", "SMachine: state's event NOT in input_events",
                        "gclass",       "%s", gclass->gclass_name,
                        "state",        "%s", fsm->state_names[i],
                        "event",        "%s", ev_action->event,
                        NULL
                    );
                }
                if(ev_action->next_state && !smachine_state_in_state_names(fsm, ev_action->next_state)) {
                    log_error(0,
                        "gobj",         "%s", __FILE__,
                        "function",     "%s", __FUNCTION__,
                        "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                        "msg",          "%s", "SMachine: next state NOT in state names",
                        "gclass",       "%s", gclass->gclass_name,
                        "state",        "%s", fsm->state_names[i],
                        "next_state",   "%s", ev_action->next_state,
                        NULL
                    );
                }

                ev_action++;
            }
        }
    }

    /*
     *  check input_list's event in state
     */
    for(i=0; fsm->input_events[i].event!=0; i++) {
        if(!smachine_event_in_state(fsm, fsm->input_events[i].event)) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                "msg",          "%s", "SMachine: input_list's event NOT in state",
                "gclass",       "%s", gclass->gclass_name,
                "event",        "%s", fsm->input_events[i].event,
                NULL
            );
        }
    }

    return 0;
}

/***************************************************************************
 *  Create gobj
 ***************************************************************************/
PRIVATE hgobj _gobj_create(
    const char *name_,
    GCLASS *gclass,
    GObj_t *parent,
    json_t *kw,
    GObj_t *yuno,
    obflag_t obflag)
{
    GObj_t *gobj;

    /*--------------------------------*
     *      Check forbidden chars
     *      in GClass's name
     *--------------------------------*/
    if(strchr(gclass->gclass_name, '.')) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "GClass name CANNOT have '.' char",
            "gclass",       "%s", gclass->gclass_name,
            NULL
        );
        JSON_DECREF(kw);
        return (hgobj)0;
    }
    if(strchr(gclass->gclass_name, '`')) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "GClass name CANNOT have '`' char",
            "gclass",       "%s", gclass->gclass_name,
            NULL
        );
        JSON_DECREF(kw);
        return (hgobj)0;
    }
    if(strchr(gclass->gclass_name, '^')) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "GClass name CANNOT have '^' char",
            "gclass",       "%s", gclass->gclass_name,
            NULL
        );
        JSON_DECREF(kw);
        return (hgobj)0;
    }

    /*--------------------------------*
     *      Check forbidden chars
     *      in gobj's name
     *--------------------------------*/
    if(!empty_string(name_)) {
// WARNING no lo puedo poner, el '.' es usado cuando le pongo el nombre el peername a un gobj
//         if(strchr(name_, '.')) {
//             log_error(LOG_OPT_TRACE_STACK,
//                 "gobj",         "%s", __FILE__,
//                 "function",     "%s", __FUNCTION__,
//                 "msgset",       "%s", MSGSET_PARAMETER_ERROR,
//                 "msg",          "%s", "GObj name CANNOT have '.' char",
//                 "gclass",       "%s", gclass->gclass_name,
//                 "name",         "%s", name_,
//                 NULL
//             );
//             JSON_DECREF(kw);
//             return (hgobj)0;
//         }
        if(strchr(name_, '`')) {
            log_error(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "GObj name CANNOT have '`' char",
                "gclass",       "%s", gclass->gclass_name,
                "name",         "%s", name_,
                NULL
            );
            JSON_DECREF(kw);
            return (hgobj)0;
        }
        if(strchr(name_, '^')) {
            log_error(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "GObj name CANNOT have '^' char",
                "gclass",       "%s", gclass->gclass_name,
                "name",         "%s", name_,
                NULL
            );
            JSON_DECREF(kw);
            return (hgobj)0;
        }
    }

    /*--------------------------------*
     *      Alloc memory
     *--------------------------------*/
    gobj = gbmem_malloc(sizeof(GObj_t));
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "no memory for sizeof(GObj_t)",
            "sizeof(GObj_t)",  "%d", sizeof(GObj_t),
            NULL
        );
        JSON_DECREF(kw);
        return (hgobj)0;
    }

    /*--------------------------------*
     *      Alloc private data
     *--------------------------------*/
    gobj->priv = gbmem_malloc(gclass->priv_size);
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "no memory for private data",
            "priv_size",    "%d", gclass->priv_size,
            NULL);
        gobj_destroy(gobj);
        JSON_DECREF(kw);
        return (hgobj)0;
    }

    /*--------------------------------*
     *      Initialize struct
     *--------------------------------*/
    gobj->obflag = obflag;
    gobj->refcount = 1;

    /*
     *  For better debug, I prefer have no dynamic memory for name.
     */
    strncpy(gobj->name, name_?name_:"", MAX_GOBJ_NAME);
    gobj->gclass = gclass;
    gobj->yuno = yuno;

    rc_init_iter(&gobj->dl_instances);
    rc_init_iter(&gobj->dl_childs);

    rc_init_iter(&gobj->dl_subscriptions);
    rc_init_iter(&gobj->dl_subscribings);

    if(!gobj->yuno) {
        if(gobj->obflag & (obflag_yuno)) {
            gobj->yuno = gobj;
        } else {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "I NEED a yuno!",
                "gclass",       "%s", gclass->gclass_name,
                "name",         "%s", gobj->name,
                NULL
            );
        }
    }

    if(__trace_gobj_create_delete__(gobj)) {
        trace_machine("💙💙⏩ creating: %s^%s",
            gclass->gclass_name,
            gobj->name
        );
    }

    /*--------------------------------*
     *      Check smachine
     *--------------------------------*/
    if(!gclass->fsm_checked) {
        smachine_check(gclass);
        gclass->fsm_checked = TRUE;
    }

    /*--------------------------------*
     *      Create smachine
     *--------------------------------*/
    gobj->mach = smachine_create(gclass->fsm, gobj);
    if(!gobj->mach) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "smachine_create() return NULL",
            "gclass",       "%s", gclass->gclass_name,
            "name",         "%s", gobj->name,
            NULL);
        gobj_destroy(gobj);
        JSON_DECREF(kw);
        return (hgobj)0;
    }

    /*--------------------------------*
     *      Alloc config
     *--------------------------------*/
    gobj->hsdata_attr = sdata_create(
        gclass->tattr_desc,
        gobj,
        on_post_write_it_cb,
        on_post_read_it_cb,
        on_post_write_stats_it_cb,
        0
    );
    if(!gobj->hsdata_attr) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "sdata_create() return NULL",
            "gclass",       "%s", gclass->gclass_name,
            "name",         "%s", gobj->name,
            NULL
        );
        gobj_destroy(gobj);
        JSON_DECREF(kw);
        return (hgobj)0;
    }

    /*--------------------------------*
     *      Alloc user_data
     *--------------------------------*/
    gobj->jn_user_data = json_object();

    /*--------------------------------*
     *      Alloc stats_data
     *--------------------------------*/
    gobj->jn_stats = json_object();

    /*--------------------------*
     *  Register unique names
     *--------------------------*/
    if(gobj->obflag & (obflag_unique_name|obflag_yuno|obflag_default_service|obflag_service)) {
        register_unique_gobj(gobj);
    }

    /*----------------------*
     *  Register services
     *----------------------*/
    if(gobj->obflag & (obflag_yuno|obflag_default_service|obflag_service)) {
        register_service(name_, gobj);
    }
    if(gobj->obflag & (obflag_default_service)) {
        if(__default_service__) {
            log_warning(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "Default Service ALREADY CREATED",
                NULL
            );
        }
        __default_service__ = gobj;
    }

    /*--------------------------------*
     *  Write configuration
     *--------------------------------*/
    gobj_write_json_parameters(gobj, kw, __jn_global_settings__);

    /*--------------------------------------*
     *  Load writable and persistent attrs
     *  of named-gobjs and __root__
     *--------------------------------------*/
    if(gobj->obflag & (obflag_unique_name)) {
        gobj_load_persistent_attrs(gobj, 0);
    }

    /*--------------------------------------*
     *      Add to parent
     *--------------------------------------*/
    if(!(gobj->obflag & (obflag_yuno))) {
        _add_child(parent, gobj);
    }

    /*---------------------------------------*
     *  Info before mt_create():
     *  Before possible creation of childs
     *---------------------------------------*/
    if(__trace_gobj_monitor__(gobj)) {
        if(gobj->obflag & (obflag_yuno)) {
            monitor_gobj(MTOR_GOBJ_YUNO_CREATED, gobj);
        } else if(gobj->obflag & (obflag_default_service)) {
            monitor_gobj(MTOR_GOBJ_DEFAULT_SERVICE_CREATED, gobj);
        } else if(gobj->obflag & (obflag_service)) {
            monitor_gobj(MTOR_GOBJ_SERVICE_CREATED, gobj);
        } else if(gobj->obflag & (obflag_unique_name)) {
            monitor_gobj(MTOR_GOBJ_UNIQUE_CREATED, gobj);
        } else {
            monitor_gobj(MTOR_GOBJ_CREATED, gobj);
        }
    }

    /*--------------------------------*
     *      Exec mt_create
     *--------------------------------*/
    gobj->obflag |= obflag_created;
    if(gobj->gclass->gmt.mt_create2) {
        JSON_INCREF(kw);
        gobj->gclass->gmt.mt_create2(gobj, kw);
    } else if(gobj->gclass->gmt.mt_create) {
        gobj->gclass->gmt.mt_create(gobj);
    }

    /*--------------------------------------*
     *  Inform to parent
     *  when the child is full operative
     *-------------------------------------*/
    if(parent && parent->gclass->gmt.mt_child_added) {
        if(__trace_gobj_create_delete__(gobj)) {
            trace_machine("👦👦🔵 child_added(%s): %s",
                gobj_full_name(parent),
                gobj_short_name(gobj)
            );
        }
        parent->gclass->gmt.mt_child_added(parent, gobj);
    }

    if(__trace_gobj_create_delete__(gobj)) {
        trace_machine("💙💙⏪ created: %s, p: %p",
            gobj_full_name(gobj),
            gobj
        );
    }
    gobj->gclass->__instances__++;

    JSON_DECREF(kw);

    if((gobj->obflag & (obflag_yuno))) {
        __yuno__ = gobj;
    }

    if(__yuno__ && __yuno__->gclass->gmt.mt_gobj_created) {
        __yuno__->gclass->gmt.mt_gobj_created(__yuno__, gobj);
    }

    return (hgobj) gobj;
}

/***************************************************************************
 *  Create gobj
 ***************************************************************************/
PRIVATE hgobj _create_yuno(
    const char *name,
    GCLASS *gclass,
    json_t *kw)
{
    GObj_t * gobj;
    obflag_t obflag = obflag_yuno;;

    gobj = _gobj_create(name, gclass, 0, kw, 0, obflag);
    if(!gobj) {
        return 0;
    }
    return gobj;
}

/***************************************************************************
 *  Create gobj
 ***************************************************************************/
PUBLIC hgobj gobj_create(
    const char *name,
    GCLASS *gclass,
    json_t *kw,
    hgobj parent_)
{
    GObj_t *parent = parent_;
    obflag_t obflag = 0;

    if(!gclass) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj_create() NEEDS a gclass!",
            "name",         "%s", name?name:"",
            NULL);
        return (hgobj) 0;
    }
    if(!parent) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj_create() NEEDS a parent!",
            "gclass",       "%s", gclass->gclass_name,
            "name",         "%s", name?name:"",
            NULL);
        return (hgobj) 0;
    }
    if(!parent->yuno) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj_create() NEEDS a parent with yuno!",
            "gclass",       "%s", gclass->gclass_name,
            "name",         "%s", name?name:"",
            NULL);
        return (hgobj) 0;
    }
    return _gobj_create(name, gclass, parent, kw, parent->yuno, obflag);
}

/***************************************************************************
 *  Create a unique-named gobj.
 ***************************************************************************/
PUBLIC hgobj gobj_create_unique(
    const char *name,
    GCLASS *gclass,
    json_t *kw,
    hgobj parent_)
{
    obflag_t obflag = obflag_unique_name;
    GObj_t *parent = parent_;

    if(!gclass) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "GClass NULL!",
            "name",         "%s", name?name:"",
            NULL
        );
        return (hgobj) 0;
    }
    if(!parent) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "Parent NULL!",
            "gclass",       "%s", gclass->gclass_name,
            "name",         "%s", name?name:"",
            NULL
        );
        return (hgobj) 0;
    }
    if(!parent->yuno) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "yuno NULL",
            "gclass",       "%s", gclass->gclass_name,
            "name",         "%s", name?name:"",
            NULL
        );
        return (hgobj) 0;
    }

    hgobj gobj = gobj_find_unique_gobj(name, FALSE);
    if(gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "unique gobj ALREADY registered!",
            "gclass",       "%s", gclass->gclass_name,
            "name",         "%s", name?name:"",
            NULL
        );
        return (hgobj) 0;
    }

    return _gobj_create(name, gclass, parent, kw, parent->yuno, obflag);
}

/***************************************************************************
 *  Create a volatil gobj.
 ***************************************************************************/
PUBLIC hgobj gobj_create_volatil(
    const char *name,
    GCLASS *gclass,
    json_t *kw,
    hgobj parent_)
{
    obflag_t obflag = obflag_volatil;
    GObj_t *parent = parent_;

    if(!gclass) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "GClass NULL!",
            "name",         "%s", name?name:"",
            NULL
        );
        return (hgobj) 0;
    }
    if(!parent) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "Parent NULL!",
            "gclass",       "%s", gclass->gclass_name,
            "name",         "%s", name?name:"",
            NULL
        );
        return (hgobj) 0;
    }
    if(!parent->yuno) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "yuno NULL",
            "gclass",       "%s", gclass->gclass_name,
            "name",         "%s", name?name:"",
            NULL
        );
        return (hgobj) 0;
    }

    return _gobj_create(name, gclass, parent, kw, parent->yuno, obflag);
}

/***************************************************************************
 *  Create a service gobj.
 ***************************************************************************/
PUBLIC hgobj gobj_create_service(
    const char *name,
    GCLASS *gclass,
    json_t *kw,
    hgobj parent_)
{
    obflag_t obflag = obflag_service;
    GObj_t *parent = parent_;

    if(!gclass) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj_create_service() NEEDS a gclass!",
            "name",         "%s", name?name:"",
            NULL
        );
        return (hgobj) 0;
    }
    if(!parent) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj_create_service() NEEDS a parent!",
            "gclass",       "%s", gclass->gclass_name,
            "name",         "%s", name?name:"",
            NULL
        );
        return (hgobj) 0;
    }
    if(!parent->yuno) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj_create_service() NEEDS a parent with yuno!",
            "gclass",       "%s", gclass->gclass_name,
            "name",         "%s", name?name:"",
            NULL
        );
        return (hgobj) 0;
    }

    return _gobj_create(name, gclass, parent, kw, parent->yuno, obflag);
}

/***************************************************************************
 *  Create tree
 ***************************************************************************/
PUBLIC hgobj gobj_create_tree0(
    hgobj parent_,
    json_t *jn_tree,
    const char *ev_on_setup,
    const char *ev_on_setup_complete
)
{
    GObj_t *parent = parent_;
    obflag_t obflag = 0;
    const char *gclass_name = kw_get_str(jn_tree, "gclass", "", KW_REQUIRED);
    const char *name = kw_get_str(jn_tree, "name", "", 0);
    BOOL default_service = kw_get_bool(jn_tree, "default_service", 0, 0);
    BOOL as_service = kw_get_bool(jn_tree, "as_service", 0, 0) ||
        kw_get_bool(jn_tree, "service", 0, 0);
    BOOL as_unique = kw_get_bool(jn_tree, "as_unique", 0, 0) ||
        kw_get_bool(jn_tree, "unique", 0, 0);
    BOOL autoplay = kw_get_bool(jn_tree, "autoplay", 0, 0);
    BOOL autostart = kw_get_bool(jn_tree, "autostart", 0, 0);
    BOOL disabled = kw_get_bool(jn_tree, "disabled", 0, 0);
    GCLASS *gclass = gobj_find_gclass(gclass_name, FALSE);
    if(!gclass) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gclass not found",
            "gclass_name",  "%s", gclass_name,
            "name",         "%s", name?name:"",
            NULL
        );
        JSON_DECREF(jn_tree);
        return 0;
    }
    BOOL local_kw = FALSE;
    json_t *kw = kw_get_dict(jn_tree, "kw", 0, 0);
    if(!kw) {
        local_kw = TRUE;
        kw = json_object();
    }

    if(gclass_has_attr(gclass, "subscriber")) {
        if(!kw_has_key(kw, "subscriber")) { // WARNING TOO implicit
            if(parent != gobj_yuno()) {
                json_object_set_new(kw, "subscriber", json_integer((json_int_t)(size_t)parent));
            }
        } else {
            json_t *jn_subscriber = kw_get_dict_value(kw, "subscriber", 0, 0);
            if(json_is_string(jn_subscriber)) {
                const char *subscriber_name = json_string_value(jn_subscriber);
                hgobj subscriber = gobj_find_unique_gobj(subscriber_name, FALSE);
                if(subscriber) {
                    json_object_set_new(kw, "subscriber", json_integer((json_int_t)(size_t)subscriber));
                } else {
                    log_error(0,
                        "gobj",         "%s", __FILE__,
                        "function",     "%s", __FUNCTION__,
                        "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                        "msg",          "%s", "subscriber unique gobj NOT FOUND",
                        "subcriber",    "%s", subscriber_name,
                        "name",         "%s", name?name:"",
                        "gclass",       "%s", gclass_name,
                        NULL
                    );
                }
            } else if(json_is_integer(jn_subscriber)) {
                json_object_set_new(kw, "subscriber", json_integer(json_integer_value(jn_subscriber)));
            } else {
                log_error(0,
                    "gobj",         "%s", __FILE__,
                    "function",     "%s", __FUNCTION__,
                    "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                    "msg",          "%s", "subscriber json INVALID",
                    "name",         "%s", name?name:"",
                    "gclass",       "%s", gclass_name,
                    NULL
                );
            }
        }
    }

    if(!local_kw) {
        json_incref(kw);
    }
    if(default_service) {
        obflag |= obflag_default_service;
    }
    if(as_service) {
        obflag |= obflag_service;
    }
    if(as_unique) {
        obflag |= obflag_unique_name;
    }
    if(autoplay) {
        obflag |= obflag_autoplay;
    }
    if(autostart) {
        obflag |= obflag_autostart;
    }

    hgobj first_child = _gobj_create(name, gclass, parent, kw, parent->yuno, obflag);
    if(!first_child) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "_gobj_create() FAILED",
            "name",         "%s", name?name:"",
            "gclass",       "%s", gclass_name,
            NULL
        );
        JSON_DECREF(jn_tree);
        return 0;
    }
    if(disabled) {
        gobj_disable(first_child);
    }

    if(!empty_string(ev_on_setup)) {
        if(gobj_event_in_input_event_list(parent, ev_on_setup, 0)) {
            gobj_send_event(parent, ev_on_setup, 0, first_child);
        }
    }

    hgobj last_child = 0;
    json_t *jn_childs = kw_get_list(jn_tree, "zchilds", 0, 0);
    size_t index;
    json_t *jn_child;
    json_array_foreach(jn_childs, index, jn_child) {
        if(!json_is_object(jn_child)) {
            continue;
        }
        json_incref(jn_child);
        last_child = gobj_create_tree0(
            first_child,
            jn_child,
            ev_on_setup,
            ev_on_setup_complete
        );
        if(!last_child) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                "msg",          "%s", "gobj_create_tree0() FAILED",
                "jn_child",     "%j", jn_child,
                NULL
            );
            JSON_DECREF(jn_tree);
            return 0;
        }
    }
    if(json_array_size(jn_childs) == 1) {
        gobj_set_bottom_gobj(first_child, last_child);
    }

    if(!empty_string(ev_on_setup_complete)) {
        if(gobj_event_in_input_event_list(parent, ev_on_setup_complete, 0)) {
            gobj_send_event(parent, ev_on_setup_complete, 0, first_child);
        }
    }
    JSON_DECREF(jn_tree);
    return first_child;
}

/***************************************************************************
 *  Create gobj tree
 ***************************************************************************/
PUBLIC hgobj gobj_create_tree(
    hgobj parent_,
    const char *tree_config_,
    const char *json_config_variables,
    const char *ev_on_setup,
    const char *ev_on_setup_complete)
{
    GObj_t *parent = parent_;

    if(!parent) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "parent null",
            NULL);
        return (hgobj) 0;
    }
    if(!parent->yuno) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "parent without yuno",
            NULL);
        return (hgobj) 0;
    }

    char *config = gbmem_strdup(tree_config_);
    helper_quote2doublequote(config);

    char *__json_config_variables__ = gbmem_strdup(json_config_variables);
    helper_quote2doublequote(__json_config_variables__);

    char *tree_config = json_config(
        0,
        0,
        0,                          // fixed_config
        config,                     // variable_config
        0,                          // config_json_file
        __json_config_variables__,  // parameter_config
        PEF_CONTINUE
    );
    gbmem_free(config);
    gbmem_free(__json_config_variables__);

    json_t *jn_tree = legalstring2json(tree_config, TRUE);
    jsonp_free(tree_config) ;

    if(!jn_tree) {
        // error already logged
        return 0;
    }
    return gobj_create_tree0(parent, jn_tree, ev_on_setup, ev_on_setup_complete);
}

/***************************************************************************
 *  Destroy gobj
 ***************************************************************************/
PUBLIC void gobj_destroy(hgobj gobj_)
{
    register GObj_t * gobj = gobj_;
    GObj_t * parent;

    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return;
    }
    if(gobj->obflag & obflag_destroying) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj DESTROYED",
            "gobj",         "%p", gobj,
            NULL
        );
        return;
    }
    gobj->obflag |= obflag_destroying;

    if(__trace_gobj_create_delete__(gobj)) {
        trace_machine("💔💔⏩ destroying: %s, p: %p",
            gobj_full_name(gobj),
            gobj
        );
    }

    /*------------------------------------------*
     *  Inform to parent
     *  when the child is still full operative
     *------------------------------------------*/
    parent = gobj->__parent__;
    if(parent && parent->gclass->gmt.mt_child_removed) {
        if(__trace_gobj_create_delete__(gobj)) {
            trace_machine("👦👦🔴 child_removed(%s): %s",
                gobj_full_name(parent),
                gobj_short_name(gobj)
            );
        }
        parent->gclass->gmt.mt_child_removed(parent, gobj);
    }

    /*--------------------------------*
     *  Deregister if unique name
     *--------------------------------*/
    if(gobj->obflag & obflag_unique_name) {
        deregister_unique_gobj(gobj);
    }
    if(gobj->obflag & obflag_service) {
        service_register_t *srv_reg = _find_service(gobj->name);
        if(srv_reg) {
            free_service_reg(srv_reg);
        }
    }

    /*--------------------------------*
     *      Pause
     *--------------------------------*/
    if(gobj->playing) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
            "msg",          "%s", "Destroying a PLAYING gobj",
            "full-name",    "%s", gobj_full_name(gobj),
            NULL
        );
        gobj_pause(gobj);
    }
    /*--------------------------------*
     *      Stop
     *--------------------------------*/
    if(gobj->running) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
            "msg",          "%s", "Destroying a RUNNING gobj",
            "full-name",    "%s", gobj_full_name(gobj),
            NULL
        );
        gobj_stop(gobj);
    }

    /*--------------------------------*
     *      Delete subscriptions
     *--------------------------------*/
    _delete_subscribings(gobj);
    _delete_subscriptions(gobj);

    /*--------------------------------*
     *      Delete from parent
     *--------------------------------*/
    if(parent) {
        _remove_child(parent, gobj);
    }

    /*--------------------------------*
     *      Delete childs
     *--------------------------------*/
    gobj_destroy_childs(gobj);

    /*--------------------------------*
     *      Mark as destroyed
     *--------------------------------*/
    gobj->obflag |= obflag_destroyed;

    /*-------------------------------------------------*
     *  Exec mt_destroy
     *  Call this after all childs are destroyed.
     *  Then you can delete resources used by childs
     *  (example: event_loop in main/threads)
     *-------------------------------------------------*/
    if(gobj->obflag & obflag_created) {
        if(gobj->gclass->gmt.mt_destroy) {
            gobj->gclass->gmt.mt_destroy(gobj);
        }
    }

    /*--------------------------------*
     *  Free user_data, stats_data
     *--------------------------------*/
    JSON_DECREF(gobj->jn_user_data);
    JSON_DECREF(gobj->jn_stats);

    /*----------------------------------------------*
     *  Info after mt_destroy():
     *  DESTROYED event will occur first in childs
     *----------------------------------------------*/
    if(__trace_gobj_monitor__(gobj)) {
        monitor_gobj(MTOR_GOBJ_DESTROYED, gobj);
    }
    if(__trace_gobj_create_delete__(gobj)) {
        trace_machine("💔💔⏪ destroyed: p: %p",
            gobj
        );
    }
    gobj->gclass->__instances__--;

    if(gobj->obflag & obflag_yuno) {
        JSON_DECREF(__jn_unique_named_dict__);
    }
    gobj_free(gobj);
}

/***************************************************************************
 *  Free gobj memory
 *  Useful when you cannot free gobj memory because it's using by
 *  external resources (libuv for example).
 *      You can mark the gobj as future release in mt_destroy.
 *      When the external callback are called, you can call gobj_free.
 ***************************************************************************/
PRIVATE void gobj_free(hgobj gobj_)
{
    register GObj_t * gobj = gobj_;
    /*--------------------------------*
     *      Delete smachine
     *--------------------------------*/
    if(gobj->mach) {
        smachine_destroy(gobj->mach);
        gobj->mach = 0;
    }

    /*--------------------------------*
     *      Delete subscriptions
     *--------------------------------*/
    rc_free_iter(&gobj->dl_subscriptions, FALSE, sdata_destroy);
    rc_free_iter(&gobj->dl_subscribings, FALSE, sdata_destroy);

    /*--------------------------------*
     *      Delete attr
     *--------------------------------*/
    if(gobj->hsdata_attr) {
        sdata_destroy(gobj->hsdata_attr);
        gobj->hsdata_attr = 0;
    }

    /*--------------------------------*
     *      Free memory
     *--------------------------------*/
    if(gobj->full_name) {
        gbmem_free((void *)gobj->full_name);
        gobj->full_name = 0;
    }
    if(gobj->short_name) {
        gbmem_free((void *)gobj->short_name);
        gobj->short_name = 0;
    }
    if(gobj->escaped_short_name) {
        gbmem_free((void *)gobj->escaped_short_name);
        gobj->escaped_short_name = 0;
    }
    if(gobj->error_message) {
        gbmem_free((void *)gobj->error_message);
        gobj->error_message = 0;
    }
    if(gobj->poid) {
        gbmem_free((void *)gobj->poid);
        gobj->poid = 0;
    }
    if(gobj->priv) {
        gbmem_free(gobj->priv);
        gobj->priv = 0;
    }
    gbmem_free(gobj);
}

/***************************************************************************
 *  Destroy named childs, with auto pause/stop
 ***************************************************************************/
PRIVATE int cb_imminentdestroy_named_childs(
    rc_instance_t *i_child, hgobj child, void *user_data, void *user_data2, void *user_data3
)
{
    const char *name = user_data;
    const char *name_ = gobj_name(child);
    if(name_ && strcmp(name_, name)==0) {
        if(__trace_gobj_create_delete__(child)) {
            trace_machine("❎❎📍 gobj_set_imminent_destroy: %s",
                gobj_short_name(child)
            );
        }
        gobj_set_imminent_destroy(child, TRUE);
    }
    return 0;
}
PRIVATE int cb_destroy_named_childs(
    rc_instance_t *i_child, hgobj child, void *user_data, void *user_data2, void *user_data3
)
{
    const char *name = user_data;
    const char *name_ = gobj_name(child);
    if(name_ && strcmp(name_, name)==0) {
        if(__trace_gobj_create_delete__(child)) {
            trace_machine("❎❎❌ gobj_destroy with previous pause/stop: %s",
                gobj_short_name(child)
            );
        }
        if(gobj_is_playing(child))
            gobj_pause(child);
        if(gobj_is_running(child))
            gobj_stop(child);
        gobj_destroy(child);
    }
    return 0;
}
PUBLIC int gobj_destroy_named_childs(hgobj gobj_, const char *name)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }

    if(__trace_gobj_create_delete__(gobj)) {
        trace_machine("❎❎ gobj_destroy_named_childs: %s",
            name
        );
    }

    gobj_walk_gobj_childs(
        gobj, WALK_FIRST2LAST, cb_imminentdestroy_named_childs, (void *)name, 0, 0
    );
    gobj_walk_gobj_childs(
        gobj, WALK_FIRST2LAST, cb_destroy_named_childs, (void *)name, 0, 0
    );
    return 0;
}

PUBLIC int gobj_destroy_named_tree(hgobj gobj_, const char *name)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }

    if(__trace_gobj_create_delete__(gobj)) {
        trace_machine("❎❎✝ gobj_destroy_named_tree: %s",
            name
        );
    }

    gobj_walk_gobj_childs_tree(
        gobj, WALK_BOTTOM2TOP, cb_imminentdestroy_named_childs, (void *)name, 0, 0
    );
    gobj_walk_gobj_childs_tree(
        gobj, WALK_BOTTOM2TOP, cb_destroy_named_childs, (void *)name, 0, 0
    );
    return 0;
}

/***************************************************************************
 *  Destroy childs
 ***************************************************************************/
PUBLIC void gobj_destroy_childs(hgobj gobj_)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return;
    }

    GObj_t * child; rc_instance_t *i_child;
    while((i_child = gobj_first_child(gobj, (hgobj *)&child))) {
        if(!(child->obflag & (obflag_destroyed|obflag_destroying))) {
            gobj_destroy(child);
        }
    }
}

/***************************************************************************
 *  Duplicate gclass structure
 ***************************************************************************/
PUBLIC GCLASS *gobj_subclass_gclass(GCLASS *base, const char *gclass_name)
{
    GCLASS *gclass;

    gclass = gbmem_malloc(sizeof(GCLASS)); // WARNING: this memory is not freed
    if(!gclass) {
        return 0;
    }
    memcpy(gclass, base, sizeof(GCLASS));
    gclass->gclass_name = gclass_name;
    gclass->base = base;
    return gclass;
}

/***************************************************************************
 *  register unique named gobj
 ***************************************************************************/
PRIVATE int register_unique_gobj(GObj_t * gobj)
{
    const char *unique_name = gobj_name(gobj);

    if(!__jn_unique_named_dict__) {
        __jn_unique_named_dict__ = json_object();
    }
    if(json_object_get(__jn_unique_named_dict__, unique_name)) {
        hgobj prev_gobj = (hgobj)(size_t)json_integer_value(
            json_object_get(__jn_unique_named_dict__, unique_name)
        );
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", "__yuno__",
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj unique ALREADY REGISTERED. Will be UPDATED",
            "prev gclass",  "%s", gobj_gclass_name(prev_gobj),
            "gclass",       "%s", gobj_gclass_name(gobj),
            "name",         "%s", gobj_name(gobj),
            NULL
        );
        json_object_del(__jn_unique_named_dict__, unique_name);
    }

    int ret = json_object_set_new(
        __jn_unique_named_dict__,
        unique_name,
        json_integer((json_int_t)(size_t)gobj)
    );
    if(ret == -1) {
        log_error(0,
            "gobj",         "%s", "__yuno__",
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_JSON_ERROR,
            "msg",          "%s", "json_object_set_new() FAILED",
            "gclass",       "%s", gobj_gclass_name(gobj),
            "name",         "%s", gobj_name(gobj),
            NULL
        );
    }
    gobj->obflag |= obflag_unique_name;

    return ret;
}

/***************************************************************************
 *  deregister unique named gobj
 ***************************************************************************/
PRIVATE int deregister_unique_gobj(GObj_t * gobj)
{
    const char *unique_name = gobj_name(gobj);

    hgobj gobj_found = json_object_get(__jn_unique_named_dict__, unique_name);
    if(!gobj_found) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", "__yuno__",
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NOT FOUND",
            "gclass",       "%s", gobj_gclass_name(gobj),
            "name",         "%s", gobj_name(gobj),
            NULL
        );
        return -1;
    }
    json_object_del(__jn_unique_named_dict__, unique_name);
    gobj->obflag &= ~obflag_unique_name;

    return 0;
}




                    /*------------------------------*
                     *      Resource functions
                     *------------------------------*/




/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_create_resource(
    hgobj gobj_,
    const char *resource,
    json_t *kw,  // owned
    json_t *jn_options // owned
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        JSON_DECREF(jn_options);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_create_resource) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_create_resource not defined",
            NULL
        );
        KW_DECREF(kw);
        JSON_DECREF(jn_options);
        return 0;
    }

    return gobj->gclass->gmt.mt_create_resource(gobj, resource, kw, jn_options);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_save_resource(
    hgobj gobj_,
    const char *resource,
    json_t *record,     // WARNING NOT owned
    json_t *jn_options // owned
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        JSON_DECREF(jn_options);
        return -1;
    }
    if(!record) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj_save_resource(): record NULL",
            NULL
        );
        JSON_DECREF(jn_options);
        return -1;
    }
    if(!gobj->gclass->gmt.mt_save_resource) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "mt_save_resource not defined",
            NULL
        );
        JSON_DECREF(jn_options);
        return -1;
    }

    return gobj->gclass->gmt.mt_save_resource(gobj, resource, record, jn_options);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_delete_resource(
    hgobj gobj_,
    const char *resource,
    json_t *record,  // owned
    json_t *jn_options // owned
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(record);
        JSON_DECREF(jn_options);
        return -1;
    }
    if(!record) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj_delete_resource(): record NULL",
            NULL
        );
        JSON_DECREF(jn_options);
        return -1;
    }
    if(!gobj->gclass->gmt.mt_delete_resource) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "mt_delete_resource not defined",
            NULL
        );
        KW_DECREF(record);
        JSON_DECREF(jn_options);
        return -1;
    }

    return gobj->gclass->gmt.mt_delete_resource(gobj, resource, record, jn_options);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_list_resource(
    hgobj gobj_,
    const char *resource,
    json_t *jn_filter,  // owned
    json_t *jn_options // owned
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(jn_filter);
        JSON_DECREF(jn_options);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_list_resource) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "mt_list_resource not defined",
            NULL
        );
        KW_DECREF(jn_filter);
        JSON_DECREF(jn_options);
        return 0;
    }
    return gobj->gclass->gmt.mt_list_resource(gobj, resource, jn_filter, jn_options);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_get_resource( // WARNING return is NOT yours!
    hgobj gobj_,
    const char *resource,
    json_t *jn_filter,  // owned
    json_t *jn_options // owned
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(jn_filter);
        JSON_DECREF(jn_options);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_get_resource) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_get_resource not defined",
            NULL
        );
        KW_DECREF(jn_filter);
        JSON_DECREF(jn_options);
        return 0;
    }
    return gobj->gclass->gmt.mt_get_resource(gobj, resource, jn_filter, jn_options);
}




                    /*------------------------------*
                     *      Node functions
                     *------------------------------*/




/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_treedbs( // Return a list with treedb names
    hgobj gobj_,
    json_t *kw,
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_treedbs) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_treedbs not defined",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }
    return gobj->gclass->gmt.mt_treedbs(gobj, kw, src);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_treedb_topics(
    hgobj gobj_,
    const char *treedb_name,
    json_t *options, // "dict" return list of dicts, otherwise return list of strings
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(options);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_treedb_topics) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_treedb_topics not defined",
            NULL
        );
        KW_DECREF(options);
        return 0;
    }
    return gobj->gclass->gmt.mt_treedb_topics(gobj, treedb_name, options, src);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_topic_desc(
    hgobj gobj_,
    const char *topic_name
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        return 0;
    }
    if(!gobj->gclass->gmt.mt_topic_desc) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_topic_desc not defined",
            NULL
        );
        return 0;
    }
    return gobj->gclass->gmt.mt_topic_desc(gobj, topic_name);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_topic_links(
    hgobj gobj_,
    const char *treedb_name,
    const char *topic_name,
    json_t *kw,
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_topic_links) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_topic_links not defined",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }
    return gobj->gclass->gmt.mt_topic_links(gobj, treedb_name, topic_name, kw, src);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_topic_hooks(
    hgobj gobj_,
    const char *treedb_name,
    const char *topic_name,
    json_t *kw,
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_topic_hooks) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_topic_hooks not defined",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }
    return gobj->gclass->gmt.mt_topic_hooks(gobj, treedb_name, topic_name, kw, src);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC size_t gobj_topic_size(
    hgobj gobj_,
    const char *topic_name
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        return 0;
    }
    if(!gobj->gclass->gmt.mt_topic_size) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_topic_size not defined",
            NULL
        );
        return 0;
    }
    return gobj->gclass->gmt.mt_topic_size(gobj, topic_name);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_create_node( // Return is YOURS
    hgobj gobj_,
    const char *topic_name,
    json_t *kw,
    json_t *jn_options, // fkey,hook options
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        JSON_DECREF(jn_options);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_create_node) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_create_node not defined",
            NULL
        );
        KW_DECREF(kw);
        JSON_DECREF(jn_options);
        return 0;
    }
    return gobj->gclass->gmt.mt_create_node(gobj, topic_name, kw, jn_options, src);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_update_node( // Return is YOURS
    hgobj gobj_,
    const char *topic_name,
    json_t *kw,         // 'id' and pkey2s fields are used to find the node
    json_t *jn_options, // "create" "autolink" "volatil" fkey,hook options
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        JSON_DECREF(jn_options);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_update_node) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_update_node not defined",
            NULL
        );
        KW_DECREF(kw);
        JSON_DECREF(jn_options);
        return 0;
    }
    return gobj->gclass->gmt.mt_update_node(gobj, topic_name, kw, jn_options, src);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_delete_node(
    hgobj gobj_,
    const char *topic_name,
    json_t *kw,         // 'id' and pkey2s fields are used to find the node
    json_t *jn_options, // "force"
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        JSON_DECREF(jn_options);
        return -1;
    }
    if(!gobj->gclass->gmt.mt_delete_node) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_delete_node not defined",
            NULL
        );
        KW_DECREF(kw);
        JSON_DECREF(jn_options);
        return -1;
    }
    return gobj->gclass->gmt.mt_delete_node(gobj, topic_name, kw, jn_options, src);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_link_nodes(
    hgobj gobj_,
    const char *hook,
    const char *parent_topic_name,
    json_t *parent_record,  // owned
    const char *child_topic_name,
    json_t *child_record,  // owned
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        JSON_DECREF(parent_record);
        JSON_DECREF(child_record);
        return -1;
    }
    if(!gobj->gclass->gmt.mt_link_nodes) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_link_nodes not defined",
            NULL
        );
        JSON_DECREF(parent_record);
        JSON_DECREF(child_record);
        return -1;
    }
    return gobj->gclass->gmt.mt_link_nodes(
        gobj,
        hook,
        parent_topic_name,
        parent_record,
        child_topic_name,
        child_record,
        src
    );
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_unlink_nodes(
    hgobj gobj_,
    const char *hook,
    const char *parent_topic_name,
    json_t *parent_record,  // owned
    const char *child_topic_name,
    json_t *child_record,  // owned
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        JSON_DECREF(parent_record);
        JSON_DECREF(child_record);
        return -1;
    }
    if(!gobj->gclass->gmt.mt_unlink_nodes) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_unlink_nodes not defined",
            NULL
        );
        JSON_DECREF(parent_record);
        JSON_DECREF(child_record);
        return -1;
    }
    return gobj->gclass->gmt.mt_unlink_nodes(
        gobj,
        hook,
        parent_topic_name,
        parent_record,
        child_topic_name,
        child_record,
        src
    );
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_get_node(
    hgobj gobj_,
    const char *topic_name,
    json_t *kw,         // 'id' and pkey2s fields are used to find the node
    json_t *jn_options, // fkey,hook options
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        JSON_DECREF(kw);
        JSON_DECREF(jn_options);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_get_node) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_get_node not defined",
            NULL
        );
        JSON_DECREF(kw);
        JSON_DECREF(jn_options);
        return 0;
    }
    return gobj->gclass->gmt.mt_get_node(gobj, topic_name, kw, jn_options, src);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_list_nodes(
    hgobj gobj_,
    const char *topic_name,
    json_t *jn_filter,
    json_t *jn_options, // "include-instances" fkey,hook options
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        JSON_DECREF(jn_filter);
        JSON_DECREF(jn_options);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_list_nodes) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_list_nodes not defined",
            NULL
        );
        JSON_DECREF(jn_filter);
        JSON_DECREF(jn_options);
        return 0;
    }

    return gobj->gclass->gmt.mt_list_nodes(gobj, topic_name, jn_filter, jn_options, src);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_list_instances(
    hgobj gobj_,
    const char *topic_name,
    const char *pkey2_field,
    json_t *jn_filter,
    json_t *jn_options, // owned, fkey,hook options
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        JSON_DECREF(jn_filter);
        JSON_DECREF(jn_options);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_list_instances) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_list_instances not defined",
            NULL
        );

        JSON_DECREF(jn_filter);
        JSON_DECREF(jn_options);
        return 0;
    }

    return gobj->gclass->gmt.mt_list_instances(
        gobj, topic_name, pkey2_field, jn_filter, jn_options, src
    );
}

/***************************************************************************
 *  Return a list of parent **references** pointed by the link (fkey)
 *  If no link return all links
 ***************************************************************************/
PUBLIC json_t *gobj_node_parents( // Return MUST be decref
    hgobj gobj_,
    const char *topic_name,
    json_t *kw,         // 'id' and pkey2s fields are used to find the node
    const char *link,
    json_t *jn_options, // fkey options
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        JSON_DECREF(jn_options);
        JSON_DECREF(kw);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_node_parents) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_node_parents not defined",
            NULL
        );
        JSON_DECREF(jn_options);
        JSON_DECREF(kw);
        return 0;
    }
    return gobj->gclass->gmt.mt_node_parents(gobj, topic_name, kw, link, jn_options, src);
}

/***************************************************************************
 *  Return a list of child nodes of the hook
 *  If no hook return all hooks
 ***************************************************************************/
PUBLIC json_t *gobj_node_childs( // Return MUST be decref
    hgobj gobj_,
    const char *topic_name,
    json_t *kw,         // 'id' and pkey2s fields are used to find the node
    const char *hook,
    json_t *jn_filter,  // filter to childs
    json_t *jn_options, // fkey,hook options, "recursive"
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        JSON_DECREF(jn_options);
        JSON_DECREF(kw);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_node_childs) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_node_childs not defined",
            NULL
        );
        JSON_DECREF(jn_options);
        JSON_DECREF(kw);
        return 0;
    }
    return gobj->gclass->gmt.mt_node_childs(gobj, topic_name, kw, hook, jn_filter, jn_options, src);
}

/***************************************************************************
 *  Return a hierarchical tree of the self-link topic
 *  If "webix" option is true return webix style (dict-list),
 *      else list of dict's
 *  __path__ field in all records (id`id`... style)
 *  If root node is not specified then the first with no parent is used
 ***************************************************************************/
PUBLIC json_t *gobj_topic_jtree( // Return MUST be decref
    hgobj gobj_,
    const char *topic_name,
    const char *hook,   // hook to build the hierarchical tree
    const char *rename_hook, // change the hook name in the tree response
    json_t *kw,         // 'id' and pkey2s fields are used to find the root node
    json_t *jn_filter,  // filter to match records
    json_t *jn_options, // fkey,hook options
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        JSON_DECREF(jn_filter);
        JSON_DECREF(jn_options);
        KW_DECREF(kw);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_topic_jtree) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_topic_jtree not defined",
            NULL
        );
        JSON_DECREF(jn_filter);
        JSON_DECREF(jn_options);
        KW_DECREF(kw);
        return 0;
    }
    return gobj->gclass->gmt.mt_topic_jtree(
        gobj,
        topic_name,
        hook,   // hook to build the hierarchical tree
        rename_hook,// change the hook name in the tree response
        kw,         // 'id' and pkey2s fields are used to find the node
        jn_filter,  // filter to match records
        jn_options, // fkey,hook options
        src
    );
}

/***************************************************************************
 *  Return the full tree of a node (duplicated)
 ***************************************************************************/
PUBLIC json_t *gobj_node_tree( // Return MUST be decref
    hgobj gobj_,
    const char *topic_name,
    json_t *kw,         // 'id' and pkey2s fields are used to find the root node
    json_t *jn_options, // "with_metatada"
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        JSON_DECREF(jn_options);
        KW_DECREF(kw);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_node_tree) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_node_tree not defined",
            NULL
        );
        JSON_DECREF(jn_options);
        KW_DECREF(kw);
        return 0;
    }
    return gobj->gclass->gmt.mt_node_tree(
        gobj,
        topic_name,
        kw,         // 'id' and pkey2s fields are used to find the node
        jn_options,
        src
    );
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_shoot_snap(
    hgobj gobj_,
    const char *tag,
    json_t *kw,
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        return -1;
    }
    if(!gobj->gclass->gmt.mt_shoot_snap) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_shoot_snap not defined",
            NULL
        );
        KW_DECREF(kw);
        return -1;
    }
    return gobj->gclass->gmt.mt_shoot_snap(gobj, tag, kw, src);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_activate_snap(
    hgobj gobj_,
    const char *tag,
    json_t *kw,
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        return -1;
    }
    if(!gobj->gclass->gmt.mt_activate_snap) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_activate_snap not defined",
            NULL
        );
        KW_DECREF(kw);
        return -1;
    }
    return gobj->gclass->gmt.mt_activate_snap(gobj, tag, kw, src);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_list_snaps(
    hgobj gobj_,
    json_t *filter,
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(filter);
        return 0;
    }
    if(!gobj->gclass->gmt.mt_list_snaps) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "mt_list_snaps not defined",
            NULL
        );
        KW_DECREF(filter);
        return 0;
    }
    return gobj->gclass->gmt.mt_list_snaps(gobj, filter, src);
}




                    /*---------------------------------*
                     *  SECTION: Operational functions
                     *---------------------------------*/




/***************************************************************************
 *  Execute internal method
 ***************************************************************************/
PUBLIC json_t *gobj_exec_internal_method(
    hgobj gobj_,
    const char *lmethod,
    json_t *kw,
    hgobj src
)
{
    GObj_t * gobj = gobj_;
    register const LMETHOD *lmt;

    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        return 0;
    }

    lmt = gobj->gclass->lmt;
    while(lmt && lmt->lname != 0) {
        if(strncasecmp(lmt->lname, lmethod, strlen(lmt->lname))==0) {
            if(lmt->lm) {
                return (*lmt->lm)(gobj, lmethod, kw, src);
            }
        }
        lmt++;
    }

    log_error(0,
        "gobj",         "%s", __FILE__,
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_INTERNAL_ERROR,
        "msg",          "%s", "internal method NOT EXIST",
        "full-name",    "%s", gobj_full_name(gobj),
        "lmethod",      "%s", lmethod,
        NULL
    );
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC BOOL gobj_check_required_attrs(
    GObj_t *gobj,
    not_found_cb_t not_found_cb // Called when the key not exist in hsdata
)
{
    const char **keys = sdata_keys(gobj_hsdata(gobj), SDF_REQUIRED, 0);
    if(keys) {
        while(*keys) {
            const char *name = *keys;
            hsdata hs = gobj_hsdata2(gobj, name, FALSE);
            if(!gobj_has_attr(gobj, *keys)) {
                if(not_found_cb) {
                    not_found_cb(gobj, *keys);
                }
                gbmem_free(keys);
                return FALSE;
            }

            const sdata_desc_t *it;
            void *ptr = sdata_it_pointer(hs, name, &it);

            SData_Value_t value = sdata_read_by_type(hs, it, ptr);
            if(!sdata_attr_with_value(it, value)) {
                if(not_found_cb) {
                    not_found_cb(gobj, *keys);
                }
                gbmem_free(keys);
                return FALSE;
            }

            keys++;
        };
        gbmem_free(keys);

        return TRUE;
    }

    return TRUE;
}

/***************************************************************************
 *  Execute global method "start" of the gobj
 ***************************************************************************/
PUBLIC int gobj_start(hgobj gobj_)
{
    GObj_t * gobj = gobj_;

    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }
    if(gobj->running) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
            "msg",          "%s", "GObj ALREADY RUNNING",
            NULL
        );
        return -1;
    }
    if(gobj->disabled) {
        log_warning(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
            "msg",          "%s", "GObj DISABLED",
            NULL
        );
        return -1;
    }

    // WARNING changed in v 4.9.3 (efecto colateral?)
    hgobj gobj_bottom = gobj_bottom_gobj(gobj);
    if(gobj_bottom) {
        if(!gobj_is_running(gobj_bottom) && !gobj_is_disabled(gobj_bottom)) {
            if(!((gobj_gclass(gobj_bottom))->gcflag & gcflag_manual_start)) {
                gobj_start(gobj_bottom);
            }
        }
    }

    /*
     *  Check required attributes.
     */
    if(!gobj_check_required_attrs(gobj, print_required_attr_not_found)) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
            "msg",          "%s", "Cannot start without all required attributes",
            NULL
        );
        return -1;
    }

    if(__trace_gobj_start_stop__(gobj)) {
        trace_machine("⏺ ⏺ start: %s",
            gobj_full_name(gobj)
        );
    }
    if(__trace_gobj_monitor__(gobj)) {
        monitor_gobj(MTOR_GOBJ_START, gobj);
    }

    gobj->running = TRUE;

    int ret = 0;
    if(gobj->gclass->gmt.mt_start) {
        ret = gobj->gclass->gmt.mt_start(gobj);
    }
    return ret;
}

/***************************************************************************
 *  Start all childs of the gobj.
 ***************************************************************************/
PRIVATE int cb_start_child(rc_instance_t *i_child, hgobj child, void *user_data, void *user_data2, void *user_data3)
{
    if((((GObj_t *)child)->gclass->gcflag & gcflag_manual_start)) {
        return 0;
    }
    if(!gobj_is_running(child) && !gobj_is_disabled(child)) {
        gobj_start(child);
    }
    return 0;
}
PUBLIC int gobj_start_childs(hgobj gobj)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }

    return gobj_walk_gobj_childs(gobj, WALK_FIRST2LAST, cb_start_child, 0, 0, 0);
}

/***************************************************************************
 *  Start this gobj and all childs tree of the gobj.
 ***************************************************************************/
PRIVATE int cb_start_child_tree(rc_instance_t *i_child, hgobj child, void *user_data, void *user_data2, void *user_data3)
{
    if((((GObj_t *)child)->gclass->gcflag & gcflag_manual_start)) {
        return 1;
    }
    if(gobj_is_disabled(child)) {
        return 1;
    }
    if(!gobj_is_running(child)) {
        gobj_start(child);
    }
    return 0;
}
PUBLIC int gobj_start_tree(hgobj gobj_)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }
    if(__trace_gobj_start_stop__(gobj)) {
        trace_machine("⏺ ⏺ ⏺ ⏺ start_tree: %s",
            gobj_full_name(gobj)
        );
    }

    if((gobj->gclass->gcflag & gcflag_manual_start)) {
        return 0;
    } else if(gobj->disabled) {
        return 0;
    } else {
        if(!gobj->running) {
            gobj_start(gobj);
        }
    }
    return gobj_walk_gobj_childs_tree(gobj, WALK_TOP2BOTTOM, cb_start_child_tree, 0, 0, 0);
}

/***************************************************************************
 *  Execute global method "stop" of the gobj
 ***************************************************************************/
PUBLIC int gobj_stop(hgobj gobj_)
{
    GObj_t * gobj = gobj_;

    if(!gobj) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }
    if(gobj->obflag & obflag_destroying) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_short_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj destroying",
            NULL
        );
        return -1;
    }
    if(!gobj->running) {
        if(!gobj_is_shutdowning()) {
            log_error(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", gobj_full_name(gobj),
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
                "msg",          "%s", "GObj NOT RUNNING",
                NULL
            );
        }
        return -1;
    }
    if(gobj->playing) {
        // It's auto-stopping but display error (magic but warn!).
        log_info(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
            "msg",          "%s", "GObj stopping without previous pause",
            NULL
        );
        gobj_pause(gobj);
    }

    if(__trace_gobj_start_stop__(gobj)) {
        trace_machine("⏹ ⏹ stop: %s",
            gobj_full_name(gobj)
        );
    }
    if(__trace_gobj_monitor__(gobj)) {
        monitor_gobj(MTOR_GOBJ_STOP, gobj);
    }

    gobj->running = FALSE;

    int ret = 0;
    if(gobj->gclass->gmt.mt_stop) {
        ret = gobj->gclass->gmt.mt_stop(gobj);
    }

    return ret;
}

/***************************************************************************
 *  Stop all childs of the gobj.
 ***************************************************************************/
PRIVATE int cb_stop_child(rc_instance_t *i_child, hgobj child, void *user_data, void *user_data2, void *user_data3)
{
    if(gobj_is_running(child)) {
        gobj_stop(child);
    }
    return 0;
}
PUBLIC int gobj_stop_childs(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }
    if(gobj->obflag & obflag_destroying) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj destroying",
            NULL
        );
        return -1;
    }
    return gobj_walk_gobj_childs(gobj, WALK_FIRST2LAST, cb_stop_child, 0, 0, 0);
}

/***************************************************************************
 *  Stop this gobj and all childs tree of the gobj.
 ***************************************************************************/
PUBLIC int gobj_stop_tree(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }
    if(gobj->obflag & obflag_destroying) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj destroying",
            NULL
        );
        return -1;
    }
    if(__trace_gobj_start_stop__(gobj)) {
        trace_machine("⏹ ⏹ ⏹ ⏹ stop_tree: %s",
            gobj_full_name(gobj)
        );
    }

    if(gobj_is_running(gobj)) {
        gobj_stop(gobj);
    }
    return gobj_walk_gobj_childs_tree(gobj, WALK_TOP2BOTTOM, cb_stop_child, 0, 0, 0);
}

/***************************************************************************
 *  Execute global method "play" of the gobj
 ***************************************************************************/
PUBLIC int gobj_play(hgobj gobj_)
{
    GObj_t * gobj = gobj_;

    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }
    if(gobj->obflag & obflag_destroying) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj destroying",
            NULL
        );
        return -1;
    }
    if(gobj->playing) {
        log_warning(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
            "msg",          "%s", "GObj ALREADY PLAYING",
            NULL
        );
        return -1;
    }
    if(gobj->disabled) {
        log_warning(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
            "msg",          "%s", "GObj DISABLED",
            NULL
        );
        return -1;
    }
    if(!gobj_is_running(gobj)) {
        if(!(gobj->gclass->gcflag & gcflag_required_start_to_play)) {
            // Default: It's auto-starting but display error (magic but warn!).
            log_error(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", gobj_full_name(gobj),
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
                "msg",          "%s", "GObj playing without previous start",
                NULL
            );
            gobj_start(gobj);
        } else {
            log_error(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", gobj_full_name(gobj),
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
                "msg",          "%s", "Cannot play, start not done",
                NULL
            );
            return -1;
        }
    }

    if(__trace_gobj_start_stop__(gobj)) {
        if(!is_machine_not_tracing(gobj)) {
            trace_machine("⏯ ⏯ play: %s",
                gobj_full_name(gobj)
            );
        }
    }
    if(__trace_gobj_monitor__(gobj)) {
        monitor_gobj(MTOR_GOBJ_PLAY, gobj);
    }
    gobj->playing = TRUE;

    if(gobj->gclass->gmt.mt_play) {
        int ret = gobj->gclass->gmt.mt_play(gobj);
        if(ret < 0) {
            gobj->playing = FALSE;
        }
        return ret;
    } else {
        return 0;
    }
}

/***************************************************************************
 *  Execute global method "pause" of the gobj
 ***************************************************************************/
PUBLIC int gobj_pause(hgobj gobj_)
{
    GObj_t * gobj = gobj_;

    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }
    if(gobj->obflag & obflag_destroying) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj destroying",
            NULL
        );
        return -1;
    }
    if(!gobj->playing) {
        log_info(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
            "msg",          "%s", "GObj NOT PLAYING",
            NULL
        );
        return -1;
    }

    if(__trace_gobj_start_stop__(gobj)) {
        if(!is_machine_not_tracing(gobj)) {
            trace_machine("⏸ ⏸ pause: %s",
                gobj_full_name(gobj)
            );
        }
    }
    if(__trace_gobj_monitor__(gobj)) {
        monitor_gobj(MTOR_GOBJ_PAUSE, gobj);
    }
    gobj->playing = FALSE;

    if(gobj->gclass->gmt.mt_pause) {
        return gobj->gclass->gmt.mt_pause(gobj);
    } else {
        return 0;
    }
}

/***************************************************************************
 *  If service has mt_play then start only the service gobj.
 *      (Let mt_play be responsible to start their tree)
 *  If service has not mt_play then start the tree with gobj_start_tree().
 ***************************************************************************/
PUBLIC int gobj_autostart_services(void)
{
    service_register_t *srv_reg = dl_first(&dl_service);
    while(srv_reg) {
        if(srv_reg->gobj->obflag & obflag_yuno) {
            srv_reg = dl_next(srv_reg);
            continue;
        }
        if(srv_reg->gobj) {
            if(srv_reg->gobj->obflag & obflag_autostart) {
                if(srv_reg->gobj->gclass->gmt.mt_play) { // HACK checking mt_play because if exists he have the power on!
                    gobj_start(srv_reg->gobj);
                } else {
                    gobj_start_tree(srv_reg->gobj);
                }
            }

        }
        srv_reg = dl_next(srv_reg);
    }
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_autoplay_services(void)
{
    service_register_t *srv_reg = dl_first(&dl_service);
    while(srv_reg) {
        if(srv_reg->gobj->obflag & obflag_yuno) {
            srv_reg = dl_next(srv_reg);
            continue;
        }
        if(srv_reg->gobj) {
            if(srv_reg->gobj->obflag & obflag_autoplay) {
                gobj_play(srv_reg->gobj);
            }
        }
        srv_reg = dl_next(srv_reg);
    }
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_stop_services(void)
{
    service_register_t *srv_reg = dl_first(&dl_service);
    while(srv_reg) {
        if(srv_reg->gobj->obflag & obflag_yuno) {
            srv_reg = dl_next(srv_reg);
            continue;
        }
        if(srv_reg->gobj) {
            log_debug(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_STARTUP,
                "msg",          "%s", "STOP service",
                "service",      "%s", srv_reg->service,
                "pid",          "%d", getpid(),
                NULL
            );
            if(gobj_is_playing(srv_reg->gobj)) {
                gobj_pause(srv_reg->gobj);
            }
            gobj_stop_tree(srv_reg->gobj);
        }
        srv_reg = dl_next(srv_reg);
    }
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_disable(hgobj gobj_)
{
    GObj_t * gobj = gobj_;

    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }
    if(gobj->disabled) {
        log_info(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
            "msg",          "%s", "GObj ALREADY disabled",
            NULL
        );
        return -1;
    }
    gobj->disabled = TRUE;
    if(gobj->gclass->gmt.mt_disable) {
        return gobj->gclass->gmt.mt_disable(gobj);
    } else {
        return gobj_stop_tree(gobj);
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_enable(hgobj gobj_)
{
    GObj_t * gobj = gobj_;

    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }
    if(!gobj->disabled) {
        log_info(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_OPERATIONAL_ERROR,
            "msg",          "%s", "GObj NOT disabled",
            NULL
        );
        return -1;
    }
    gobj->disabled = FALSE;
    if(gobj->gclass->gmt.mt_enable) {
        return gobj->gclass->gmt.mt_enable(gobj);
    } else {
        return gobj_start_tree(gobj);
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC void gobj_set_yuno_must_die(void)
{
    __yuno_must_die__ = TRUE;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC BOOL gobj_get_yuno_must_die(void)
{
    return __yuno_must_die__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC void gobj_set_exit_code(int exit_code)
{
    __exit_code__ = exit_code;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_get_exit_code(void)
{
    return __exit_code__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC hgobj gobj_default_service(void)
{
    return __default_service__;
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE service_register_t * _find_service(const char *service)
{
    if(empty_string(service)) {
        return 0;
    }
    service_register_t *srv_reg = dl_first(&dl_service);
    while(srv_reg) {
        if(srv_reg->service) {
            if(strcasecmp(srv_reg->service, service)==0) {
                return srv_reg;
            }
        }
        srv_reg = dl_next(srv_reg);
    }
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC hgobj gobj_find_service(const char *service, BOOL verbose)
{
    if(empty_string(service)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "service EMPTY",
            NULL
        );
        return 0;
    }
    /*
     *  WARNING code repeated
     */
    if(strcasecmp(service, "__default_service__")==0) {
        return __default_service__;
    }
    if(strcasecmp(service, "__yuno__")==0 || strcasecmp(service, "__root__")==0) {
        return gobj_yuno();
    }
    service_register_t *srv_reg = _find_service(service);
    if(srv_reg) {
        if(!srv_reg->gobj) {
            return 0;
        }
        return srv_reg->gobj;
    }
    if(verbose) {
        char temp[250];
        snprintf(temp, sizeof(temp), "service NOT FOUND: %s", service);
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", temp,
            "service",      "%s", service,
            NULL
        );
    }
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC hgobj gobj_find_gclass_service(const char *gclass_name, BOOL verbose)
{
    if(empty_string(gclass_name)) {
        return 0;
    }

    service_register_t *srv_reg = dl_first(&dl_service);
    while(srv_reg) {
        if(srv_reg->service) {
            if(srv_reg->gobj &&
                    strcasecmp(srv_reg->gobj->gclass->gclass_name, gclass_name)==0) {
                return srv_reg->gobj;
            }
        }
        srv_reg = dl_next(srv_reg);
    }

    if(verbose) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "service of gclass NOT FOUND",
            "gclass",       "%s", gclass_name,
            NULL
        );
    }
    return 0;
}

/***************************************************************************
 *  Return nearest (parent) top service (service or __yuno__) gobj
 ***************************************************************************/
PUBLIC hgobj gobj_nearest_top_service(hgobj gobj_)
{
    GObj_t *gobj = gobj_;

    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        return 0;
    }
    gobj = gobj->__parent__;
    while(gobj) {
        if(gobj->obflag & (obflag_service|obflag_yuno)) {
            break;
        }
        gobj = gobj->__parent__;
    }
    return gobj;
}

/***************************************************************************
 *  Return nearest (parent) top unique (or service) gobj
 ***************************************************************************/
PUBLIC hgobj gobj_nearest_top_unique(hgobj gobj_)
{
    GObj_t *gobj = gobj_;

    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        return 0;
    }
    gobj = gobj->__parent__;
    while(gobj) {
        if(gobj->obflag & (obflag_unique_name|obflag_service|obflag_yuno)) {
            break;
        }
        gobj = gobj->__parent__;
    }
    return gobj;
}

/***************************************************************************
 *  path relative to gobj, including the attribute
 ***************************************************************************/
PUBLIC hgobj gobj_find_tree_path(hgobj gobj, const char *path)
{
    log_error(0,
        "gobj",         "%s", "__yuno__",
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_INTERNAL_ERROR,
        "msg",          "%s", "gobj_find_tree_path() NOT IMPLEMENTED",
        NULL
    );
    return 0; // TODO
}

/***************************************************************************
 *  Return the object searched by path.
 ***************************************************************************/
PRIVATE BOOL its_me(GObj_t *gobj, char *shortname)
{
    const char *gobj_name_ = strchr(shortname, '^');
    if(gobj_name_) {
        int gclass_len = gobj_name_ - shortname;
        if(strncmp(shortname, gobj_gclass_name(gobj), gclass_len)!=0) {
            return FALSE;
        }
        gobj_name_++;
    } else {
        gobj_name_ = shortname;
    }
    if(strcasecmp(gobj_name_, "__default_service__")==0) {
        if(gobj == __default_service__) {
            return TRUE;
        }
    }
    if(strcasecmp(gobj_name_, "__yuno__")==0 || strcasecmp(gobj_name_, "__root__")==0) {
        if(gobj == __yuno__) {
            return TRUE;
        }
    }
    if(strcmp(gobj_name_, gobj_name(gobj))==0) {
        return TRUE;
    }
    return FALSE;
}

/***************************************************************************
 *  Return the object searched by path.
 *  The separator of tree's gobj must be '`'
 ***************************************************************************/
PRIVATE hgobj _gobj_search_path(GObj_t *gobj, const char *path)
{
    if(!path) {
        return 0;
    }

    /*
     *  Get node and compare with this
     */
    const char *p = strchr(path, '`');

    char temp[120];
    if(p) {
        snprintf(temp, sizeof(temp), "%.*s", (int)(p-path), path);
    } else {
        snprintf(temp, sizeof(temp), "%s", path);
    }
    if(!its_me(gobj, temp)) {
        return 0;
    }
    if(!p) {
        // No more nodes
        return gobj;
    }

    /*
     *  Get next node and compare with childs
     */
    p++;
    const char *n = p;
    p = strchr(n, '`');

    /*
     *  Search in childs
     */
    if(p) {
        snprintf(temp, sizeof(temp), "%.*s", (int)(p-n), n);
    } else {
        snprintf(temp, sizeof(temp), "%s", n);
    }
    char *gclass_name_ = 0;
    char *gobj_name_ = strchr(temp, '^');
    if(gobj_name_) {
        gclass_name_ = temp;
        *gobj_name_ = 0;
        gobj_name_++;
    } else {
        gobj_name_ = temp;
    }

    json_t *jn_filter = json_pack("{s:s}",
        "__gobj_name__", gobj_name_
    );
    if(gclass_name_) {
        json_object_set_new(jn_filter, "__gclass_name__", json_string(gclass_name_));
    }

    hgobj child = gobj_find_child(gobj, jn_filter);
    if(!child) {
        return 0;
    }
    return _gobj_search_path(child, n);
}

/***************************************************************************
 *  Return the object searched by oid.
 *  The separator of tree's gobj must be '`'
 ***************************************************************************/
PRIVATE hgobj _gobj_search_oid(GObj_t *gobj, const char *path)
{
    if(!path) {
        return 0;
    }

    /*
     *  Get node and compare with this
     */
    const char *p = strchr(path, '`');

    char temp[120];
    if(p) {
        snprintf(temp, sizeof(temp), "%.*s", (int)(p-path), path);
    } else {
        snprintf(temp, sizeof(temp), "%s", path);
    }
    size_t idx = atol(temp);
    if(idx != gobj->oid) {
        return 0;
    }
    if(!p) {
        // No more nodes
        return gobj;
    }

    /*
     *  Get next node and compare with childs
     */
    p++;
    const char *n = p;
    p = strchr(n, '`');

    /*
     *  Search in childs
     */
    if(p) {
        snprintf(temp, sizeof(temp), "%.*s", (int)(p-n), n);
    } else {
        snprintf(temp, sizeof(temp), "%s", n);
    }
    idx = atol(temp);

    hgobj child;
    if(!gobj_child_by_index(gobj, idx, &child)) {
        return 0;
    }
    return _gobj_search_oid(child, n);
}

/***************************************************************************
 *  Find a gobj by path
 ***************************************************************************/
PUBLIC hgobj gobj_find_gobj(const char *path)
{
    if(empty_string(path)) {
        return 0;
    }
    /*
     *  WARNING code repeated
     */
    if(strcasecmp(path, "__default_service__")==0) {
        return __default_service__;
    }
    if(strcasecmp(path, "__yuno__")==0 || strcasecmp(path, "__root__")==0) {
        return __yuno__;
    }

    if(path[0]=='1') {
        return _gobj_search_oid(__yuno__, path);
    } else {
        return _gobj_search_path(__yuno__, path);
    }
}

/***************************************************************************
 *  Find a unique named gobj
 ***************************************************************************/
PUBLIC hgobj gobj_find_unique_gobj(const char *unique_name, BOOL verbose)
{
    hgobj gobj_found;

    if(empty_string(unique_name)) {
        if(verbose) {
            log_error(0,
                "gobj",         "%s", "__yuno__",
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "unique_name EMPTY",
                NULL
            );
        }
        return 0;
    }

    /*
     *  WARNING code repeated
     */
    if(strcasecmp(unique_name, "__default_service__")==0) {
        return __default_service__;
    }
    if(strcasecmp(unique_name, "__yuno__")==0 || strcasecmp(unique_name, "__root__")==0) {
        return __yuno__;
    }

    json_t *o = json_object_get(__jn_unique_named_dict__, unique_name);
    if(!o) {
        if(verbose) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "unique_name NOT FOUND",
                "unique_name",  "%s", unique_name,
                NULL
            );
        }
        return 0;
    }
    gobj_found = (hgobj)(size_t)json_integer_value(o);
    return gobj_found;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC rc_instance_t * gobj_first_child(hgobj gobj_, hgobj *child_)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }

    GObj_t * child; rc_instance_t *i_child;
    i_child = rc_first_instance(&gobj->dl_childs, (rc_resource_t **)&child);
    if(child_) {
        *child_ = (hgobj)child;
    }
    return i_child;
}

/***************************************************************************
 *  Return last child, last to `child`
 ***************************************************************************/
PUBLIC rc_instance_t * gobj_last_child(hgobj gobj_, hgobj *child_)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }

    GObj_t * child; rc_instance_t *i_child;
    i_child = rc_last_instance(&gobj->dl_childs, (rc_resource_t **)&child);
    if(child_) {
        *child_ = (hgobj)child;
    }
    return i_child;
}


/***************************************************************************
 *  Return next child, next to `child`
 ***************************************************************************/
PUBLIC rc_instance_t * gobj_next_child(rc_instance_t *iter, hgobj *child_)
{
    GObj_t * child; rc_instance_t *i_child;
    i_child = rc_next_instance(iter, (rc_resource_t **)&child);
    if(child_) {
        *child_ = (hgobj)child;
    }
    return i_child;
}

/***************************************************************************
 *  Return prev child, prev to `child`
 ***************************************************************************/
PUBLIC rc_instance_t * gobj_prev_child(rc_instance_t *iter, hgobj *child_)
{
    GObj_t * child; rc_instance_t *i_child;
    i_child = rc_prev_instance(iter, (rc_resource_t **)&child);
    if(child_) {
        *child_ = (hgobj)child;
    }
    return i_child;
}

/***************************************************************************
 *  Return the child of gobj in the `index` position. (relative to 1)
 ***************************************************************************/
PUBLIC rc_instance_t * gobj_child_by_index(hgobj gobj, size_t index, hgobj *child_)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        if(child_) {
            *child_ = 0;
        }
        return 0;
    }

    GObj_t * child; rc_instance_t *i_child;
    i_child = rc_child_nfind(gobj, index, (rc_resource_t **)&child);
    if(i_child) {
        if(child_) {
            *child_ = (hgobj)child;
        }
        return i_child;
    }

    if(child_) {
        *child_ = 0;
    }
    return 0;
}

/***************************************************************************
 *  Return the child index in the parent list
 ***************************************************************************/
PUBLIC size_t  gobj_child_index(hgobj parent, hgobj child)
{
    if(!parent) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }
    if(!child) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }
    size_t index;
    rc_child_index(parent, child, &index);

    return index;
}

/***************************************************************************
 *  Return the child of gobj by name.
 *  The first found is returned.
 ***************************************************************************/
PUBLIC hgobj gobj_child_by_name(hgobj gobj, const char *name, rc_instance_t **i_child_)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }
    if(empty_string(name)) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "name NULL",
            NULL
        );
        if(i_child_) {
            *i_child_ = 0;
        }
        return 0;
    }

    hgobj child; rc_instance_t *i_child;
    i_child = gobj_first_child(gobj, &child);

    while(i_child) {
        const char *name_ = gobj_name(child);
        if(name_ && strcmp(name_, name)==0) {
            if(i_child_) {
                *i_child_ = i_child;
            }
            return child;
        }

        i_child = gobj_next_child(i_child, &child);
    }
    if(i_child_) {
        *i_child_ = 0;
    }
    return 0;
}

/***************************************************************************
 *  Return the size of childs of gobj
 ***************************************************************************/
PUBLIC size_t gobj_child_size(hgobj gobj_)
{
    GObj_t * gobj = gobj_;

    return dl_size(&gobj->dl_childs);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_walk_gobj_childs(
    hgobj gobj,
    walk_type_t walk_type,
    int (*cb_walking)(rc_instance_t *i_gobj, hgobj gobj, void *user_data, void *user_data2, void *user_data3),
    void *user_data,
    void *user_data2,
    void *user_data3)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }

    return rc_walk_by_childs(
        (rc_resource_t *)gobj,
        walk_type,
        (cb_walking_t)cb_walking,
        user_data,
        user_data2,
        user_data3
    );
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_walk_gobj_childs_tree(
    hgobj gobj,
    walk_type_t walk_type,
    int (*cb_walking)(rc_instance_t *i_gobj, hgobj gobj, void *user_data, void *user_data2, void *user_data3),
    void *user_data,
    void *user_data2,
    void *user_data3)
{
    if(!gobj) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }
    return rc_walk_by_childs_tree(
        (rc_resource_t *)gobj,
        walk_type,
        (cb_walking_t)cb_walking,
        user_data,
        user_data2,
        user_data3
    );
}

/***************************************************************************
 *  Match child.
 ***************************************************************************/
PRIVATE BOOL match_child(
    GObj_t *child,
    json_t *jn_filter_  // NOT owned
)
{
    const char *__inherited_gclass_name__ = kw_get_str(jn_filter_, "__inherited_gclass_name__", 0, 0);
    const char *__gclass_name__ = kw_get_str(jn_filter_, "__gclass_name__", 0, 0);
    const char *__gobj_name__ = kw_get_str(jn_filter_, "__gobj_name__", 0, 0);
    const char *__prefix_gobj_name__ = kw_get_str(jn_filter_, "__prefix_gobj_name__", 0, 0);
    const char *__state__ = kw_get_str(jn_filter_, "__state__", 0, 0);

    /*
     *  Delete the system keys of the jn_filter used in find loop
     */
    json_t *jn_filter = json_deep_copy(jn_filter_);
    if(__inherited_gclass_name__) {
        json_object_del(jn_filter, "__inherited_gclass_name__");
    }
    if(__gclass_name__) {
        json_object_del(jn_filter, "__gclass_name__");
    }
    if(__gobj_name__) {
        json_object_del(jn_filter, "__gobj_name__");
    }
    if(__prefix_gobj_name__) {
        json_object_del(jn_filter, "__prefix_gobj_name__");
    }
    if(__state__) {
        json_object_del(jn_filter, "__state__");
    }

    if(kw_has_key(jn_filter_, "__disabled__")) {
        BOOL disabled = kw_get_bool(jn_filter_, "__disabled__", 0, 0);
        if(disabled && !gobj_is_disabled(child)) {
            json_decref(jn_filter); // clone
            return FALSE;
        }
        if(!disabled && gobj_is_disabled(child)) {
            json_decref(jn_filter); // clone
            return FALSE;
        }
        json_object_del(jn_filter, "__disabled__");
    }


    if(!empty_string(__inherited_gclass_name__)) {
        if(!gobj_typeof_inherited_gclass(child, __inherited_gclass_name__)) {
            json_decref(jn_filter); // clone
            return FALSE;
        }
    }
    if(!empty_string(__gclass_name__)) {
        if(!gobj_typeof_gclass(child, __gclass_name__)) {
            json_decref(jn_filter); // clone
            return FALSE;
        }
    }
    const char *child_name = gobj_name(child);
    if(!empty_string(__gobj_name__)) {
        if(strcmp(__gobj_name__, child_name)!=0) {
            json_decref(jn_filter); // clone
            return FALSE;
        }
    }
    if(!empty_string(__prefix_gobj_name__)) {
        if(strncmp(__prefix_gobj_name__, child_name, strlen(__prefix_gobj_name__))!=0) {
            json_decref(jn_filter); // clone
            return FALSE;
        }
    }
    if(!empty_string(__state__)) {
        if(strcasecmp(__state__, gobj_current_state(child))!=0) {
            json_decref(jn_filter); // clone
            return FALSE;
        }
    }

    const char *key;
    json_t *jn_value;

    BOOL matched = TRUE;
    json_object_foreach(jn_filter, key, jn_value) {
        hsdata hs = gobj_hsdata2(child, key, FALSE);
        if(hs) {
            json_t *jn_var1 = item2json(hs, key, 0, 0);
            int cmp = cmp_two_simple_json(jn_var1, jn_value);
            JSON_DECREF(jn_var1);
            if(cmp!=0) {
                matched = FALSE;
                break;
            }
        }
    }

    json_decref(jn_filter); // clone
    return matched;
}

/***************************************************************************
 *  Returns the first matched child.
 ***************************************************************************/
PUBLIC hgobj gobj_find_child(
    hgobj gobj,
    json_t *jn_filter  // not owned
)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        JSON_DECREF(jn_filter);
        return 0;
    }

    GObj_t * child; rc_instance_t *i_child;
    i_child = gobj_first_child(gobj, (hgobj *)&child);

    while(i_child) {
        if(match_child(child, jn_filter)) {
            JSON_DECREF(jn_filter);
            return child;
        }
        i_child = gobj_next_child(i_child, (hgobj *)&child);
    }

    JSON_DECREF(jn_filter);
    return 0;
}

/***************************************************************************
 *  Returns the first matched child of gclass in bottom line.
 ***************************************************************************/
PUBLIC hgobj gobj_find_bottom_child_by_gclass(
    hgobj gobj_,
    const char *gclass_name
)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }


    if(gobj->bottom_gobj) {
        if(gobj_typeof_gclass(gobj->bottom_gobj, gclass_name)) {
            return gobj->bottom_gobj;
        } else {
            return gobj_find_bottom_child_by_gclass(gobj->bottom_gobj, gclass_name);
        }
    }
    return 0;
}

/***************************************************************************
 *  Returns a list (iter) with all matched childs.
 *  If dl_list is null a dynamic dl_list (iter) will be created and returned,
 *  that you must free with rc_free_iter(dl_list, TRUE, 0);
 *
 *  Check ONLY first level of childs.
 ***************************************************************************/
PRIVATE int cb_match_childs(rc_instance_t *i_child, hgobj child, void *user_data, void *user_data2, void *user_data3)
{
    dl_list_t *dl_list = (dl_list_t *)user_data;
    json_t *jn_filter = (json_t *)user_data2;

    if(match_child(child, jn_filter)) {
        rc_add_instance(dl_list, child, 0);
    }
    return 0;
}
PUBLIC dl_list_t *gobj_match_childs(
    hgobj gobj,
    dl_list_t *dl_list,
    json_t *jn_filter   // owned
)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        JSON_DECREF(jn_filter);
        return 0;
    }
    dl_list = rc_init_iter(dl_list);

    gobj_walk_gobj_childs(gobj, WALK_FIRST2LAST, cb_match_childs, dl_list, jn_filter, 0);
    JSON_DECREF(jn_filter);
    return dl_list;
}

/***************************************************************************
 *  Returns a list (iter) with all matched childs.
 *  If dl_list is null a dynamic dl_list (iter) will be created and returned,
 *  that you must free with rc_free_iter(dl_list, TRUE, 0);
 *
 *  Check deep levels of childs
 ***************************************************************************/
PUBLIC dl_list_t *gobj_match_childs_tree(
    hgobj gobj,
    dl_list_t *dl_list,
    json_t *jn_filter   // owned
)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        JSON_DECREF(jn_filter);
        return 0;
    }
    dl_list = rc_init_iter(dl_list);

    gobj_walk_gobj_childs_tree(gobj, WALK_TOP2BOTTOM, cb_match_childs, dl_list, jn_filter, 0);
    JSON_DECREF(jn_filter);
    return dl_list;
}

/***************************************************************************
 *  Returns a list (iter) with all matched childs.
 *  If dl_list is null a dynamic dl_list (iter) will be created and returned,
 *  that you must free with rc_free_iter(dl_list, TRUE, 0);
 *
 *  Check ONLY first level of childs.
 ***************************************************************************/
PUBLIC dl_list_t *gobj_match_childs_by_strict_gclass(hgobj gobj, const char *gclass_name)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }

    dl_list_t *dl_list = rc_init_iter(0);

    json_t *jn_filter = json_pack("{s:s}",
        "__gclass_name__", gclass_name?gclass_name:""
    );
    gobj_match_childs(gobj, dl_list, jn_filter);
    return dl_list;
}

/***************************************************************************
 *  Returns a list (iter) with all matched childs.
 *  If dl_list is null a dynamic dl_list (iter) will be created and returned,
 *  that you must free with rc_free_iter(dl_list, TRUE, 0);
 *
 *  Check ONLY first level of childs.
 ***************************************************************************/
PUBLIC dl_list_t *gobj_match_childs_by_inherited_gclass(hgobj gobj, const char *gclass_name)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }

    dl_list_t *dl_list = rc_init_iter(0);

    json_t *jn_filter = json_pack("{s:s}",
        "__inherited_gclass_name__", gclass_name?gclass_name:""
    );
    gobj_match_childs(gobj, dl_list, jn_filter);
    return dl_list;
}

/***************************************************************************
 *  Returns a list (iter) with all matched childs.
 *  If dl_list is null a dynamic dl_list (iter) will be created and returned,
 *  that you must free with rc_free_iter(dl_list, TRUE, 0);
 *
 *  Check deep levels of childs
 ***************************************************************************/
PUBLIC dl_list_t *gobj_match_childs_tree_by_strict_gclass(hgobj gobj, const char *gclass_name)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }

    dl_list_t *dl_list = rc_init_iter(0);

    json_t *jn_filter = json_pack("{s:s}",
        "__gclass_name__", gclass_name
    );
    gobj_match_childs_tree(gobj, dl_list, jn_filter);
    return dl_list;
}

/***************************************************************************
 *  Returns a list (iter) with all matched childs with regular expression.
 *  If dl_list is null a dynamic dl_list (iter) will be created and returned,
 *  that you must free with rc_free_iter(dl_list, TRUE, 0);
 *
 *  Check ONLY first level of childs.
 ***************************************************************************/
PUBLIC dl_list_t *gobj_filter_childs_by_re_name(dl_list_t *dl_childs, const char *re_name)
{
    regex_t _re_name;
    if(regcomp(&_re_name, re_name, REG_EXTENDED | REG_NOSUB)!=0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "regcomp() FAILED",
            "re",           "%s", re_name,
            NULL
        );
        return 0;
    }
    dl_list_t *dl_list = rc_init_iter(0);

    hgobj child; rc_instance_t *i_hs;
    i_hs = rc_first_instance(dl_childs, (rc_resource_t **)&child);
    while(i_hs) {
        const char *name = gobj_name(child);
        if(regexec(&_re_name, name, 0, 0, 0)==0) {
            rc_add_instance(dl_list, child, 0);
        }

        i_hs = rc_next_instance(i_hs, (rc_resource_t **)&child);
    }
    regfree(&_re_name);
    return dl_list;
}




                    /*---------------------------------*
                     *  SECTION: Subcriptions
                     *---------------------------------*/




/***************************************************************************
 *
 ***************************************************************************/
PRIVATE hsdata _create_subscription(
    GObj_t * publisher,
    const char *event,
    json_t *kw, // not owned
    GObj_t * subscriber)
{
    hsdata subs = sdata_create(subscription_desc, 0, 0, 0, 0, 0);
    sdata_write_str(subs, "event", event);
    sdata_write_pointer(subs, "subscriber", subscriber);
    sdata_write_pointer(subs, "publisher", publisher);
    subs_flag_t subs_flag = 0;

    if(kw) {
        json_t *__config__ = kw_get_dict(kw, "__config__", 0, 0);
        json_t *__global__ = kw_get_dict(kw, "__global__", 0, 0);
        json_t *__local__ = kw_get_dict(kw, "__local__", 0, 0);
        json_t *__filter__ = kw_get_dict_value(kw, "__filter__", 0, 0);
        const char *__service__ = kw_get_str(kw, "__service__", 0, 0);

        if(__global__) { // HACK __global__ the first. __config__ can write in this area.
            json_t *kw_clone = kw_duplicate(__global__);
            sdata_write_json(subs, "__global__", kw_clone); // Incref
            kw_decref(kw_clone); // Incref above
        }
        if(__config__) {
            json_t *kw_clone = kw_duplicate(__config__);
            sdata_write_json(subs, "__config__", kw_clone); // Incref
            kw_decref(kw_clone); // Incref above
            if(kw_has_key(kw_clone, "__rename_event_name__")) {
                const char *renamed_event = kw_get_str(kw_clone, "__rename_event_name__", 0, 0);
                sdata_write_str(subs, "renamed_event", renamed_event);
                json_object_del(kw_clone, "__rename_event_name__");
                subs_flag |= __rename_event_name__;

                // Get/Create __global__
                json_t *kw_global = sdata_read_json(subs, "__global__");
                if(!kw_global) {
                    kw_global = json_object();
                    sdata_write_json(subs, "__global__", kw_global);
                    kw_decref(kw_global); // Incref above
                }
                kw_set_dict_value(
                    kw_global,
                    "__original_event_name__",
                    json_string(event)
                );
            }
            if(kw_has_key(kw_clone, "__hard_subscription__")) {
                BOOL hard_subscription = kw_get_bool(kw_clone, "__hard_subscription__", 0, 0);
                json_object_del(kw_clone, "__hard_subscription__");
                if(hard_subscription) {
                    subs_flag |= __hard_subscription__;
                }
            }
            if(kw_has_key(kw_clone, "__first_shot__")) {
                BOOL first_short = kw_get_bool(kw_clone, "__first_short__", 0, 0);
                if(first_short) {
                    subs_flag |= __first_shot__;
                }
            }
            if(kw_has_key(kw_clone, "__share_kw__")) {
                BOOL share_kw = kw_get_bool(kw_clone, "__share_kw__", 0, 0);
                if(share_kw) {
                    subs_flag |= __share_kw__;
                }
            }
        }
        if(__local__) {
            json_t *kw_clone = kw_duplicate(__local__);
            sdata_write_json(subs, "__local__", kw_clone); // Incref
            kw_decref(kw_clone); // Incref above
        }
        if(__filter__) {
            json_t *kw_clone = kw_duplicate(__filter__);
            sdata_write_json(subs, "__filter__", kw_clone); // Incref
            kw_decref(kw_clone); // Incref above
        }
        if(__service__) {
            sdata_write_str(subs, "__service__", __service__);
        }
    }

    return subs;
}

/***************************************************************************
 *  Return a iter of subscriptions (sdata),
 *  filtering by matching:
 *      event,kw (__config__, __global__, __local__, __filter__),subscriber
 *  Free return with rc_free_iter(iter, TRUE, FALSE);
 ***************************************************************************/
PRIVATE dl_list_t * _find_subscription(
    dl_list_t *dl_subs,
    GObj_t * publisher,
    const char *event,
    json_t *kw, // owned
    GObj_t * subscriber,
    BOOL strict)
{
    BOOL (*_match)(json_t *, json_t *) = 0;
    if(strict) {
        _match = kw_is_identical; // WARNING don't decref anything
    } else {
        _match = kw_match_simple; // WARNING decref second parameter
    }

    dl_list_t *iter = rc_init_iter(NULL);
    json_t *__config__ = kw_get_dict(kw, "__config__", 0, 0);
    json_t *__global__ = kw_get_dict(kw, "__global__", 0, 0);
    json_t *__local__ = kw_get_dict(kw, "__local__", 0, 0);
    json_t *__filter__ = kw_get_dict_value(kw, "__filter__", 0, 0);

    hsdata subs;
    rc_instance_t *i_subs = rc_first_instance(dl_subs, (rc_resource_t **)&subs);
    while(i_subs) {
        hsdata next_subs; rc_instance_t *next_i_subs;
        next_i_subs = rc_next_instance(i_subs, (rc_resource_t **)&next_subs);

        BOOL match = TRUE;

        if(publisher) {
            GObj_t *publisher_  = sdata_read_pointer(subs, "publisher");
            if(publisher != publisher_) {
                match = FALSE;
            }
        }

        if(subscriber) {
            GObj_t *subscriber_  = sdata_read_pointer(subs, "subscriber");
            if(subscriber != subscriber_) {
                match = FALSE;
            }
        }

        if(event) {
            const char *event_ = sdata_read_str(subs, "event");
            if(!event_ || strcasecmp(event, event_)!=0) {
                match = FALSE;
            }
        }

        if(__config__) {
            json_t *kw_config = sdata_read_json(subs, "__config__");
            if(kw_config) {
                if(!strict) { // HACK decref when calling _match (kw_match_simple)
                    KW_INCREF(__config__);
                }
                if(!_match(kw_config, __config__)) {
                    match = FALSE;
                }
            } else {
                match = FALSE;
            }
        }
        if(__global__) {
            json_t *kw_global = sdata_read_json(subs, "__global__");
            if(kw_global) {
                if(!strict) { // HACK decref when calling _match (kw_match_simple)
                    KW_INCREF(__global__);
                }
                if(!_match(kw_global, __global__)) {
                    match = FALSE;
                }
            } else {
                match = FALSE;
            }
        }
        if(__local__) {
            json_t *kw_local = sdata_read_json(subs, "__local__");
            if(kw_local) {
                if(!strict) { // HACK decref when calling _match (kw_match_simple)
                    KW_INCREF(__local__);
                }
                if(!_match(kw_local, __local__)) {
                    match = FALSE;
                }
            } else {
                match = FALSE;
            }
        }
        if(__filter__) {
            json_t *jn_filter = sdata_read_json(subs, "__filter__");
            if(jn_filter) {
                if(!strict) { // HACK decref when calling _match (kw_match_simple)
                    KW_INCREF(__filter__);
                }
                if(!_match(jn_filter, __filter__)) {
                    match = FALSE;
                }
            } else {
                match = FALSE;
            }
        }

        if(match) {
            rc_add_instance(iter, subs, 0);
        }

        i_subs = next_i_subs;
        subs = next_subs;
    }

    KW_DECREF(kw);
    return iter;
}

/***************************************************************************
 *  Return the schema of subcriptions hsdata
 ***************************************************************************/
PUBLIC const sdata_desc_t *gobj_subscription_schema(void)
{
    return subscription_desc;
}

/***************************************************************************
 *  Return number of subcriptions
 ***************************************************************************/
PUBLIC size_t gobj_subscriptions_size(
    hgobj publisher_
)
{
    GObj_t * publisher = publisher_;
    return dl_size(&publisher->dl_subscriptions);
}

/***************************************************************************
 *  Return number of subcribings
 ***************************************************************************/
PUBLIC size_t gobj_subscribings_size(hgobj subscriber_)
{
    GObj_t * subscriber = subscriber_;
    return dl_size(&subscriber->dl_subscribings);
}

/***************************************************************************
 *  Return a iter of subscriptions (sdata) in the publisher gobj,
 *  filtering by matching: event,kw (__config__, __global__, __local__, __filter__),subscriber
 *  Free return with rc_free_iter(iter, TRUE, FALSE);
 ***************************************************************************/
PUBLIC dl_list_t * gobj_find_subscriptions(
    hgobj publisher_,
    const char *event,
    json_t *kw,
    hgobj subscriber)
{
    GObj_t * publisher = publisher_;
    return _find_subscription(&publisher->dl_subscriptions, publisher_, event, kw, subscriber, FALSE);
}

/***************************************************************************
 *  Return a iter of subscribings (sdata) of the subcriber gobj,
 *  filtering by matching: event,kw (__config__, __global__, __local__, __filter__), publisher
 *  Free return with rc_free_iter(iter, TRUE, FALSE);
 ***************************************************************************/
PUBLIC dl_list_t *gobj_find_subscribings(
    hgobj subscriber_,
    const char *event,
    json_t *kw,             // kw (__config__, __global__, __local__, __filter__)
    hgobj publisher
)
{
    GObj_t * subscriber = subscriber_;
    return _find_subscription(&subscriber->dl_subscribings, publisher, event, kw, subscriber, FALSE);
}

/***************************************************************************
 *  Delete subscription
 ***************************************************************************/
PRIVATE int _delete_subscription(hsdata subs, BOOL force, BOOL not_inform)
{
    GObj_t * publisher = sdata_read_pointer(subs, "publisher");
    GObj_t * subscriber = sdata_read_pointer(subs, "subscriber");
    const char *event = sdata_read_str(subs, "event");
    BOOL hard_subscription = (sdata_read_uint64(subs, "subs_flag") & __hard_subscription__)?1:0;

    /*-------------------------------*
     *  Check if hard subscription
     *-------------------------------*/
    if(hard_subscription) {
        if(!force) {
            return -1;
        }
    }

    /*-----------------------------*
     *  Trace
     *-----------------------------*/
    if(__trace_gobj_subscriptions__(subscriber) || __trace_gobj_subscriptions__(publisher) ||
        __trace_gobj_subscriptions2__(subscriber) || __trace_gobj_subscriptions2__(publisher)
    ) {
        trace_machine("💜💜👎 %s unsubscribing event '%s' of %s",
            gobj_full_name(subscriber),
            event,
            gobj_full_name(publisher)
        );

        if(1 || __trace_gobj_ev_kw__(subscriber) || __trace_gobj_ev_kw__(publisher)) {
            json_t *kw_config = sdata_read_json(subs, "__config__");
            json_t *kw_global = sdata_read_json(subs, "__global__");
            json_t *kw_local = sdata_read_json(subs, "__local__");
            json_t *jn_filter = sdata_read_json(subs, "__filter__");
            json_t *kww = json_object();
            if(kw_config) {
                json_object_set(kww, "__config__", kw_config);
            }
            if(kw_global) {
                json_object_set(kww, "__global__", kw_global);
            }
            if(kw_local) {
                json_object_set(kww, "__local__", kw_local);
            }
            if(jn_filter) {
                json_object_set(kww, "__filter__", jn_filter);
            }
            log_debug_json(0, kww, "kww");
            json_decref(kww);
        }
    }

    /*------------------------------------------------*
     *      Inform (BEFORE) of subscription removed
     *------------------------------------------------*/
    if(!(publisher->obflag & obflag_destroyed)) {
        if(!not_inform) {
            if(publisher->gclass->gmt.mt_subscription_deleted) {
                publisher->gclass->gmt.mt_subscription_deleted(
                    publisher,
                    subs
                );
            }
        }
    }

    /*--------------------------------*
     *      Delete subscription
     *--------------------------------*/
    rc_delete_resource(subs, sdata_destroy);

    return 0;
}

/***************************************************************************
 *  Delete all subscriptions as subscriber
 *  WARNING Use only in destroying gobj
 ***************************************************************************/
PRIVATE int _delete_subscribings(GObj_t * subscriber)
{
    hsdata subs; rc_instance_t *i_subs;

    while((i_subs=rc_first_instance(&subscriber->dl_subscribings, (rc_resource_t **)&subs))) {
        rc_delete_resource(subs, sdata_destroy);
    }
    return 0;
}

/***************************************************************************
 *  Delete all subscriptions as subscriber
 *  WARNING Use only in destroying gobj
 ***************************************************************************/
PRIVATE int _delete_subscriptions(GObj_t * publisher)
{
    hsdata subs; rc_instance_t *i_subs;

    while((i_subs=rc_first_instance(&publisher->dl_subscriptions, (rc_resource_t **)&subs))) {
        rc_delete_resource(subs, sdata_destroy);
    }
    return 0;
}

/***************************************************************************
 *  Subscribe to an event
 *
 *  event is only one event (not a string list like ginsfsm).
 *
 *  IDEMPOTENT function
 *
 *  See .h
 ***************************************************************************/
PUBLIC hsdata gobj_subscribe_event(
    hgobj publisher_,
    const char *event,
    json_t *kw,
    hgobj subscriber_)
{
    GObj_t * publisher = publisher_;
    GObj_t * subscriber = subscriber_;

    /*---------------------*
     *  Check something
     *---------------------*/
    if(!publisher) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "publisher NULL",
            "event",        "%s", event?event:"",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }
    if(!subscriber) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "subscriber NULL",
            "event",        "%s", event?event:"",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }

    /*-------------------------------------------------*
     *  Check AUTHZ
     *-------------------------------------------------*/
//     const EVENT *output_event = gobj_output_event(publisher, event);
//         if(output_event->authz & EV_AUTHZ_SUBSCRIBE) {
//             const char *event = output_event->event?output_event->event:"";
//             /*
//              *  AUTHZ Required
//              */
//             json_t *kw_authz = json_pack("{s:s}",
//                 "event", event
//             );
//             if(kw) {
//                 json_object_set(kw_authz, "kw", kw);
//             } else {
//                 json_object_set_new(kw_authz, "kw", json_object());
//             }
//             if(!gobj_user_has_authz(
//                 publisher_,
//                 "__subscribe_event__",
//                 kw_authz,
//                 subscriber_
//             )) {
//                 log_error(0,
//                     "gobj",         "%s", gobj_full_name(publisher_),
//                     "function",     "%s", __FUNCTION__,
//                     "msgset",       "%s", MSGSET_OAUTH_ERROR,
//                     "msg",          "%s", "No permission to subscribe event",
//                     //"user",         "%s", gobj_get_user(subscriber_),
//                     "gclass",       "%s", gobj_gclass_name(publisher_),
//                     "event",        "%s", event?event:"",
//                     NULL
//                 );
//                 KW_DECREF(kw);
//                 return 0;
//             }
//         }
//         output_event_list++;
//     }

    /*-------------------------------------------------*
     *
     *-------------------------------------------------*/
    if(empty_string(event) || strchr(event, '*')) {
        event = "";
    }

    /*--------------------------------------------------------------*
     *  Event must be in output event list
     *  You can avoid this with gcflag_no_check_output_events flag
     *--------------------------------------------------------------*/
    if(!empty_string(event)) {
        if(!(publisher->gclass->gcflag & gcflag_no_check_output_events)) {
            const EVENT *output_event = gobj_output_event(publisher, event);
            if(!output_event) {
                log_error(0,
                    "gobj",         "%s", __FILE__,
                    "function",     "%s", __FUNCTION__,
                    "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                    "msg",          "%s", "event NOT in output event list",
                    "event",        "%s", event,
                    "publisher",    "%s", gobj_full_name(publisher),
                    "subscriber",   "%s", gobj_full_name(subscriber),
                    NULL
                );
            }
        }
    }

    /*------------------------------*
     *  Find repeated subscription
     *------------------------------*/
    KW_INCREF(kw);
    dl_list_t *dl_subs = _find_subscription(
        &publisher->dl_subscriptions,
        publisher,
        event,
        kw,
        subscriber,
        TRUE
    );
    int size = rc_iter_size(dl_subs);
    if(size > 0) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "subscription(s) REPEATED, will be deleted and overrided",
            "event",        "%s", event,
            "publisher",    "%s", gobj_full_name(publisher),
            "subscriber",   "%s", gobj_full_name(subscriber),
            NULL
        );
        gobj_unsubscribe_list(dl_subs, FALSE, FALSE);
    }
    rc_free_iter(dl_subs, TRUE, 0);

    /*-----------------------------*
     *  Create subscription
     *-----------------------------*/
    hsdata subs = _create_subscription(
        publisher,
        event,
        kw,  // not owned
        subscriber
    );
    if(!subs) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "_create_subscription() FAILED",
            "event",        "%s", event?event:"*",
            "publisher",    "%s", gobj_full_name(publisher),
            "subscriber",   "%s", gobj_full_name(subscriber),
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }
    rc_add_instance(&publisher->dl_subscriptions, subs, 0);
    rc_add_instance(&subscriber->dl_subscribings, subs, 0);

    /*-----------------------------*
     *  Trace
     *-----------------------------*/
    if(__trace_gobj_subscriptions__(subscriber) || __trace_gobj_subscriptions__(publisher) ||
        __trace_gobj_subscriptions2__(subscriber) || __trace_gobj_subscriptions2__(publisher)
    ) {
        trace_machine("💜💜👍 %s subscribing event '%s' of %s",
            gobj_full_name(subscriber),
            event?event:"*",
            gobj_full_name(publisher)
        );
        if(kw) {
            if(1 || __trace_gobj_ev_kw__(subscriber) || __trace_gobj_ev_kw__(publisher)) {
                log_debug_json(0, kw, "kw");
            }
        }
    }

    /*--------------------------------*
     *      Inform new subscription
     *--------------------------------*/
    if(publisher->gclass->gmt.mt_subscription_added) {
        int result = publisher->gclass->gmt.mt_subscription_added(
            publisher,
            subs
        );
        if(result < 0) {
            _delete_subscription(subs, TRUE, TRUE);
            subs = 0;
        }
    }

    KW_DECREF(kw);
    return subs;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_unsubscribe_event(
    hgobj publisher_,
    const char *event,
    json_t *kw,
    hgobj subscriber_)
{
    GObj_t * publisher = publisher_;
    GObj_t * subscriber = subscriber_;

    /*---------------------*
     *  Check something
     *---------------------*/
    if(!publisher) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "publisher NULL",
            "event",        "%s", event?event:"",
            NULL
        );
        KW_DECREF(kw);
        return -1;
    }
    if(!subscriber) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "subscriber NULL",
            "event",        "%s", event?event:"",
            NULL
        );
        KW_DECREF(kw);
        return -1;
    }

    /*-------------------------------------------------*
     *
     *-------------------------------------------------*/
    if(empty_string(event) || strchr(event, '*')) {
        event = "";
    }

    /*-----------------------------*
     *      Find subscription
     *-----------------------------*/
    KW_INCREF(kw);
    dl_list_t *dl_subs = _find_subscription(
        &publisher->dl_subscriptions,
        publisher,
        event,
        kw,
        subscriber,
        TRUE
    );
    int deleted = 0;
    hsdata subs; rc_instance_t *i_subs;
    i_subs = rc_first_instance(dl_subs, (rc_resource_t **)&subs);
    while(i_subs) {
        hsdata next_subs; rc_instance_t *next_i_subs;
        next_i_subs = rc_next_instance(i_subs, (rc_resource_t **)&next_subs);

        _delete_subscription(subs, 0, 0);
        deleted++;

        i_subs = next_i_subs;
        subs = next_subs;
    }

    if(!deleted) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj_unsubscribe_event() NO SUBCRIPTION FOUND",
            "event",        "%s", event,
            "publisher",    "%s", gobj_full_name(publisher),
            "subscriber",   "%s", gobj_full_name(subscriber),
            NULL
        );
        log_debug_json(0, kw, "gobj_unsubscribe_event() NO SUBCRIPTION FOUND");
    }
    rc_free_iter(dl_subs, TRUE, sdata_destroy);

    KW_DECREF(kw);
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_unsubscribe_event2(
    hsdata subs
)
{
    return _delete_subscription(subs, 0, 0);
}

/***************************************************************************
 *  Unsubscribe a list of subscription hsdata
 ***************************************************************************/
PUBLIC int gobj_unsubscribe_list(
    dl_list_t *dl_subs, // iter of subscription hsdata
    BOOL free_iter,
    BOOL force  // delete hard_subscription subs too
)
{
    hsdata subs; rc_instance_t *i_subs;
    i_subs = rc_first_instance(dl_subs, (rc_resource_t **)&subs);
    while(i_subs) {
        hsdata next_subs; rc_instance_t *next_i_subs;
        next_i_subs = rc_next_instance(i_subs, (rc_resource_t **)&next_subs);

        _delete_subscription(subs, force, 0);

        i_subs = next_i_subs;
        subs = next_subs;
    }
    if(free_iter) {
        rc_free_iter(dl_subs, TRUE, sdata_destroy);
    }
    return 0;
}

/***************************************************************************
 *  Set funtion to be applied in Selection Filter __filter__
 *  Default is kw_match_simple()
 *  Return old function
 ***************************************************************************/
PUBLIC kw_match_fn gobj_set_publication_selection_filter_fn(kw_match_fn kw_match)
{
    kw_match_fn old_fn = __publish_event_match__;
    __publish_event_match__ = kw_match;
    return old_fn;
}

/***************************************************************************
 *  Add transformation filter function,
 *  to be applied with __config__`__trans_filter__
 ***************************************************************************/
PUBLIC int gobj_add_publication_transformation_filter_fn(
    const char *name,
    json_t * (*transformation_fn)(
        json_t *kw // owned
    )
)
{
    return register_transformation_filter(name, transformation_fn);
}

/***************************************************************************
 *  Apply transformation filter functions
 *  from __config__`__trans_filter__
 ***************************************************************************/
PRIVATE json_t *apply_trans(json_t *kw, const char *name)
{
    if(!name) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "name NULL",
            NULL
        );
        return 0;
    }

    trans_filter_t *trans_reg = dl_first(&dl_trans_filter);
    while(trans_reg) {
        if(trans_reg->name) {
            if(strcasecmp(trans_reg->name, name)==0) {
                return trans_reg->transformation_fn(kw);
            }
        }
        trans_reg = dl_next(trans_reg);
    }
    log_error(0,
        "gobj",         "%s", "__yuno__",
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "trans filter NOT FOUND",
        "name",         "%s", name?name:"",
        NULL
    );

    return kw;
}

/***************************************************************************
 *  Apply transformation filter functions
 *  from __config__`__trans_filter__
 ***************************************************************************/
PRIVATE json_t *apply_trans_filters(json_t *kw, json_t *jn_trans_filters)
{
    if(!jn_trans_filters) {
        return kw;
    }

    if(json_is_object(jn_trans_filters)) {
        const char *key;
        json_t *jn_value;
        json_object_foreach(jn_trans_filters, key, jn_value) {
            kw = apply_trans(kw, key);
        }
    } else if(json_is_array(jn_trans_filters)) {
        size_t index;
        json_t *jn_value;
        json_array_foreach(jn_trans_filters, index, jn_value) {
            kw = apply_trans_filters(kw, jn_value);
        }
    } else if(json_is_string(jn_trans_filters)) {
        kw = apply_trans(kw, json_string_value(jn_trans_filters));
    }

    return kw;
}

/***************************************************************************
 *  Return the sum of sent events returns.
 ***************************************************************************/
PUBLIC int gobj_publish_event(
    hgobj publisher_,
    const char *event,
    json_t *kw)
{
    GObj_t * publisher = publisher_;

    if(!kw) {
        kw = json_object();
    }

    /*---------------------*
     *  Check something
     *---------------------*/
    if(!publisher) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }
    if(publisher->obflag & obflag_destroyed) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }
    if(empty_string(event)) {
        log_error(0,
            "gobj",         "%p", gobj_full_name(publisher),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "event EMPTY",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }

    /*--------------------------------------------------------------*
     *  Event must be in output event list
     *  You can avoid this with gcflag_no_check_output_events flag
     *--------------------------------------------------------------*/
    const EVENT *ev = gobj_output_event(publisher, event);
    if(!ev) {
        if(!(publisher->gclass->gcflag & gcflag_no_check_output_events)) {
            log_error(0,
                "gobj",         "%s", gobj_full_name(publisher),
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "event NOT in output event list",
                "event",        "%s", event,
                NULL
            );
            KW_DECREF(kw);
            return 0;
        }
    }

    BOOL tracea = is_machine_tracing(publisher) &&
        !is_machine_not_tracing(publisher) &&
        __trace_gobj_subscriptions__(publisher);
    if(tracea) {
        trace_machine("🔝🔝 mach(%s%s^%s), ev: %s, st: %s",
            (!publisher->running)?"!!":"",
            gobj_gclass_name(publisher), gobj_name(publisher),
            event,
            publisher->mach->fsm->state_names[publisher->mach->current_state]
        );
        if(__trace_gobj_ev_kw__(publisher)) {
            log_debug_json(0, kw, "kw");
        }
    }
    BOOL tracea2 = __trace_gobj_subscriptions2__(publisher);

    /*-------------------------------------*
     *  Own publication method
     *  Return:
     *     -1  (broke),
     *      0  continue without publish,
     *      1  continue and publish
     *-------------------------------------*/
    if(publisher->gclass->gmt.mt_publish_event) {
        int topublish = publisher->gclass->gmt.mt_publish_event(
            publisher,
            event,
            kw  // not owned
        );
        if(topublish<0) {
            KW_DECREF(kw);
            return 0;
        }
    }

    /*--------------------------------------------------------------*
     *  Default publication method
     *--------------------------------------------------------------*/
    dl_list_t *dl_subs = &publisher->dl_subscriptions;
    int ret_sum = 0;
    int sent_count = 0;
    hsdata subs; rc_instance_t *i_subs;
    i_subs = rc_first_instance(dl_subs, (rc_resource_t **)&subs);
    while(i_subs) {
        // TODO no protegido contra borrados
        /*-------------------------------------*
         *  Pre-filter
         *  Return:
         *     -1  (broke),
         *      0  continue without publish,
         *      1  continue and publish
         *-------------------------------------*/
        if(publisher->gclass->gmt.mt_publication_pre_filter) {
            int topublish = publisher->gclass->gmt.mt_publication_pre_filter(
                publisher,
                subs,
                event,
                kw  // not owned
            );
            if(topublish<0) {
                break;
            } else if(topublish==0) {
                /*
                 *  Next subs
                 */
                i_subs = rc_next_instance(i_subs, (rc_resource_t **)&subs);
                continue;
            }
        }
        subs_flag_t subs_flag = sdata_read_uint64(subs, "subs_flag");
        GObj_t *subscriber = sdata_read_pointer(subs, "subscriber");
        if(!(subscriber && !(subscriber->obflag & obflag_destroyed))) {
            /*
             *  Next subs
             */
            i_subs = rc_next_instance(i_subs, (rc_resource_t **)&subs);
            continue;
        }

        /*
         *  Check if event null or event in event_list
         */
        const char *event_ = sdata_read_str(subs, "event");
        if(empty_string(event_) || strcasecmp(event_, event)==0) {
            json_t *__config__ = sdata_read_json(subs, "__config__");
            json_t *__global__ = sdata_read_json(subs, "__global__");
            json_t *__local__ = sdata_read_json(subs, "__local__");
            json_t *__filter__ = sdata_read_json(subs, "__filter__");

            /*
             *  Check renamed_event
             */
            const char *event_name = sdata_read_str(subs, "renamed_event");
            if(empty_string(event_name)) {
                event_name = event;
            }

            /*
             *  Clone the kw to publish if not shared
             */
            json_t *kw2publish = 0;
            if(subs_flag & __share_kw__) {
                KW_INCREF(kw);
                kw2publish = kw;
            } else {
                kw2publish = kw_duplicate(kw);
            }

            /*-------------------------------------*
             *  User filter or configured filter
             *  Return:
             *     -1  (broke),
             *      0  continue without publish,
             *      1  continue and publish
             *-------------------------------------*/
            int topublish = 1;
            if(publisher->gclass->gmt.mt_publication_filter) {
                topublish = publisher->gclass->gmt.mt_publication_filter(
                    publisher,
                    event,
                    kw2publish,  // not owned
                    subscriber
                );
            } else if(__filter__) {
                if(__publish_event_match__) {
                    KW_INCREF(__filter__);
                    topublish = __publish_event_match__(kw2publish , __filter__);
                }
            }

            if(topublish<0) {
                break;
            } else if(topublish==0) {
                /*
                 *  Must not be publicated
                 *  Next subs
                 */
                KW_DECREF(kw2publish);
                i_subs = rc_next_instance(i_subs, (rc_resource_t **)&subs);
                continue;
            }

            /*
             *  Check if System event: don't send if subscriber has not it
             */
            if(ev && ev->flag & EVF_SYSTEM_EVENT) {
                if(!gobj_input_event(subscriber, event)) {
                    KW_DECREF(kw2publish);
                    i_subs = rc_next_instance(i_subs, (rc_resource_t **)&subs);
                    continue;
                }
            }

            /*
             *  Remove local keys
             */
            if(__local__) {
                kw_pop(
                    kw2publish,
                    __local__ // not owned
                );
            }

            /*
             *  Apply transformation filters
             */
            if(__config__) {
                json_t *jn_trans_filters = kw_get_dict_value(__config__, "__trans_filter__", 0, 0);
                if(jn_trans_filters) {
                    kw2publish = apply_trans_filters(kw2publish, jn_trans_filters);
                }
            }

            /*
             *  Add global keys
             */
            if(__global__) {
                json_object_update(kw2publish, __global__);
            }

            /*
             *  Send event
             */
            if(tracea2) {
                trace_machine("🔝🔄 mach(%s%s), ev: %s, from(%s%s)",
                    (!subscriber->running)?"!!":"",
                    gobj_short_name(subscriber),
                    event,
                    (publisher && !publisher->running)?"!!":"",
                    gobj_short_name(publisher)
                );
                if(__trace_gobj_ev_kw__(publisher)) {
                    log_debug_json(0, kw2publish, "kw");
                }
            }

            ret_sum += gobj_send_event(subscriber, event_name, kw2publish, publisher);

            sent_count++;

            if(publisher->obflag & obflag_destroyed) {
                /*
                 *  break all, self publisher deleted
                 */
                break;
            }
        }

        /*
         *  Next subs
         */
        i_subs = rc_next_instance(i_subs, (rc_resource_t **)&subs);
    }

    if(!sent_count) {
        if(!ev || !(ev->flag & EVF_NO_WARN_SUBS)) {
            log_warning(0,
                "publisher",         "%s", gobj_full_name(publisher),
                "msgset",       "%s", MSGSET_MONITORING,
                "msg",          "%s", "Publish event WITHOUT subscribers",
                "event",        "%s", event,
                NULL
            );
            if(__trace_gobj_ev_kw__(publisher)) {
                log_debug_json(0, kw, "kw");
            }
        }
    }

    KW_DECREF(kw);
    return ret_sum;
}

/***************************************************************************
 *  Inject event
 *
 *  I only see event parameter,
 *  the remains parameters are high level specifics
 *  and transparents for me.
 *
 *  Return:
 *      - no permission:.                       -403
 *      - no destine or destine destroyed:      -1000
 *      - event not accepted:                   -1001
 *      - if event is accepted:
 *          - no action:                        -1002
 *          - input event not defined           -1003
 *          - an action is defined:             the return of action.
 ***************************************************************************/
#define RETEVENT_NO_GOBJ                    -1000
#define RETEVENT_NOT_ACCEPTED               -1001
#define RETEVENT_NO_ACTION                  -1002
#define RETEVENT_INPUT_EVENT_NOT_DEFINED    -1003

/***************************************************************************
 *  Send event
 ***************************************************************************/
PUBLIC int gobj_send_event(
    hgobj dst_,
    const char *event,
    json_t *kw,
    hgobj src_)
{
    GObj_t * dst = dst_;
    GObj_t * src = src_;

    SMachine_t * mach;
    register EV_ACTION *actions;
    int ret;
    BOOL tracea;

    if(!dst) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        KW_DECREF(kw);
        return RETEVENT_NO_GOBJ;
    }
    if(dst->obflag & (obflag_destroyed|obflag_destroying)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", (dst->obflag & obflag_destroyed)? "gobj DESTROYED":"gobj DESTROYING",
            NULL
        );
        KW_DECREF(kw);
        return RETEVENT_NO_GOBJ;
    }

    __inside__ ++;

    tracea = is_machine_tracing(dst) && !is_machine_not_tracing(src);
    mach = dst->mach;
    actions = mach->fsm->states[mach->current_state];

    const EVENT *ev_desc = gobj_input_event(dst, event);
    if(!ev_desc) {
        if(dst->gclass->gmt.mt_inject_event) {
            __inside__ --;
            if(tracea) {
                trace_machine("🔃 mach(%s%s^%s), ev: %s, st(%d:%s), from(%s%s^%s)",
                    (!dst->running)?"!!":"",
                    gobj_gclass_name(dst), gobj_name(dst),
                    event,
                    mach->current_state,
                    mach->fsm->state_names[mach->current_state],
                    (src && !src->running)?"!!":"",
                    gobj_gclass_name(src), gobj_name(src)
                );
                if(kw) {
                    if(__trace_gobj_ev_kw__(dst)) {
                        log_debug_json(0, kw, "kw");
                    }
                }
            }
            return dst->gclass->gmt.mt_inject_event(dst, event, kw, src);
        }
        if(tracea) {
            trace_machine("📛 mach(%s%s^%s), st: %s, ev: %s, 📛📛EVENT NOT DEFINED📛📛",
                        (!dst->running)?"!!":"",
                gobj_gclass_name(dst), gobj_name(dst),
                mach->fsm->state_names[mach->current_state],
                event
            );
        } else {
            log_error(0,
                "gobj",         "%s", gobj_short_name(dst),
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "Event NOT DEFINED in input_events array",
                "event",        "%s", event,
                "src",          "%s", gobj_short_name(src),
                "state",        "%s", gobj_current_state(dst),
                "full_src",     "%s", gobj_full_name(src),
                "full_dst",     "%s", gobj_full_name(dst),
                NULL
            );
        }

        __inside__ --;

        KW_DECREF(kw);
        return RETEVENT_INPUT_EVENT_NOT_DEFINED;
    }

    /*----------------------------------*
     *  Check AUTHZ
     *----------------------------------*/
//     if(ev_desc->authz & EV_AUTHZ_INJECT) {
//         /*
//          *  AUTHZ Required
//          */
//         json_t *kw_authz = json_pack("{s:s}",
//             "event", event
//         );
//         if(kw) {
//             json_object_set(kw_authz, "kw", kw);
//         } else {
//             json_object_set_new(kw_authz, "kw", json_object());
//         }
//         if(!gobj_user_has_authz(
//                 dst, "__inject_event__", kw_authz, src)
//           ) {
//             log_error(0,
//                 "gobj",         "%s", gobj_full_name(dst),
//                 "function",     "%s", __FUNCTION__,
//                 "msgset",       "%s", MSGSET_OAUTH_ERROR,
//                 "msg",          "%s", "No permission to inject event",
//                 //"user",         "%s", gobj_get_user(src),
//                 "gclass",       "%s", gobj_gclass_name(dst),
//                 "event",        "%s", event?event:"",
//                 NULL
//             );
//             __inside__ --;
//             KW_DECREF(kw);
//             return -403;
//         }
//     }

    /*----------------------------------*
     *  Search event in current state
     *----------------------------------*/
    BOOL tracea_states = __trace_gobj_states__(dst)?TRUE:FALSE;

    while(actions->event) {
        if(strcasecmp(actions->event, event)==0) {
            if(tracea) {
                trace_machine("🔄 mach(%s%s^%s), ev: %s, st(%d:%s), from(%s%s^%s)",
                    (!dst->running)?"!!":"",
                    gobj_gclass_name(dst), gobj_name(dst),
                    event,
                    mach->current_state,
                    mach->fsm->state_names[mach->current_state],
                    (src && !src->running)?"!!":"",
                    gobj_gclass_name(src), gobj_name(src)
                );
                if(kw) {
                    if(__trace_gobj_ev_kw__(dst)) {
                        log_debug_json(0, kw, "kw");
                    }
                }
            }

            /*
             *  IMPORTANT HACK
             *  Set new state BEFORE run 'action'
             *
             *  The next state is changed before executing the action.
             *  If you don’t like this behavior, set the next-state to NULL
             *  and use change_state() to change the state inside the actions.
             */
            BOOL state_changed = FALSE;
            if(actions->next_state) {
                state_changed = _change_state(dst, actions->next_state);
            }
            if(ev_desc->flag & EVF_KW_WRITING) {
                KW_INCREF(kw);
            }
            if(actions->action) {
                // Execute the action
                ret = (*actions->action)(mach->self, event, kw, src);
            } else {
                // No action, there is nothing amiss!.
                ret = RETEVENT_NO_ACTION;
                KW_DECREF(kw);
            }

            if((src && __trace_gobj_event_monitor__(src)) || (dst &&  __trace_gobj_event_monitor__(dst))) {
                monitor_event(MTOR_EVENT_ACCEPTED, event, src, dst);
            }

            if(state_changed && gobj_is_running(dst)) {
                if(tracea && tracea_states) {
                    trace_machine("🔀🔀 mach(%s%s^%s), st(%d:%s%s%s), ev: %s, from(%s%s^%s)",
                        (!dst->running)?"!!":"",
                        gobj_gclass_name(dst), gobj_name(dst),
                        mach->current_state,
                        On_Black RGreen,
                        mach->fsm->state_names[mach->current_state],
                        Color_Off,
                        event,
                        (src && !src->running)?"!!":"",
                        gobj_gclass_name(src), gobj_name(src)
                    );
                }

                json_t *kw_st = json_object();
                json_object_set_new(
                    kw_st,
                    "previous_state",
                    json_string(mach->fsm->state_names[mach->last_state])
                );
                json_object_set_new(
                    kw_st,
                    "current_state",
                    json_string(mach->fsm->state_names[mach->current_state])
                );

                if(dst->gclass->gmt.mt_state_changed) {
                    dst->gclass->gmt.mt_state_changed(dst, __EV_STATE_CHANGED__, kw_st);
                } else {
                    gobj_publish_event(dst, __EV_STATE_CHANGED__, kw_st);
                }
            }

            if(tracea && !(dst->obflag & obflag_destroyed)) {
                trace_machine("<- mach(%s%s^%s), ev: %s, st(%d:%s), ret: %d",
                    (!dst->running)?"!!":"",
                    gobj_gclass_name(dst), gobj_name(dst),
                    event,
                    mach->current_state,
                    mach->fsm->state_names[mach->current_state],
                    ret
                );
            }

            __inside__ --;

            return ret;
        }
        actions++;
    }

    if(!(dst->obflag & obflag_destroyed)) {
        if(tracea) {
            trace_machine("📛 mach(%s%s^%s), st: %s, ev: %s, 📛📛EVENT REFUSED📛📛 from %s",
                        (!dst->running)?"!!":"",
                gobj_gclass_name(dst), gobj_name(dst),
                mach->fsm->state_names[mach->current_state],
                event,
                gobj_gclass_name(src)
            );
        } else {
            log_warning(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", gobj_short_name(dst),
                "msgset",       "%s", MSGSET_SMACHINE_ERROR,
                "msg",          "%s", "Event REFUSED",
                "orig-gclass",  "%s", gobj_full_name(src),
                "dest-gclass",  "%s", gobj_full_name(dst),
                "state",        "%s", mach->fsm->state_names[mach->current_state],
                "event",        "%s", event,
                NULL
            );
        }
    }
    if((src && __trace_gobj_event_monitor__(src)) || __trace_gobj_event_monitor__(dst)) {
         monitor_event(MTOR_EVENT_REFUSED, event, src, dst);
    }

    KW_DECREF(kw);

    __inside__ --;

    return RETEVENT_NOT_ACCEPTED;
}

/***************************************************************************
 *  Send the event to all gclass's instances in the gobj's tree.
 ***************************************************************************/
PRIVATE int cb_send_event(hgobj child_, void *user_data, void *user_data2, void *user_data3)
{
    GObj_t *child = child_;
    const char *event = user_data;
    json_t *kw = user_data2;
    hgobj src = user_data3;

    JSON_INCREF(kw);
    gobj_send_event(child, event, kw, src);

    return 0;
}
PUBLIC int gobj_send_event_to_gclass_instances(
    hgobj gobj,
    const char *gclass_name,
    const char *event,
    json_t *kw,
    hgobj src)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }
    if(empty_string(gclass_name)) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gclass_name NULL",
            NULL
        );
        return -1;
    }

    if(gobj_typeof_gclass(gobj, gclass_name)) {
        JSON_INCREF(kw);
        gobj_send_event(gobj, event, kw, src);
    }

    dl_list_t dl_list;
    json_t *jn_filter = json_pack("{s:s}",
        "__gclass_name__", gclass_name
    );
    gobj_match_childs_tree(gobj, &dl_list, jn_filter);

    rc_walk_by_list(
        &dl_list,
        WALK_LAST2FIRST,
        (cb_walking_t)cb_send_event,
        (void *)event,
        kw,
        src
    );
    rc_free_iter(&dl_list, 0, 0);

    JSON_DECREF(kw);
    return 0;
}

/***************************************************************************
 *  Send the event to child with name ``name``
 ***************************************************************************/
PUBLIC int gobj_send_event_to_named_child(
    hgobj gobj,
    const char *name,
    const char *event,
    json_t *kw,
    hgobj src)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }
    if(empty_string(name)) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "name NULL",
            NULL
        );
        return -1;
    }

    dl_list_t dl_list;
    json_t *jn_filter = json_pack("{s:s}",
        "__gobj_name__", name
    );
    gobj_match_childs(gobj, &dl_list, jn_filter);

    rc_walk_by_list(
        &dl_list,
        WALK_LAST2FIRST,
        (cb_walking_t)cb_send_event,
        (void *)event,
        kw,
        src
    );
    rc_free_iter(&dl_list, 0, 0);

    JSON_DECREF(kw);
    return 0;
}

/***************************************************************************
 *  Send the event to all childs
 ***************************************************************************/
PRIVATE int cb_send_event_if_in(rc_instance_t *i_child, hgobj child_, void *user_data, void *user_data2, void *user_data3)
{
    GObj_t *child = child_;
    const char *event = user_data;
    json_t *kw = user_data2;
    hgobj src = user_data3;

    if(gobj_event_in_input_event_list(child, event, 0)) {
        JSON_INCREF(kw)
        gobj_send_event(child, event, kw, src);
    }

    return 0;
}
PUBLIC int gobj_send_event_to_childs(
    hgobj gobj,
    const char *event,
    json_t *kw,
    hgobj src)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }

    gobj_walk_gobj_childs(gobj, WALK_FIRST2LAST, cb_send_event_if_in, (void *)event, kw, src);

    JSON_DECREF(kw)
    return 0;
}

/***************************************************************************
 *  Send the event to all childs tree
 *  same as gobj_send_event_to_childs but recursive
 ***************************************************************************/
PUBLIC int gobj_send_event_to_childs_tree(
    hgobj gobj,
    const char *event,
    json_t *kw,
    hgobj src)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }

    gobj_walk_gobj_childs_tree(gobj, WALK_FIRST2LAST, cb_send_event_if_in, (void *)event, kw, src);

    JSON_DECREF(kw)
    return 0;
}

/***************************************************************************
 *  Same as gobj_send_event_to_childs
 *  but recursive by levels instead of traverse the tree.
 *  - by levels: first all childs, next all childs of the childs, etc
 ***************************************************************************/
PUBLIC int gobj_send_event_to_childs_tree_by_level(
    hgobj gobj,
    const char *event,
    json_t *kw,
    hgobj src)
{
    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return -1;
    }

    gobj_walk_gobj_childs_tree(
        gobj,
        WALK_BYLEVEL|WALK_FIRST2LAST,
        cb_send_event_if_in,
        (void *)event,
        kw,
        src
    );

    JSON_DECREF(kw)
    return 0;
}




                    /*------------------------------------*
                     *  SECTION: Organization functions
                     *-----------------------------------*/




/***************************************************************************
 *
 ***************************************************************************/
PRIVATE GObj_t * _deep_bottom_gobj(GObj_t *gobj)
{
    if(gobj->bottom_gobj) {
        return _deep_bottom_gobj(gobj->bottom_gobj);
    } else {
        return gobj;
    }
}

/***************************************************************************
 *  Set manually the bottom gobj
 ***************************************************************************/
PUBLIC hgobj gobj_set_bottom_gobj(hgobj gobj_, hgobj bottom_gobj)
{
    GObj_t * gobj = gobj_;

    if(is_machine_tracing(gobj)) {
        trace_machine("🔽 set_bottom_gobj('%s') = '%s'",
            gobj_short_name(gobj),
            bottom_gobj?gobj_short_name(bottom_gobj):""
        );
    }
    if(gobj->bottom_gobj) {
        /*
         *  En los gobj con manual start ellos suelen poner su bottom gobj.
         *  No lo consideramos un error.
         *  Ej: emailsender -> curl
         *      y luego va e intenta poner emailsender -> IOGate
         *      porque está definido con tree, y no tiene en cuenta la creación de un bottom interno.
         *
         */
        if(bottom_gobj) {
            log_warning(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", gobj_full_name(gobj),
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "bottom_gobj already set",
                "prev_gobj",    "%s", gobj_full_name(gobj->bottom_gobj),
                "new_gobj",     "%s", gobj_full_name(bottom_gobj),
                NULL
            );
        }
        // anyway set -> NO! -> the already set has preference. New 8-10-2016
        // anyway set -> YES! -> the new set has preference. New 27-Jan-2017
        //return 0;
    }
    gobj->bottom_gobj = bottom_gobj;
    return 0;
}

/***************************************************************************
 *  Return the last bottom gobj of gobj tree.
 *  Useful when there is a stack of gobjs acting as a unit.
 *  Filled by gobj_create_tree0() function.
 ***************************************************************************/
PUBLIC hgobj gobj_last_bottom_gobj(hgobj gobj_)
{
    GObj_t * gobj = gobj_;

    if(gobj->bottom_gobj) {
        return _deep_bottom_gobj(gobj->bottom_gobj);
    } else {
        return 0;
    }
}

/***************************************************************************
 *  Return the next bottom gobj of the gobj.
 *  See gobj_last_bottom_gobj()
 ***************************************************************************/
PUBLIC hgobj gobj_bottom_gobj(hgobj gobj_)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        return 0;
    }
    return gobj->bottom_gobj;
}




                    /*---------------------------------*
                     *  SECTION: Attribute functions
                     *---------------------------------*/




/***************************************************************************
 *  ATTR:
 *  Contain the key my gobj-name or my gclass-name?
 *  Return NULL if not, or the key cleaned of prefix.
 ***************************************************************************/
PRIVATE const char * is_for_me(
    const char *gclass_name,
    const char *gobj_name,
    const char *key)
{
    char *p = strchr(key, '.');
    if(p) {
        char temp[256]; // if your names are longer than 256 bytes, you are ...
        int ln = p - key;
        if (ln >= sizeof(temp))
            return 0; // ... very inteligent!
        strncpy(temp, key, ln);
        temp[ln]=0;

        if(gobj_name && strcmp(gobj_name, temp)==0) {
            // match the name
            return p+1;
        }
        if(gclass_name && strcasecmp(gclass_name, temp)==0) {
            // match the gclass name
            return p+1;
        }
    }
    return 0;
}

/***************************************************************************
 *  ATTR:
 ***************************************************************************/
PRIVATE json_t *extract_all_mine(
    const char *gclass_name,
    const char *gobj_name,
    json_t *kw)     // not own
{
    json_t *kw_mine = json_object();
    const char *key;
    json_t *jn_value;

    json_object_foreach(kw, key, jn_value) {
        const char *pk = is_for_me(
            gclass_name,
            gobj_name,
            key
        );
        if(!pk)
            continue;
        if(strcasecmp(pk, "kw")==0) {
            json_object_update(kw_mine, jn_value);
        } else {
            json_object_set(kw_mine, pk, jn_value);
        }
    }

    return kw_mine;
}

/***************************************************************************
 *  ATTR:
 ***************************************************************************/
PRIVATE json_t *extract_json_config_variables_mine(
    const char *gclass_name,
    const char *gobj_name,
    json_t *kw     // not own
)
{
    json_t *kw_mine = json_object();
    const char *key;
    json_t *jn_value;

    json_object_foreach(kw, key, jn_value) {
        const char *pk = is_for_me(
            gclass_name,
            gobj_name,
            key
        );
        if(!pk)
            continue;
        if(strcasecmp(pk, "__json_config_variables__")==0) {
            json_object_set(kw_mine, pk, jn_value);
        }
    }

    return kw_mine;
}

/***************************************************************************
 *  ATTR:
 *  Get data from json kw parameter, and put the data in config sdata.

    With filter_own:
    Filter only the parameters in settings belongs to gobj.
    The gobj is searched by his named-gobj or his gclass name.
    The parameter name in settings, must be a dot-named,
    with the first item being the named-gobj o gclass name.
 *
 ***************************************************************************/
PRIVATE int gobj_write_json_parameters(
    GObj_t * gobj,
    json_t *kw,     // not own
    json_t *jn_global) // not own
{
    hsdata hs = gobj_hsdata(gobj);
    json_t *jn_global_mine = extract_all_mine(
        gobj->gclass->gclass_name,
        gobj->name,
        jn_global
    );

    if(__trace_gobj_create_delete2__(gobj)) {
        trace_machine("🔰 %s^%s => global_mine",
            gobj->gclass->gclass_name,
            gobj->name
        );
        log_debug_json(0, jn_global, "global all");
        log_debug_json(0, jn_global_mine, "global_mine");
    }

    json_t *__json_config_variables__ = kw_get_dict(
        jn_global_mine,
        "__json_config_variables__",
        json_object(),
        KW_CREATE
    );

    json_object_update_new(
        __json_config_variables__,
        gobj_global_variables()
    );
    if(gobj_yuno()) {
        json_object_update_new(
            __json_config_variables__,
            json_pack("{s:s, s:b}",
                "__bind_ip__", gobj_read_str_attr(gobj_yuno(), "bind_ip"),
                "__multiple__", gobj_read_bool_attr(gobj_yuno(), "yuno_multiple")
            )
        );
    }

    if(__trace_gobj_create_delete2__(gobj)) {
        trace_machine("🔰🔰 %s^%s => __json_config_variables__",
            gobj->gclass->gclass_name,
            gobj->name
        );
        log_debug_json(0, __json_config_variables__, "__json_config_variables__");
    }

    json_t * new_kw = kw_apply_json_config_variables(kw, jn_global_mine);
    json_decref(jn_global_mine);
    if(!new_kw) {
        return -1;
    }

    if(__trace_gobj_create_delete2__(gobj)) {
        trace_machine("🔰🔰🔰 %s^%s => final kw",
            gobj->gclass->gclass_name,
            gobj->name
        );
        log_debug_json(0, new_kw, "final kw");
    }

    json_t *__user_data__ = kw_get_dict(new_kw, "__user_data__", 0, KW_EXTRACT);

    int ret = json2sdata(
        hs,
        new_kw,
        -1,
        (gobj->gclass->gcflag & gcflag_ignore_unknown_attrs)?0:print_attr_not_found,
        gobj
    );

    if(__user_data__) {
        json_object_update(((GObj_t *)gobj)->jn_user_data, __user_data__);
        json_decref(__user_data__);
    }
    json_decref(new_kw);
    return ret;
}

/***************************************************************************
 *  load or re-load persistent and writable attrs
 ***************************************************************************/
PUBLIC int gobj_load_persistent_attrs(hgobj gobj_, json_t *jn_attrs) // str, list or dict
{
    GObj_t *gobj = gobj_;

    if(!(gobj->obflag & (obflag_unique_name))) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "Cannot load writable-persistent attrs in no named-gobjs",
            NULL
        );
        JSON_DECREF(jn_attrs);
        return -1;
    }
    if(__global_load_persistent_attrs_fn__) {
        return __global_load_persistent_attrs_fn__(gobj, jn_attrs);
    }
    JSON_DECREF(jn_attrs);
    return -1;
}

/***************************************************************************
 *  save now persistent and writable attrs
    attrs can be a string, a list of keys, or a dict with the keys to save/delete
 ***************************************************************************/
PUBLIC int gobj_save_persistent_attrs(hgobj gobj_, json_t *jn_attrs)
{
    GObj_t *gobj = gobj_;

    if(!(gobj->obflag & (obflag_unique_name))) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "Cannot save writable-persistent attrs in no named-gobjs",
            NULL
        );
        JSON_DECREF(jn_attrs);
        return -1;
    }
    if(__global_save_persistent_attrs_fn__) {
        return __global_save_persistent_attrs_fn__(gobj, jn_attrs);
    }
    JSON_DECREF(jn_attrs);
    return -1;
}

/***************************************************************************
 *  remove file of persistent and writable attrs
 *  attrs can be a string, a list of keys, or a dict with the keys to save/delete
 *  if attrs is empty save/remove all attrs
 ***************************************************************************/
PUBLIC int gobj_remove_persistent_attrs(hgobj gobj_, json_t *jn_attrs)
{
    GObj_t *gobj = gobj_;

    if(!__global_remove_persistent_attrs_fn__) {
        JSON_DECREF(jn_attrs);
        return -1;
    }
    return __global_remove_persistent_attrs_fn__(gobj, jn_attrs);
}

/***************************************************************************
 *  List persistent attributes
 ***************************************************************************/
PUBLIC json_t * gobj_list_persistent_attrs(hgobj gobj, json_t *jn_attrs)
{
    if(!__global_list_persistent_attrs_fn__) {
        JSON_DECREF(jn_attrs);
        return 0;
    }
    return __global_list_persistent_attrs_fn__(gobj, jn_attrs);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC hsdata gobj_hsdata(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    if(!gobj) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }
    return ((GObj_t *)gobj)->hsdata_attr;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const sdata_desc_t * gobj_tattr_desc(hgobj gobj_)
{
    GObj_t *gobj = gobj_;

    if(!gobj) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }

    return gobj->gclass->tattr_desc;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const sdata_desc_t * gobj_attr_desc(hgobj gobj_, const char *name)
{
    GObj_t *gobj = gobj_;

    if(!gobj) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }

    return sdata_it_desc(gobj->gclass->tattr_desc, name);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const sdata_desc_t * gobj_bottom_attr_desc(hgobj gobj_, const char *name)
{
    GObj_t *gobj = gobj_;

    if(gobj_has_attr(gobj, name)) {
        return sdata_it_desc(gobj->gclass->tattr_desc, name);
    } else if(gobj && gobj->bottom_gobj) {
        return gobj_bottom_attr_desc(gobj->bottom_gobj, name);
    }
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
BOOL gclass_has_attr(GCLASS *gclass, const char* name)
{
    const sdata_desc_t *it = sdata_it_desc(gclass->tattr_desc, name);
    if(!it) {
        return FALSE;
    } else {
        return TRUE;
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC BOOL gobj_has_attr(hgobj gobj, const char *name)
{
    if(!gobj) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            "attr",         "%s", name?name:"",
            NULL
        );
        return FALSE;
    }
    return gclass_has_attr(((GObj_t *)gobj)->gclass, name);
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC BOOL gobj_has_bottom_attr(hgobj gobj_, const char *name)
{
    GObj_t *gobj = gobj_;

    if(gobj_has_attr(gobj, name)) {
        return TRUE;
    } else if(gobj && gobj->bottom_gobj) {
        return gobj_has_bottom_attr(gobj->bottom_gobj, name);
    }

    return FALSE;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_attr_type(hgobj gobj, const char *name)
{
    const sdata_desc_t *it = gobj_attr_desc(gobj, name);
    if(it) {
        return it->type;
    } else {
        return 0;
    }
}

/***************************************************************************
 *  ATTR: callback for sdata
 ***************************************************************************/
PRIVATE int on_post_write_it_cb(void *user_data, const char *name)
{
    GObj_t *gobj = user_data;

    if((gobj->obflag & obflag_created) && !(gobj->obflag & obflag_destroyed)) {
        // Avoid call to mt_writing before mt_create!
        if(gobj->gclass->gmt.mt_writing) {
            gobj->gclass->gmt.mt_writing(gobj, name);
        }
    }
    return 0;
}

/***************************************************************************
 *  ATTR: callback for sdata
 ***************************************************************************/
PRIVATE int on_post_write_stats_it_cb(
    void* user_data,
    const char* name,
    int type,
    SData_Value_t old_v,
    SData_Value_t new_v)
{
    GObj_t *obj_stats = user_data;

    if(!(obj_stats->obflag & obflag_created) || (obj_stats->obflag & (obflag_destroyed|obflag_destroying))) {
        return 0;
    }
    // al servicio mas cernano o al yuno
    GObj_t *gobj_s = gobj_nearest_top_service(obj_stats);
    while(gobj_s) {
        if(gobj_s && gobj_s->gclass->gmt.mt_stats_updated) {
            if(gobj_s->gclass->gmt.mt_stats_updated(gobj_s, obj_stats, name, type, old_v, new_v)==0) {
                break;
            }
        }
        gobj_s = gobj_nearest_top_service(gobj_s);
    }
    return 0;
}

/***************************************************************************
 *  ATTR: callback for sdata
 ***************************************************************************/
PRIVATE SData_Value_t on_post_read_it_cb(void *user_data, const char *name, int type, SData_Value_t data)
{
    GObj_t *gobj = user_data;

    if(!(gobj->obflag & obflag_destroyed)) {
        if(gobj->gclass->gmt.mt_reading) {
            return gobj->gclass->gmt.mt_reading(gobj, name, type, data);
        }
    }
    return data;
}

/***************************************************************************
 *  ATTR: Get hsdata of inherited attribute.
 ***************************************************************************/
PUBLIC hsdata gobj_hsdata2(hgobj gobj_, const char *name, BOOL verbose)
{
    GObj_t *gobj = gobj_;

    if(!gobj) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return 0;
    }

    if(gobj_has_attr(gobj, name)) {
        return gobj_hsdata(gobj);
    } else if(gobj && gobj->bottom_gobj) {
        return gobj_hsdata2(gobj->bottom_gobj, name, verbose);
    }
    if(verbose) {
        log_warning(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "GClass Attribute NOT FOUND",
            "gclass",       "%s", gobj_gclass_name(gobj),
            "attr",         "%s", name?name:"",
            NULL
        );
    }
    return 0;
}

/***************************************************************************
 *  ATTR: read the attr pointer, traversing inherited gobjs if need it.
 *  DANGER if you don't cast well: OVERFLOW variables!
 ***************************************************************************/
PUBLIC void *gobj_danger_attr_ptr(hgobj gobj, const char *name)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_it_pointer(hs, name, 0);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read the attr pointer, traversing inherited gobjs if need it.
 *  DANGER if you don't cast well: OVERFLOW variables!
 ***************************************************************************/
PUBLIC void *gobj_danger_attr_ptr2(hgobj gobj, const char *name, const sdata_desc_t **pit)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_it_pointer(hs, name, pit);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read str
 *  Return is yours! Must be decref. New api (May/2019), js style
 *  With AUTHZ
 ***************************************************************************/
PUBLIC json_t *gobj_read_attr(
    hgobj gobj,
    const char *name,
    hgobj src
)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(!hs) {
        log_warning(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "GClass Attribute NOT FOUND",
            "gclass",       "%s", gobj_gclass_name(gobj),
            "attr",         "%s", name?name:"",
            NULL
        );
        return 0;
    }

    const sdata_desc_t *it = sdata_it_desc(sdata_schema(hs), name);

//     if(it->flag & SDF_AUTHZ_R) {
//         /*
//          *  AUTHZ Required
//          */
//         if(!gobj_user_has_authz(
//                 gobj, "__read_attribute__", json_pack("{s:s}", "path", name), src)
//           ) {
//             log_error(0,
//                 "gobj",         "%s", gobj_full_name(gobj),
//                 "function",     "%s", __FUNCTION__,
//                 "msgset",       "%s", MSGSET_OAUTH_ERROR,
//                 "msg",          "%s", "No permission to read attribute",
//                 //"user",         "%s", gobj_get_user(src),
//                 "gclass",       "%s", gobj_gclass_name(gobj),
//                 "attr",         "%s", name?name:"",
//                 NULL
//             );
//             return 0;
//         }
//     }

    json_t *jn_value = 0;

    if(ASN_IS_STRING(it->type)) {
        jn_value = json_string(sdata_read_str(hs, name));
    } else if(ASN_IS_BOOLEAN(it->type)) {
        jn_value = sdata_read_bool(hs, name)?json_true():json_false();
    } else if(ASN_IS_UNSIGNED32(it->type)) {
        jn_value = json_integer(sdata_read_uint32(hs, name));
    } else if(ASN_IS_SIGNED32(it->type)) {
        jn_value = json_integer(sdata_read_int32(hs, name));
    } else if(ASN_IS_UNSIGNED64(it->type)) {
        jn_value = json_integer(sdata_read_uint64(hs, name));
    } else if(ASN_IS_SIGNED64(it->type)) {
        jn_value = json_integer(sdata_read_int64(hs, name));
    } else if(ASN_IS_DOUBLE(it->type)) {
        jn_value = json_real(sdata_read_real(hs, name));
    } else if(ASN_IS_JSON(it->type)) {
        jn_value = sdata_read_json(hs, name);
        JSON_INCREF(jn_value);
    } else if(ASN_IS_POINTER(it->type)) {
        jn_value = json_integer((json_int_t)(size_t)sdata_read_pointer(hs, name));
    } else {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "GClass Attribute Type NOT VALID",
            "gclass",       "%s", gobj_gclass_name(gobj),
            "attr",         "%s", name?name:"",
            NULL
        );
    }

    return jn_value;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC json_t *gobj_read_user_data(
    hgobj gobj,
    const char *name
)
{
    if(gobj) {
        if(empty_string(name)) {
            return ((GObj_t *)gobj)->jn_user_data;
        } else {
            return json_object_get(((GObj_t *)gobj)->jn_user_data, name);
        }
    } else {
        return 0;
    }
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC json_t *gobj_kw_get_user_data(
    hgobj gobj,
    const char *path,
    json_t *default_value,
    kw_flag_t flag
)
{
    if(gobj) {
        return kw_get_dict_value(((GObj_t *)gobj)->jn_user_data, path, default_value, flag);
    } else {
        return 0;
    }
}

/***************************************************************************
 *  ATTR: read str
 ***************************************************************************/
PUBLIC const char *gobj_read_str_attr(hgobj gobj, const char *name)
{
    if(name && strcasecmp(name, "__state__")==0) {
        return gobj_current_state(gobj);
    }
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_str(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read bool
 ***************************************************************************/
PUBLIC BOOL gobj_read_bool_attr(hgobj gobj, const char *name)
{
    if(name) {
        if(strcasecmp(name, "__disabled__")==0) {
            return gobj_is_disabled(gobj);
        } else if(strcasecmp(name, "__running__")==0) {
            return gobj_is_running(gobj);
        } else if(strcasecmp(name, "__playing__")==0) {
            return gobj_is_playing(gobj);
        } else if(strcasecmp(name, "__service__")==0) {
            return gobj_is_service(gobj);
        }
    }
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_bool(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC int32_t gobj_read_int32_attr(hgobj gobj, const char *name)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_int32(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC uint32_t gobj_read_uint32_attr(hgobj gobj, const char *name)
{
    if(name && strcasecmp(name, "__trace_level__")==0) {
        return gobj_trace_level(gobj);
    }
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_uint32(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC int64_t gobj_read_int64_attr(hgobj gobj, const char *name)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_int64(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC uint64_t gobj_read_uint64_attr(hgobj gobj, const char *name)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_uint64(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC uint64_t gobj_read_integer_attr(hgobj gobj, const char *name)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_integer(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC double gobj_read_real_attr(hgobj gobj, const char *name)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_real(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC json_t *gobj_read_json_attr(hgobj gobj, const char *name)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_json(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC void *gobj_read_pointer_attr(hgobj gobj, const char *name)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_pointer(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC dl_list_t *gobj_read_dl_list_attr(hgobj gobj, const char *name)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_dl_list(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC dl_list_t *gobj_read_iter_attr(hgobj gobj, const char *name)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_read_iter(hs, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC const sdata_desc_t * gobj_read_iter_schema(hgobj gobj_, const char *name)
{
    GObj_t *gobj = gobj_;

    if(gobj_has_attr(gobj, name)) {
        const sdata_desc_t * desc = gobj_attr_desc(gobj, name);
        return desc->schema;
    } else if(gobj && gobj->bottom_gobj) {
        return gobj_read_iter_schema(gobj->bottom_gobj, name);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC BOOL gobj_is_readable_attr(hgobj gobj, const char *name)
{
    const sdata_desc_t *it = gobj_attr_desc(gobj, name);
    if(it && it->flag & (ATTR_READABLE)) {
        return TRUE;
    } else {
        return FALSE;
    }
}
/***************************************************************************
 *  ATTR: read
 ***************************************************************************/
PUBLIC SData_Value_t gobj_read_default_attr_value(hgobj gobj, const char* name)  // Only for basic types
{
    return sdata_get_default_value(gobj_hsdata(gobj), name);
}

/***************************************************************************
 *  ATTR: write
 *  New api (May/2019), js style
 *  With AUTHZ
 ***************************************************************************/
PUBLIC int gobj_write_attr(
    hgobj gobj,
    const char *path,
    json_t *value,  // owned
    hgobj src
)
{
    hsdata hs = gobj_hsdata2(gobj, path, FALSE);
    if(!hs) {
        log_warning(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "GClass Attribute NOT FOUND",
            "gclass",       "%s", gobj_gclass_name(gobj),
            "attr",         "%s", path?path:"",
            NULL
        );
        JSON_DECREF(value);
        return -1;
    }

    const sdata_desc_t *it = sdata_it_desc(sdata_schema(hs), path);

//     if(it->flag & SDF_AUTHZ_W) {
//         /*
//          *  AUTHZ Required
//          */
//         if(!gobj_user_has_authz(
//                 gobj, "__write_attribute__", json_pack("{s:s}", "path", path), src)
//           ) {
//             log_error(0,
//                 "gobj",         "%s", gobj_full_name(gobj),
//                 "function",     "%s", __FUNCTION__,
//                 "msgset",       "%s", MSGSET_OAUTH_ERROR,
//                 "msg",          "%s", "No permission to write attribute",
//                 //"user",         "%s", gobj_get_user(src),
//                 "gclass",       "%s", gobj_gclass_name(gobj),
//                 "attr",         "%s", path?path:"",
//                 NULL
//             );
//             JSON_DECREF(value);
//             return -403;
//         }
//     }

    int ret;

    if(ASN_IS_STRING(it->type)) {
        ret = sdata_write_str(hs, path, json_string_value(value));
    } else if(ASN_IS_BOOLEAN(it->type)) {
        ret = sdata_write_bool(hs, path, json_boolean_value(value));
    } else if(ASN_IS_UNSIGNED32(it->type)) {
        ret = sdata_write_uint32(hs, path, json_integer_value(value));
    } else if(ASN_IS_SIGNED32(it->type)) {
        ret = sdata_write_int32(hs, path, json_integer_value(value));
    } else if(ASN_IS_UNSIGNED64(it->type)) {
        ret = sdata_write_uint64(hs, path, json_integer_value(value));
    } else if(ASN_IS_SIGNED64(it->type)) {
        ret = sdata_write_int64(hs, path, json_integer_value(value));
    } else if(ASN_IS_DOUBLE(it->type)) {
        ret = sdata_write_real(hs, path, json_real_value(value));
    } else if(ASN_IS_JSON(it->type)) {
        ret = sdata_write_json(hs, path, value); // WARNING json is incref
        JSON_DECREF(value);
    } else if(ASN_IS_POINTER(it->type)) {
        ret = sdata_write_pointer(hs, path, (void *)(size_t)json_integer_value(value));
    } else {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "GClass Attribute Type NOT VALID",
            "gclass",       "%s", gobj_gclass_name(gobj),
            "attr",         "%s", path?path:"",
            NULL
        );
        ret = -1;
    }

    JSON_DECREF(value);
    return ret;
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_write_user_data(
    hgobj gobj,
    const char *name,
    json_t *value // owned
)
{
    if(gobj) {
        return json_object_set_new(((GObj_t *)gobj)->jn_user_data, name, value);
    } else {
        return -1;
    }
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_kw_set_user_data(
    hgobj gobj,
    const char *path,   // The last word after ` is the key
    json_t *value // owned
)
{
    if(gobj) {
        return kw_set_dict_value(((GObj_t *)gobj)->jn_user_data, path, value);
    } else {
        return -1;
    }
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_write_str_attr(hgobj gobj, const char *name, const char *value)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_write_str(hs, name, value);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return -1;
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_write_strn_attr(hgobj gobj, const char *name, const char *value, int string_length)
{
    // HACK TODO sdata_write_str() está mal diseñado, no permite pasar un string con longitud
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        char *value_ = gbmem_strndup(value, string_length);
        if(!value_) {
            log_warning(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", gobj_full_name(gobj),
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "gbmem_strndup() FAILED",
                "gclass",       "%s", gobj_gclass_name(gobj),
                "attr",         "%s", name?name:"",
                "string_length","%d", string_length,
                NULL
            );
            return -1;
        }
        int ret = sdata_write_str(hs, name, value_);
        GBMEM_FREE(value_);
        return ret;
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return -1;
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_write_bool_attr(hgobj gobj, const char *name, BOOL value)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_write_bool(hs, name, value);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return -1;
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_write_int32_attr(hgobj gobj, const char *name, int32_t value)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_write_int32(hs, name, value);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return -1;
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_write_uint32_attr(hgobj gobj, const char *name, uint32_t value)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_write_uint32(hs, name, value);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return -1;
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_write_int64_attr(hgobj gobj, const char *name, int64_t value)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_write_int64(hs, name, value);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return -1;
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_write_uint64_attr(hgobj gobj, const char *name, uint64_t value)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_write_uint64(hs, name, value);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return -1;
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_write_integer_attr(hgobj gobj, const char *name, uint64_t value)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_write_integer(hs, name, value);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return -1;
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_write_real_attr(hgobj gobj, const char *name, double value)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_write_real(hs, name, value);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return -1;
}

/***************************************************************************
 *  ATTR: write.  WARNING json is incref! (new V2)
 ***************************************************************************/
PUBLIC int gobj_write_json_attr(hgobj gobj, const char *name, json_t *value)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_write_json(hs, name, value); // value is incref!
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    KW_INCREF(value); // although error the json is incref
    return -1;
}

/***************************************************************************
 *  ATTR: write.  WARNING json is NOT incref
 ***************************************************************************/
PUBLIC int gobj_write_new_json_attr(hgobj gobj, const char *name, json_t *value)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        int ret = sdata_write_json(hs, name, value); // value is incref!
        KW_DECREF(value);
        return ret;
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    KW_DECREF(value);
    return -1;
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC int gobj_write_pointer_attr(hgobj gobj, const char *name, void *value)
{
    hsdata hs = gobj_hsdata2(gobj, name, FALSE);
    if(hs) {
        return sdata_write_pointer(hs, name, value);
    }
    log_warning(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "GClass Attribute NOT FOUND",
        "gclass",       "%s", gobj_gclass_name(gobj),
        "attr",         "%s", name?name:"",
        NULL
    );
    return -1;
}

/***************************************************************************
 *  ATTR: write
 ***************************************************************************/
PUBLIC BOOL gobj_is_writable_attr(hgobj gobj, const char *name)
{
    const sdata_desc_t *it = gobj_attr_desc(gobj, name);
    if(it && it->flag & (ATTR_WRITABLE)) {
        return TRUE;
    } else {
        return FALSE;
    }
}

/***************************************************************************
 *  ATTR: write
 *  Return a json list of writable attribute names
 ***************************************************************************/
PUBLIC json_t *gobj_get_writable_attrs(hgobj gobj) // Return is yours, decref!
{
    json_t *jn_list = json_array();

    const sdata_desc_t *it = gobj_tattr_desc(gobj);

    while(it && it->name) {
        if(it->flag & (SDF_PERSIST|SDF_WR)) {
            json_array_append_new(jn_list, json_string(it->name));
        }
        it++;
    }
    return jn_list;
}

/***************************************************************************
 *  ATTR: write
 *  Update writable attrs
 ***************************************************************************/
PUBLIC int gobj_update_writable_attrs(
    hgobj gobj,
    json_t *jn_attrs, // owned
    hgobj src
)
{
    int ret = 0;
    const char *key; json_t *jn_attr;
    json_object_foreach(jn_attrs, key, jn_attr) {
        if(gobj_is_writable_attr(gobj, key)) {
            JSON_INCREF(jn_attr);
            ret += gobj_write_attr(gobj, key, jn_attr, src);
        }
    }

    JSON_DECREF(jn_attrs);
    return ret;
}

/***************************************************************************
 *  ATTR: write
 *  Reset volatile attributes
 ***************************************************************************/
PUBLIC int gobj_reset_volatil_attrs(hgobj gobj)
{
    return sdata_write_default_values(
        gobj_hsdata(gobj),
        SDF_VOLATIL,    // include_flag
        0               // exclude_flag
    );
}

/***************************************************************************
 *  Print yuneta gclass's methods in json
 ***************************************************************************/
PRIVATE json_t *yunetamethods2json(GMETHODS *gmt)
{
    json_t *jn_methods = json_array();

    if(gmt->mt_create)
        json_array_append_new(jn_methods, json_string("mt_create"));
    if(gmt->mt_create2)
        json_array_append_new(jn_methods, json_string("mt_create2"));
    if(gmt->mt_destroy)
        json_array_append_new(jn_methods, json_string("mt_destroy"));
    if(gmt->mt_start)
        json_array_append_new(jn_methods, json_string("mt_start"));
    if(gmt->mt_stop)
        json_array_append_new(jn_methods, json_string("mt_stop"));
    if(gmt->mt_play)
        json_array_append_new(jn_methods, json_string("mt_play"));
    if(gmt->mt_pause)
        json_array_append_new(jn_methods, json_string("mt_pause"));
    if(gmt->mt_writing)
        json_array_append_new(jn_methods, json_string("mt_writing"));
    if(gmt->mt_reading)
        json_array_append_new(jn_methods, json_string("mt_reading"));
    if(gmt->mt_subscription_added)
        json_array_append_new(jn_methods, json_string("mt_subscription_added"));
    if(gmt->mt_subscription_deleted)
        json_array_append_new(jn_methods, json_string("mt_subscription_deleted"));
    if(gmt->mt_child_added)
        json_array_append_new(jn_methods, json_string("mt_child_added"));
    if(gmt->mt_child_removed)
        json_array_append_new(jn_methods, json_string("mt_child_removed"));
    if(gmt->mt_stats)
        json_array_append_new(jn_methods, json_string("mt_stats"));
    if(gmt->mt_command_parser)
        json_array_append_new(jn_methods, json_string("mt_command_parser"));
    if(gmt->mt_inject_event)
        json_array_append_new(jn_methods, json_string("mt_inject_event"));
    if(gmt->mt_create_resource)
        json_array_append_new(jn_methods, json_string("mt_create_resource"));
    if(gmt->mt_list_resource)
        json_array_append_new(jn_methods, json_string("mt_list_resource"));
    if(gmt->mt_save_resource)
        json_array_append_new(jn_methods, json_string("mt_save_resource"));
    if(gmt->mt_delete_resource)
        json_array_append_new(jn_methods, json_string("mt_delete_resource"));
    if(gmt->mt_future21)
        json_array_append_new(jn_methods, json_string("mt_future21"));
    if(gmt->mt_future22)
        json_array_append_new(jn_methods, json_string("mt_future22"));
    if(gmt->mt_get_resource)
        json_array_append_new(jn_methods, json_string("mt_get_resource"));
    if(gmt->mt_state_changed)
        json_array_append_new(jn_methods, json_string("mt_state_changed"));
    if(gmt->mt_authenticate)
        json_array_append_new(jn_methods, json_string("mt_authenticate"));
    if(gmt->mt_list_childs)
        json_array_append_new(jn_methods, json_string("mt_list_childs"));
    if(gmt->mt_stats_updated)
        json_array_append_new(jn_methods, json_string("mt_stats_updated"));
    if(gmt->mt_disable)
        json_array_append_new(jn_methods, json_string("mt_disable"));
    if(gmt->mt_enable)
        json_array_append_new(jn_methods, json_string("mt_enable"));
    if(gmt->mt_trace_on)
        json_array_append_new(jn_methods, json_string("mt_trace_on"));
    if(gmt->mt_trace_off)
        json_array_append_new(jn_methods, json_string("mt_trace_off"));
    if(gmt->mt_gobj_created)
        json_array_append_new(jn_methods, json_string("mt_gobj_created"));
    if(gmt->mt_future33)
        json_array_append_new(jn_methods, json_string("mt_future33"));
    if(gmt->mt_future34)
        json_array_append_new(jn_methods, json_string("mt_future34"));
    if(gmt->mt_publish_event)
        json_array_append_new(jn_methods, json_string("mt_publish_event"));
    if(gmt->mt_publication_pre_filter)
        json_array_append_new(jn_methods, json_string("mt_publication_pre_filter"));
    if(gmt->mt_publication_filter)
        json_array_append_new(jn_methods, json_string("mt_publication_filter"));
    if(gmt->mt_authz_checker)
        json_array_append_new(jn_methods, json_string("mt_authz_checker"));
    if(gmt->mt_future39)
        json_array_append_new(jn_methods, json_string("mt_future39"));
    if(gmt->mt_create_node)
        json_array_append_new(jn_methods, json_string("mt_create_node"));
    if(gmt->mt_update_node)
        json_array_append_new(jn_methods, json_string("mt_update_node"));
    if(gmt->mt_delete_node)
        json_array_append_new(jn_methods, json_string("mt_delete_node"));
    if(gmt->mt_link_nodes)
        json_array_append_new(jn_methods, json_string("mt_link_nodes"));
    if(gmt->mt_future44)
        json_array_append_new(jn_methods, json_string("mt_future44"));
    if(gmt->mt_unlink_nodes)
        json_array_append_new(jn_methods, json_string("mt_unlink_nodes"));
    if(gmt->mt_topic_jtree)
        json_array_append_new(jn_methods, json_string("mt_topic_jtree"));
    if(gmt->mt_get_node)
        json_array_append_new(jn_methods, json_string("mt_get_node"));
    if(gmt->mt_list_nodes)
        json_array_append_new(jn_methods, json_string("mt_list_nodes"));
    if(gmt->mt_shoot_snap)
        json_array_append_new(jn_methods, json_string("mt_shoot_snap"));
    if(gmt->mt_activate_snap)
        json_array_append_new(jn_methods, json_string("mt_activate_snap"));
    if(gmt->mt_list_snaps)
        json_array_append_new(jn_methods, json_string("mt_list_snaps"));
    if(gmt->mt_treedbs)
        json_array_append_new(jn_methods, json_string("mt_treedbs"));
    if(gmt->mt_treedb_topics)
        json_array_append_new(jn_methods, json_string("mt_treedb_topics"));
    if(gmt->mt_topic_desc)
        json_array_append_new(jn_methods, json_string("mt_topic_desc"));
    if(gmt->mt_topic_links)
        json_array_append_new(jn_methods, json_string("mt_topic_links"));
    if(gmt->mt_topic_hooks)
        json_array_append_new(jn_methods, json_string("mt_topic_hooks"));
    if(gmt->mt_node_parents)
        json_array_append_new(jn_methods, json_string("mt_node_parents"));
    if(gmt->mt_node_childs)
        json_array_append_new(jn_methods, json_string("mt_node_childs"));
    if(gmt->mt_list_instances)
        json_array_append_new(jn_methods, json_string("mt_list_instances"));
    if(gmt->mt_node_tree)
        json_array_append_new(jn_methods, json_string("mt_node_tree"));
    if(gmt->mt_topic_size)
        json_array_append_new(jn_methods, json_string("mt_topic_size"));
    if(gmt->mt_future62)
        json_array_append_new(jn_methods, json_string("mt_future62"));
    if(gmt->mt_future63)
        json_array_append_new(jn_methods, json_string("mt_future63"));
    if(gmt->mt_future64)
        json_array_append_new(jn_methods, json_string("mt_future64"));

    return jn_methods;
}

/***************************************************************************
 *  Print internal gclass's methods in json
 ***************************************************************************/
PRIVATE json_t *internalmethods2json(const LMETHOD *lmt)
{
    json_t *jn_methods = json_array();

    if(lmt) {
        while(lmt->lname) {
            json_array_append_new(jn_methods, json_string(lmt->lname));
            lmt++;
        }
    }

    return jn_methods;
}

/***************************************************************************
 *  Return a list with event flag strings
 ***************************************************************************/
PRIVATE json_t *eventflag2json(event_flag_t flag)
{
    json_t *jn_dict = json_object();

    const event_flag_names_t *ef = event_flag_names;
    while(ef->name) {
        if(flag & 0x01) {
            json_object_set_new(
                jn_dict,
                ef->name,
                json_string(ef->description?ef->description:"")
            );
        }
        flag = flag >> 1;
        ef++;
    }
    return jn_dict;
}

/***************************************************************************
 *  Return a list with event auth strings
 ***************************************************************************/
PRIVATE json_t *eventauth2json(event_authz_t authz)
{
    json_t *jn_dict = json_object();

    const trace_level_t *ef = event_authz_names;
    while(ef->name) {
        if(authz & 0x01) {
            json_object_set_new(
                jn_dict,
                ef->name,
                json_string(ef->description?ef->description:"")
            );
        }
        authz = authz >> 1;
        ef++;
    }
    return jn_dict;
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE json_t *events2json(const EVENT *events)
{
    json_t *jn_events = json_array();

    while(events->event) {
        json_t *jn_ev = json_object();
        json_object_set_new(jn_ev, "id", json_string(events->event));
        json_object_set_new(jn_ev, "flag", eventflag2json(events->flag));
        json_object_set_new(jn_ev, "authz", eventauth2json(events->authz));

        json_object_set_new(
            jn_ev,
            "description",
            events->description?json_string(events->description):json_string("")
        );

        json_array_append_new(jn_events, jn_ev);
        events++;
    }

    return jn_events;
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE json_t *states2json(FSM *fsm)
{
    json_t *jn_states = json_object();

    EV_ACTION **sts = fsm->states;
    const char **state_name = fsm->state_names;

    while(*sts && *state_name) {
        json_t *jn_st = json_array();

        EV_ACTION *st = *sts;
        while(st->event) {
            json_t *jn_ac = json_array();
            json_array_append_new(jn_ac, json_string(st->event));
            json_array_append_new(jn_ac, json_string("ac_action"));
            if(st->next_state) {
                json_array_append_new(jn_ac, json_string(st->next_state));
            } else {
                json_array_append_new(jn_ac, json_integer(0));
            }
            json_array_append_new(jn_st, jn_ac);
            st++;
        }
        json_object_set_new(
            jn_states,
            *state_name,
            jn_st
        );
        sts++;
        state_name++;
    }


    return jn_states;
}

/***************************************************************************
 *  Print gclass's fsm in json
 ***************************************************************************/
PRIVATE json_t *fsm2json(FSM *fsm)
{
    json_t *jn_fsm = json_object();

    json_object_set_new(
        jn_fsm,
        "input_events",
        events2json(fsm->input_events)
    );
    json_object_set_new(
        jn_fsm,
        "output_events",
        events2json(fsm->output_events)
    );
    json_object_set_new(
        jn_fsm,
        "states",
        states2json(fsm)
    );

    return jn_fsm;
}




                    /*---------------------------------*
                     *  SECTION: Info functions
                     *---------------------------------*/




/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_node_owner(void)
{
    return __node_owner__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_set_node_owner(const char *node_owner)
{
    if(empty_string(__node_owner__)) {
        snprintf(
            __node_owner__, sizeof(__node_owner__),
            "%s", node_owner?node_owner:""
        );
        return 0;
    }
    return -1;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_yuno_realm_id(void)
{
    return __realm_id__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_yuno_realm_owner(void)
{
    return __realm_owner__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_yuno_realm_role(void)
{
    return __realm_role__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_yuno_realm_name(void)
{
    return __realm_name__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_yuno_realm_env(void)
{
    return __realm_env__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_yuno_id(void)
{
    return __yuno_id__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_yuno_name(void)
{
    return __yuno_name__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_yuno_role(void)
{
    return __yuno_role__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_yuno_role_plus_name(void)
{
    return __yuno_role_plus_name__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_yuno_role_plus_id(void)
{
    return __yuno_role_plus_id__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const char *gobj_yuno_tag(void)
{
    return __yuno_tag__;
}

/***************************************************************************
 *  Return level
 ***************************************************************************/
PRIVATE int _gobj_level(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    int level = 0;
    while(gobj->__parent__) {
        level++;
        gobj = gobj->__parent__;
    }
    return level;
}

/***************************************************************************
 *  Debug print service register in json
 ***************************************************************************/
PUBLIC json_t *gobj_services(void)
{
    json_t *jn_register = json_array();

    service_register_t *srv_reg = dl_first(&dl_service);
    while(srv_reg) {
        if(srv_reg->service) {
            json_array_append_new(
                jn_register,
                json_string(srv_reg->service)
            );
        }

        srv_reg = dl_next(srv_reg);
    }

    return jn_register;
}

/***************************************************************************
 *  Return yuno, the grandfather
 ***************************************************************************/
PUBLIC hgobj gobj_yuno(void)
{
    if(!__yuno__ || __yuno__->obflag & obflag_destroyed) {
        return 0;
    }
    return __yuno__;
}

/***************************************************************************
 *  Return name
 ***************************************************************************/
PUBLIC const char * gobj_name(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    return gobj? gobj->name: "???";
}

/***************************************************************************
 *  Return mach name. Same as gclass name.
 ***************************************************************************/
PUBLIC const char * gobj_gclass_name(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    return (gobj && gobj->gclass)? gobj->gclass->gclass_name: "???";
}

/***************************************************************************
 *  Return gclass
 ***************************************************************************/
PUBLIC GCLASS * gobj_gclass(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    if(!gobj) {
        return 0;
    }
    return gobj->gclass;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_instances(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    if(!gobj) {
        return 0;
    }
    return gobj->gclass->__instances__;
}

/***************************************************************************
 *  Return full name
 ***************************************************************************/
PUBLIC const char * gobj_full_name(hgobj gobj_)
{
    GObj_t *gobj = gobj_;

    if(!gobj)
        return "???";

    if(!gobj->full_name) {
        #define SIZEBUFTEMP (8*1024)
        GObj_t *parent = gobj;
        char *bf = malloc(SIZEBUFTEMP);
        if(bf) {
            int ln;
            memset(bf, 0, SIZEBUFTEMP);
            while(parent) {
                char pp[256];
                const char *format;
                if(strlen(bf))
                    format = "%s`";
                else
                    format = "%s";
                snprintf(pp, sizeof(pp), format, gobj_short_name(parent));
                ln = strlen(pp);
                if(ln + strlen(bf) < SIZEBUFTEMP) {
                    memmove(bf+ln, bf, strlen(bf));
                    memmove(bf, pp, ln);
                }
                if(parent == parent->__parent__) {
                    print_error(
                        PEF_CONTINUE,
                        "ERROR YUNETA",
                        "infinite loop gobj_full_name(), gclass %s, gobj %s",
                        parent->gclass->gclass_name,
                        parent->name
                    );
                    break;
                }
                parent = parent->__parent__;
            }
            gobj->full_name = gbmem_strdup(bf);
            free(bf);
        }

    }
    return gobj->full_name;
}

/***************************************************************************
 *  Full name  with states of running/playing
 ***************************************************************************/
PUBLIC char *gobj_full_name2(hgobj gobj_, char *bf, int bfsize)
{
    GObj_t *gobj = gobj_;

    if(!gobj)
        return "???";

    GObj_t *parent = gobj;

    int ln;
    memset(bf, 0, bfsize);
    while(parent) {
        char pp[256];
        const char *format;
        if(strlen(bf))
            format = "%s(%c%c)`";
        else
            format = "%s(%c%c)";
        snprintf(pp, sizeof(pp), format,
            gobj_short_name(parent),
            parent->running?'R':'r',
            parent->playing?'P':'p'
        );
        ln = strlen(pp);
        if(ln + strlen(bf) < bfsize) {
            memmove(bf+ln, bf, strlen(bf));
            memmove(bf, pp, ln);
        }
        if(parent == parent->__parent__) {
            print_error(
                PEF_CONTINUE,
                "ERROR YUNETA",
                "infinite loop gobj_full_name(), gclass %s, gobj %s",
                parent->gclass->gclass_name,
                parent->name
            );
            break;
        }
        parent = parent->__parent__;
    }
    return bf;
}

/***************************************************************************
 *  Full tree bottom names with states of running/playing
 ***************************************************************************/
PUBLIC char *gobj_full_bottom_name(hgobj gobj_, char *bf_, int bfsize)
{
    GObj_t *gobj = gobj_;

    if(!gobj)
        return "???";

    GObj_t *bottom = gobj;
    char *bf = bf_;
    memset(bf, 0, bfsize);
    while(bottom && bfsize > 0) {
        const char *format;
        if(strlen(bf_))
            format = "`%s(%c%c)";
        else
            format = "%s(%c%c)";
        int written = snprintf(bf, bfsize, format,
            gobj_short_name(bottom),
            bottom->running?'R':'r',
            bottom->playing?'P':'p'
        );
        if(written > 0) {
            bf += written;
            bfsize -= written;
        } else {
            break;
        }
        bottom = bottom->bottom_gobj;

    }
    return bf_;
}

/***************************************************************************
 *  Return short name (gclass^name)
 ***************************************************************************/
PUBLIC const char * gobj_short_name(hgobj gobj_)
{
    GObj_t *gobj = gobj_;

    if(!gobj)
        return "???";

    if(!gobj->short_name) {
        char temp[256];
        snprintf(temp, sizeof(temp),
            "%s^%s",
            gobj_gclass_name(gobj),
            gobj_name(gobj)
        );
        gobj->short_name = gbmem_strdup(temp);
    }
    return gobj->short_name;
}

/***************************************************************************
 *  Return escaped short name (gclass-name)
 ***************************************************************************/
PUBLIC const char * gobj_escaped_short_name(hgobj gobj_)
{
    GObj_t *gobj = gobj_;

    if(!gobj)
        return "???";

    if(!gobj->escaped_short_name) {
        char temp[256];
        snprintf(temp, sizeof(temp),
            "%s-%s",
            gobj_gclass_name(gobj),
            gobj_name(gobj)
        );
        gobj->escaped_short_name = gbmem_strdup(temp);
    }
    return gobj->escaped_short_name;
}



/***************************************************************************
 *  Return snmp name
 ***************************************************************************/
PUBLIC const char * gobj_snmp_name(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    GObj_t *child = gobj_;

    if(!gobj)
        return "";

    if(gobj->oid_changed && gobj->poid) {
        gbmem_free((void *)gobj->poid);
        gobj->poid = 0;
    }
    if(!gobj->poid) {
        #define SIZEBUFTEMP (8*1024)
        char *bf = malloc(SIZEBUFTEMP);
        if(bf) {
            int ln;
            memset(bf, 0, SIZEBUFTEMP);
            while(child) {
                char pp[256];
                const char *format;
                if(strlen(bf))
                    format = "%d`";
                else
                    format = "%d";

                size_t idx = 1;
                if(child->__parent__) {
                    rc_child_index((rc_resource_t *)child->__parent__, (rc_resource_t *)child, &idx);
                }
                child->oid = idx;
                snprintf(pp, sizeof(pp), format, idx);
                ln = strlen(pp);
                if(ln + strlen(bf) < SIZEBUFTEMP) {
                    memmove(bf+ln, bf, strlen(bf));
                    memmove(bf, pp, ln);
                }
                if(child == child->__parent__) {
                    print_error(
                        PEF_CONTINUE,
                        "ERROR YUNETA",
                        "infinite loop gobj_snmp_name(), gclass %s, gobj %s",
                        child->gclass->gclass_name,
                        child->name
                    );
                    break;
                }
                child = child->__parent__;
            }
            gobj->poid = gbmem_strdup(bf);
            free(bf);
        }

    }
    gobj->oid_changed = 0;
    return gobj->poid;
}

/***************************************************************************
 *  True if gobj is volatil
 ***************************************************************************/
PUBLIC BOOL gobj_is_volatil(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    if(gobj->obflag & obflag_volatil) {
        return TRUE;
    }
    return FALSE;
}

/***************************************************************************
 *  Set as volatil
 ***************************************************************************/
PUBLIC int gobj_set_volatil(hgobj gobj_, BOOL set)
{
    GObj_t *gobj = gobj_;
    if(set) {
        gobj->obflag |= obflag_volatil;
    } else {
        gobj->obflag &= ~obflag_volatil;
    }

    return 0;
}

/***************************************************************************
 *  True if gobj is imminent_destroy
 ***************************************************************************/
PUBLIC BOOL gobj_is_imminent_destroy(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    if(gobj->obflag & obflag_imminent_destroy) {
        return TRUE;
    }
    return FALSE;
}

/***************************************************************************
 *  Set as imminent_destroy
 ***************************************************************************/
PUBLIC int gobj_set_imminent_destroy(hgobj gobj_, BOOL set)
{
    GObj_t *gobj = gobj_;
    if(set) {
        gobj->obflag |= obflag_imminent_destroy;
    } else {
        gobj->obflag &= ~obflag_imminent_destroy;
    }

    return 0;
}

/***************************************************************************
 *  True if gobj is of gclass gclass_name
 ***************************************************************************/
PUBLIC BOOL gobj_typeof_gclass(hgobj gobj, const char *gclass_name)
{
    if(strcasecmp(((GObj_t *)gobj)->gclass->gclass_name, gclass_name)==0)
        return TRUE;
    else
        return FALSE;
}

/***************************************************************************
 *  Is subclass of this gclass?
 ***************************************************************************/
PUBLIC BOOL gobj_typeof_subgclass(hgobj gobj_, const char *gclass_name)
{
    GObj_t * gobj = gobj_;
    GCLASS *gclass = gobj->gclass;

    while(gclass) {
        if(strcasecmp(gclass_name, gclass->gclass_name)==0) {
            return TRUE;
        }
        gclass = gclass->base;
    }

    return FALSE;
}

/***************************************************************************
 *  Is a inherited (bottom) of this gclass?
 ***************************************************************************/
PUBLIC BOOL gobj_typeof_inherited_gclass(hgobj gobj_, const char *gclass_name)
{
    GObj_t * gobj = gobj_;

    while(gobj) {
        if(strcasecmp(gclass_name, gobj->gclass->gclass_name)==0) {
            return TRUE;
        }
        gobj = gobj->bottom_gobj;
    }

    return FALSE;
}

/***************************************************************************
 *  Change state (internal use)
 ***************************************************************************/
PRIVATE BOOL _change_state(GObj_t * gobj, const char *new_state)
{
    SMachine_t * mach;
    register char **state_names;
    register int i;

    mach = gobj->mach;
    state_names = (char **)mach->fsm->state_names;

    for(i=0; *state_names!=0; i++, state_names++) {
        if(strcasecmp(*state_names, new_state)==0) {
            if(mach->current_state != i) {
                mach->last_state = mach->current_state;
                mach->current_state = i;
                if(__trace_gobj_event_monitor__(gobj)) {
                    monitor_state(gobj);
                }
                BOOL tracea = is_machine_tracing(gobj) && !is_machine_not_tracing(gobj);
                if(tracea) {
                    trace_machine(" 🔷 mach(%s%s^%s), new_st: %s",
                        (!gobj->running)?"!!":"",
                        gobj_gclass_name(gobj), gobj_name(gobj),
                        new_state
                    );
                }
                return TRUE;
            }
            return FALSE;
        }
    }
    log_error(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "new_state UNKNOWN",
        "new_state",    "%s", new_state,
        "cur_state",    "%s", gobj_current_state(gobj),
        NULL
    );
    return FALSE;
}

/***************************************************************************
 *  Change state
 ***************************************************************************/
PUBLIC BOOL gobj_change_state(hgobj gobj_, const char *new_state)
{
    GObj_t * gobj =  gobj_;

    if(!gobj) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj NULL",
            NULL
        );
        return FALSE;
    }
    if(gobj->obflag & obflag_destroyed) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "gobj DESTROYED",
            NULL
        );
        return FALSE;
    }

    BOOL state_changed = _change_state(gobj, new_state);

    if(state_changed && gobj_is_running(gobj)) {
        BOOL tracea_states = __trace_gobj_states__(gobj)?TRUE:FALSE;
        BOOL tracea = is_machine_tracing(gobj) && !is_machine_not_tracing(gobj);
        SMachine_t * mach = gobj->mach;

        if(tracea && tracea_states) {
            trace_machine("🔀🔀 mach(%s%s^%s), st(%d:%s%s%s)",
                (!gobj->running)?"!!":"",
                gobj_gclass_name(gobj), gobj_name(gobj),
                mach->current_state,
                On_Black RGreen,
                mach->fsm->state_names[mach->current_state],
                Color_Off
            );
        }

        json_t *kw_st = json_object();
        json_object_set_new(
            kw_st,
            "previous_state",
            json_string(mach->fsm->state_names[mach->last_state])
        );
        json_object_set_new(
            kw_st,
            "current_state",
            json_string(mach->fsm->state_names[mach->current_state])
        );

        if(gobj->gclass->gmt.mt_state_changed) {
            gobj->gclass->gmt.mt_state_changed(gobj, __EV_STATE_CHANGED__, kw_st);
        } else {
            gobj_publish_event(gobj, __EV_STATE_CHANGED__, kw_st);
        }
    }

    return state_changed;
}

/***************************************************************************
 *  Return current state
 ***************************************************************************/
PUBLIC const char *gobj_current_state(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    register const char **state_names = gobj->mach->fsm->state_names;
    register int current_state = gobj->mach->current_state;

    return *(state_names+current_state);
}

/***************************************************************************
 *  Return last state
 ***************************************************************************/
PUBLIC const char *gobj_last_state(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    register const char **state_names = gobj->mach->fsm->state_names;
    register int last_state = gobj->mach->last_state;

    return *(state_names+last_state);
}

/***************************************************************************
 *  Return true if gobj is in this `state`
 ***************************************************************************/
PUBLIC BOOL gobj_in_this_state(hgobj gobj, const char *state)
{
    if(strcasecmp(gobj_current_state(gobj), state)==0)
        return TRUE;
    return FALSE;
}

/***************************************************************************
 *  Compare current state with `state`.
 *  Return:
 *      < 0     current state is lower than `state`
 *      == 0    current state is `state`
 *      > 0     current state is higher than `state`.
 ***************************************************************************/
PUBLIC int gobj_cmp_current_state(hgobj gobj_, const char *state)
{
    GObj_t * gobj = gobj_;
    SMachine_t * mach = gobj->mach;
    register char **state_names = (char **)mach->fsm->state_names;
    register int cur = mach->current_state;
    register int i;

    for(i=0; *state_names!=0; i++, state_names++) {
        if(strcasecmp(*state_names, state)==0) {
            if(cur == i)
                return 0;
            else if(cur < i)
                return -1;
            else
                return +1;
        }
    }
    log_error(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "state UNKNOWN",
        "state",        "%s", state,
        NULL
    );
    return -1;
}

/***************************************************************************
 *  Return input event desc
 ***************************************************************************/
PUBLIC const EVENT *gobj_input_event(hgobj gobj_, const char *event)
{
    GObj_t *gobj = gobj_;
    const EVENT *events = gobj->mach->fsm->input_events;
    if(!events) {
        return 0;
    }
    for(int i=0; events->event!=0; i++, events++) {
        if(strcasecmp(events->event, event)==0) {
            return events;
        }
    }
    return 0;
}

/***************************************************************************
 *  Return output event desc
 ***************************************************************************/
PUBLIC const EVENT *gobj_output_event(hgobj gobj_, const char *event)
{
    GObj_t *gobj = gobj_;
    const EVENT *output_events = gobj->mach->fsm->output_events;

    /*
     *  Check gclass output events
     */
    if(output_events) {
        for(int i=0; output_events->event!=0; i++, output_events++) {
            if(strcasecmp(output_events->event, event)==0) {
                return output_events;
            }
        }
    }

    /*
     *  Check global (gobj) output events
     */
    output_events = global_output_events;
    for(int i=0; output_events->event!=0; i++, output_events++) {
        if(strcasecmp(output_events->event, event)==0) {
            return output_events;
        }
    }

    return 0;
}

/***************************************************************************
 *  Return TRUE if gobj supports event
 ***************************************************************************/
PUBLIC BOOL gobj_event_in_input_event_list(hgobj gobj_, const char *event, event_flag_t flag)
{
    GObj_t *gobj = gobj_;
    const EVENT *input_events = gobj->mach->fsm->input_events;
    if(!input_events) {
        return FALSE;
    }
    for(int i=0; input_events[i].event!=0; i++) {
        if(flag) {
            if(!(input_events[i].flag & flag)) {
                continue;
            }
        }
        if(strcasecmp(input_events[i].event, event)==0) {
            return TRUE;
        }
    }
    return FALSE;
}

/***************************************************************************
 *  Return TRUE if gobj supports event
 ***************************************************************************/
PUBLIC BOOL gobj_event_in_output_event_list(hgobj gobj_, const char *event, event_flag_t flag)
{
    GObj_t *gobj = gobj_;
    const EVENT *output_events = gobj->mach->fsm->output_events;
    if(!output_events) {
        return FALSE;
    }
    for(int i=0; output_events[i].event!=0; i++) {
        if(flag) {
            if(!(output_events[i].flag & flag)) {
                continue;
            }
        }
        if(strcasecmp(output_events[i].event, event)==0) {
            return TRUE;
        }
    }
    return FALSE;
}

/***************************************************************************
 *  Return private data pointer
 ***************************************************************************/
PUBLIC void * gobj_priv_data(hgobj gobj)
{
    return ((GObj_t *)gobj)->priv;
}

/***************************************************************************
 *  Return parent
 ***************************************************************************/
PUBLIC hgobj gobj_parent(hgobj gobj)
{
    return ((GObj_t *)gobj)->__parent__;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC BOOL gobj_is_destroying(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    if(!gobj_ || gobj->obflag & (obflag_destroyed|obflag_destroying)) {
        return TRUE;
    }
    return FALSE;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC BOOL gobj_is_running(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    if(!gobj_ || gobj->obflag & (obflag_destroyed|obflag_destroying)) {
        log_error(LOG_OPT_TRACE_STACK,
          "gobj",         "%s", __FILE__,
          "function",     "%s", __FUNCTION__,
          "msgset",       "%s", MSGSET_PARAMETER_ERROR,
          "msg",          "%s", "hgobj NULL or DESTROYED",
          NULL
        );
        return FALSE;
    }
    return gobj->running;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC BOOL gobj_is_playing(hgobj gobj_)
{
    GObj_t *gobj = gobj_;

    if(!gobj_ || gobj->obflag & (obflag_destroyed|obflag_destroying)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        return FALSE;
    }
    return gobj->playing;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC BOOL gobj_is_service(hgobj gobj_)
{
    GObj_t *gobj = gobj_;

    if(gobj && (gobj->obflag & (obflag_yuno|obflag_default_service|obflag_service))) {
        return TRUE;
    } else {
        return FALSE;
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC BOOL gobj_is_disabled(hgobj gobj)
{
    return ((GObj_t *)gobj)->disabled;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC BOOL gobj_is_unique(hgobj gobj_)
{
    GObj_t * gobj = gobj_;
    if(gobj->obflag & obflag_unique_name) {
        return TRUE;
    } else {
        return FALSE;
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *build_webix(
    json_int_t result,
    json_t *jn_comment, // owned
    json_t *jn_schema,  // owned
    json_t *jn_data)    // owned
{
    if(!jn_comment) {
        jn_comment = json_string("");
    }
    if(!jn_schema) {
        jn_schema = json_null();
    }
    if(!jn_data) {
        jn_data = json_null();
    }

    const char *comment = json_string_value(jn_comment);
    json_t *webix = json_pack("{s:I, s:s, s:o, s:o}",
        "result", (json_int_t)result,
        "comment", comment?comment:"",
        "schema", jn_schema,
        "data", jn_data
    );
    JSON_DECREF(jn_comment);
    if(!webix) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "build webix FAILED",
            NULL
        );
    }
    return webix;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int append_yuno_metadata(hgobj gobj, json_t *kw, const char *source)
{
    static char hostname[80];

    if(!kw) {
        return -1;
    }

    if(!*hostname) {
        memset(hostname, 0, sizeof(hostname));
        gethostname(hostname, sizeof(hostname)-1);
    }

    time_t t;
    time(&t);

    if(!source) {
        source = "";
    }
    json_t *jn_metadatos = json_object();
    json_object_set_new(jn_metadatos, "__t__", json_integer(t));
    json_object_set_new(jn_metadatos, "__origin__", json_string(source));
    json_object_set_new(jn_metadatos, "hostname", json_string(hostname));
    json_object_set_new(jn_metadatos, "node_owner", json_string(__node_owner__));
    json_object_set_new(jn_metadatos, "realm_id", json_string(__realm_id__));
    json_object_set_new(jn_metadatos, "realm_owner", json_string(__realm_owner__));
    json_object_set_new(jn_metadatos, "realm_role", json_string(__realm_role__));
    json_object_set_new(jn_metadatos, "realm_name", json_string(__realm_name__));
    json_object_set_new(jn_metadatos, "realm_env", json_string(__realm_env__));
    json_object_set_new(jn_metadatos, "yuno_id", json_string(__yuno_id__));
    json_object_set_new(jn_metadatos, "yuno_role", json_string(__yuno_role__));
    json_object_set_new(jn_metadatos, "yuno_name", json_string(__yuno_name__));
    json_object_set_new(jn_metadatos, "gobj_name", json_string(gobj_short_name(gobj)));
    json_object_set_new(jn_metadatos, "pid", json_integer(getpid()));
    json_object_set_new(kw, "__md_yuno__", jn_metadatos);

    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_stats(hgobj gobj_, const char *stats, json_t *kw, hgobj src)
{
    GObj_t * gobj = gobj_;

    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }

    /*--------------------------------------*
     *  The local mt_stats has preference
     *--------------------------------------*/
    if(gobj->gclass->gmt.mt_stats) {
        return gobj->gclass->gmt.mt_stats(gobj, stats, kw, src);
    }

    /*-----------------------------------------------*
     *  Then use the global stats parser
     *-----------------------------------------------*/
    if(__global_stats_parser_fn__) {
        return __global_stats_parser_fn__(gobj, stats, kw, src);
    } else {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "Stats parser not available",
            "stats",        "%s", stats?stats:"",
            NULL
        );
        KW_DECREF(kw);
    }
    return 0;
}

/***************************************************************************
 *  Set stats, path relative to gobj, including the attribute
 ***************************************************************************/
PUBLIC int gobj_set_stats(
    hgobj gobj,
    const char* path,
    uint32_t set
)
{
    hgobj child = gobj_find_tree_path(gobj, path);
    if(!child) {
        return -1;
    }
    const char *attr = strrchr(path, '/');
    if(attr) {
        attr++;
    } else {
        attr = path;
    }

    sdata_set_stats_metadata(gobj_hsdata(child), attr, 0, set);
    return 0;
}

/***************************************************************************
 *  Check stats, path relative to gobj, including the attribute
 ***************************************************************************/
PUBLIC BOOL gobj_has_stats(
    hgobj gobj,
    const char* path
)
{
    hgobj child = gobj_find_tree_path(gobj, path);
    if(!child) {
        return FALSE;
    }
    const char *attr = strrchr(path, '/');
    if(attr) {
        attr++;
    } else {
        attr = path;
    }

    uint32_t md = sdata_get_stats_metadata(gobj_hsdata(child), attr);
    return md?TRUE:FALSE;
}

/***************************************************************************
 *  Execute a command of gclass command table
 ***************************************************************************/
PUBLIC json_t *gobj_command( // With AUTHZ
    hgobj gobj_,
    const char *command,
    json_t *kw,
    hgobj src
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }

    /*-----------------------------------------------*
     *  Trace
     *-----------------------------------------------*/
    BOOL tracea = is_machine_tracing(gobj) && !is_machine_not_tracing(src);
    if(tracea) {
        trace_machine("🌀🌀 mach(%s%s), cmd: %s, src: %s",
            (!gobj->running)?"!!":"",
            gobj_short_name(gobj),
            command,
            gobj_short_name(src)
        );
        log_debug_json(0, kw, "command kw");
    }

    /*-----------------------------------------------*
     *  The local mt_command_parser has preference
     *-----------------------------------------------*/
    if(gobj->gclass->gmt.mt_command_parser) {
        if(__audit_command_cb__) {
            __audit_command_cb__(command, kw, __audit_command_user_data__);
        }
        return gobj->gclass->gmt.mt_command_parser(gobj, command, kw, src);
    }

    /*-----------------------------------------------*
     *  If it has command_table
     *  then use the global command parser
     *-----------------------------------------------*/
    if(gobj->gclass->command_table) {
        if(__audit_command_cb__) {
            __audit_command_cb__(command, kw, __audit_command_user_data__);
        }
        if(__global_command_parser_fn__) {
            return __global_command_parser_fn__(gobj, command, kw, src);
        } else {
            log_error(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", gobj_full_name(gobj),
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "Global command parser function not available",
                "command",      "%s", command?command:"",
                NULL
            );
            KW_DECREF(kw);
            return 0;
        }
    } else {
        return msg_iev_build_webix(
            gobj,
            -1,
            json_sprintf(
                "Command table not available in '%s' gobj",
                gobj_short_name(gobj)
            ),
            0,
            0,
            kw
        );
    }
}

/***************************************************************************
 *  Get the command desc of gclass command table
 ***************************************************************************/
PUBLIC const sdata_desc_t *gobj_get_command_desc(
    GCLASS * gclass,
    const char *command
)
{
    if(!gclass) {
        return 0;
    }
    if(gclass->command_table) {
        return command_get_cmd_desc(gclass->command_table, command);
    }
    return 0;
}

/***************************************************************************
 *  Return a list with dicc of child's attrs of typeof inherited gclass
 *  Simulate table with childs
 ***************************************************************************/
PUBLIC json_t *gobj_list_childs(
    hgobj gobj_,
    const char *child_gclass,
    const char **attributes
)
{
    GObj_t * gobj = gobj_;

    if(!gobj || gobj->obflag & obflag_destroyed) {
        return 0;
    }

    /*-----------------------------------------------*
     *  Preference the mt_list_childs of gobj
     *-----------------------------------------------*/
    if(gobj->gclass->gmt.mt_list_childs) {
        return gobj->gclass->gmt.mt_list_childs(gobj, child_gclass, attributes);
    }

    /*-----------------------------------------------*
     *  Built-in child list
     *-----------------------------------------------*/
    json_t *jn_data = json_array();

    hgobj _child; rc_instance_t *i_child;
    i_child = gobj_first_child(gobj, &_child);
    while(i_child) {
        GObj_t *child = _child;
        if(!gobj_typeof_inherited_gclass(child, child_gclass)) {
            i_child = gobj_next_child(i_child, &_child);
            continue;
        }
        json_t *jn_dict = json_object();
        json_object_set_new(jn_dict, "id", json_string(gobj_full_name(child)));

        const char **attr = attributes;
        while(*attr) {
            const char *attr_name = *attr;
            BOOL found = FALSE;
            if(gobj_has_bottom_attr(child, attr_name)) {
                const sdata_desc_t *it = gobj_bottom_attr_desc(child, attr_name);
                if(it->flag & (SDF_RD|SDF_WR|SDF_STATS|SDF_PERSIST|SDF_VOLATIL|SDF_RSTATS|SDF_PSTATS)) {
                    if(ASN_IS_REAL_NUMBER(it->type)) {
                        found = TRUE;
                        json_object_set_new(
                            jn_dict,
                            attr_name,
                            json_real(
                                gobj_read_real_attr(child, attr_name)
                            )
                        );
                    } else if(ASN_IS_NATURAL_NUMBER(it->type)) {
                        found = TRUE;
                        json_object_set_new(
                            jn_dict,
                            attr_name,
                            json_integer(
                                gobj_read_integer_attr(child, attr_name)
                            )
                        );
                    } else if(ASN_IS_STRING(it->type)) {
                        found = TRUE;
                        json_object_set_new(
                            jn_dict,
                            attr_name,
                            json_string(
                                gobj_read_str_attr(child, attr_name)
                            )
                        );
                    } else if(ASN_IS_JSON(it->type)) {
                        found = TRUE;
                        json_t *jn = gobj_read_json_attr(child, attr_name);
                        char *value = 0;
                        if(jn) {
                            value = json_dumps(jn, JSON_ENCODE_ANY|JSON_COMPACT);
                        }
                        json_object_set_new(
                            jn_dict,
                            attr_name,
                            json_string(value?value:"")
                        );
                        if(value) {
                            gbmem_free(value);
                        }
                    }
                }
            }
            if(!found) {
                json_object_set_new(
                    jn_dict,
                    attr_name,
                    json_null()
                );
            }
            attr++;
        }

        json_object_set_new(jn_dict, "__name__", json_string(gobj_name(child)));
        json_object_set_new(jn_dict, "__state__", json_string(gobj_current_state(child)));
        json_object_set_new(jn_dict, "__parent__", json_string(gobj_name(gobj_parent(child))));
        json_object_set_new(jn_dict, "__trace_level__", json_integer(gobj_trace_level(child)));
        json_object_set_new(jn_dict, "__running__", json_integer(gobj_is_running(child)));
        json_object_set_new(jn_dict, "__playing__", json_integer(gobj_is_playing(child)));
        json_object_set_new(jn_dict, "__service__", json_integer(gobj_is_service(child)));
        json_object_set_new(jn_dict, "__disabled__", json_integer(gobj_is_disabled(child)));

        json_array_append_new(jn_data, jn_dict);

        i_child = gobj_next_child(i_child, &_child);
    }

    return jn_data;
}

/***************************************************************************
 *  Audit commands
 *  Only one can audit. New calls will overwrite audit_command_cb.
 ***************************************************************************/
PUBLIC int gobj_audit_commands(
    int (*audit_command_cb)(const char *command, json_t *kw, void *user_data),
    void *user_data
){
    __audit_command_cb__ = audit_command_cb;
    __audit_command_user_data__ = user_data;
    return 0;
}

/***************************************************************************
 *  Return a dict with gclass's public attrs (all if null).
 ***************************************************************************/
PUBLIC json_t *gclass_public_attrs(GCLASS *gclass)
{
    json_t *jn_dict = json_object();
    if(gclass) {
        json_object_set_new(
            jn_dict,
            gclass->gclass_name,
            sdatadesc2json(
                gclass->tattr_desc,
                SDF_PUBLIC_ATTR,
                0
            )
        );
    } else {
        gclass_register_t *gclass_reg = dl_first(&dl_gclass);
        while(gclass_reg) {
            if(gclass_reg->gclass) {
                json_object_set_new(
                    jn_dict,
                    gclass_reg->gclass->gclass_name,
                    sdatadesc2json(
                        gclass_reg->gclass->tattr_desc,
                        SDF_PUBLIC_ATTR,
                        0
                    )
                );
            }
            gclass_reg = dl_next(gclass_reg);
        }
    }

    return jn_dict;
}

/***************************************************************************
 *  Return json object with gclass's description.
 ***************************************************************************/
PUBLIC json_t *gclass2json(GCLASS *gclass)
{
    json_t *jn_dict = json_object();
    if(!gclass) {
        return jn_dict;
    }
    json_object_set_new(
        jn_dict,
        "id",
        json_string(gclass->gclass_name)
    );

    json_object_set_new(
        jn_dict,
        "base",
        json_string(gclass->base?gclass->base->gclass_name:"")
    );
    json_object_set_new(
        jn_dict,
        "gcflag",
        gcflag2json(gclass)
    );
    json_object_set_new(
        jn_dict,
        "priv_size",
        json_integer(gclass->priv_size)
    );
    json_object_set_new(
        jn_dict,
        "attrs",
        sdatadesc2json2(gclass->tattr_desc, -1, 0)
    );
    json_object_set_new(
        jn_dict,
        "commands",
        sdatacmd2json(gclass->command_table)
    );
    json_object_set_new(
        jn_dict,
        "gclass_methods",
        yunetamethods2json(&gclass->gmt)
    );
    json_object_set_new(
        jn_dict,
        "internal_methods",
        internalmethods2json(gclass->lmt)
    );
    json_object_set_new(
        jn_dict,
        "FSM",
        fsm2json(gclass->fsm)
    );
    json_object_set_new(
        jn_dict,
        "Authzs global",
        sdataauth2json(global_authz_table)
    );
    json_object_set_new(
        jn_dict,
        "Authzs gclass", // Access Control List
        sdataauth2json(gclass->authz_table)
    );

    json_object_set_new(
        jn_dict,
        "info_global_trace",
        gobj_trace_level_list2(gclass, TRUE, FALSE)
    );

    json_object_set_new(
        jn_dict,
        "info_gclass_trace",
        gobj_trace_level_list2(gclass, FALSE, TRUE)
    );

    json_object_set_new(
        jn_dict,
        "gclass_trace_level",
        bit2level(
            s_global_trace_level,
            gclass->s_user_trace_level,
            gclass->__gclass_trace_level__
        )
    );

    json_object_set_new(
        jn_dict,
        "gclass_no_trace_level",
        bit2level(
            s_global_trace_level,
            gclass->s_user_trace_level,
            gclass->__gclass_no_trace_level__
        )
    );

    json_object_set_new(
        jn_dict,
        "instances",
        json_integer(gclass->__instances__)
    );

    return jn_dict;
}

/***************************************************************************
 *  Return a list with gcflag's strings.
 ***************************************************************************/
PUBLIC json_t *gcflag2json(GCLASS *gclass)
{
    json_t *jn_list = json_array();
    for(int i=0; i<sizeof(gclass->gcflag); i++) {
        if(!s_gcflag[i].name) {
            break;
        }
        uint32_t bitmask = 1 << i;
        if(gclass->gcflag & bitmask) {
            json_array_append_new(
                jn_list,
                json_string(s_gcflag[i].name)
            );
        }
    }
    return jn_list;
}

/***************************************************************************
 *  Return a dict with gobj's description.
 ***************************************************************************/
PUBLIC json_t *gobj2json(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    json_t *jn_dict = json_object();

    json_object_set_new( // Webix id
        jn_dict,
        "id",
        json_string(gobj_snmp_name(gobj))
    );
    json_object_set_new(
        jn_dict,
        "name",
        json_string(gobj->name)
    );
    json_object_set_new(
        jn_dict,
        "shortname",
        json_string(gobj_short_name(gobj))
    );
    json_object_set_new(
        jn_dict,
        "fullname",
        json_string(gobj_full_name(gobj))
    );
    json_object_set_new(
        jn_dict,
        "parent",
        json_string(gobj_short_name(gobj_parent(gobj)))
    );

    json_object_set_new(
        jn_dict,
        "GClass",
        json_string(gobj->gclass->gclass_name)
    );
    json_object_set_new(
        jn_dict,
        "attrs",
        sdata2json(gobj_hsdata(gobj), SDF_PUBLIC_ATTR, 0)
    );

    if(gobj->jn_user_data) {
        json_object_set(
            jn_dict,
            "user_data",
            gobj->jn_user_data
        );
    }

    json_object_set_new(
        jn_dict,
        "state",
        json_string(gobj_current_state(gobj))
    );
    json_object_set_new(
        jn_dict,
        "running",
        gobj->running? json_true(): json_false()
    );
    json_object_set_new(
        jn_dict,
        "playing",
        gobj->playing? json_true(): json_false()
    );
    json_object_set_new(
        jn_dict,
        "service",
        gobj_is_service(gobj)? json_true(): json_false()
    );
    json_object_set_new(
        jn_dict,
        "unique",
        gobj_is_unique(gobj)? json_true(): json_false()
    );
    json_object_set_new(
        jn_dict,
        "disabled",
        gobj->disabled? json_true(): json_false()
    );
    json_object_set_new(
        jn_dict,
        "gobj_trace_level",
        json_integer(gobj_trace_level(gobj))
    );
    json_object_set_new(
        jn_dict,
        "gobj_no_trace_level",
        json_integer(gobj_no_trace_level(gobj))
    );

    return jn_dict;
}

/***************************************************************************
 *
 *  Return list of objects with gobj's attribute description,
 *  restricted to attributes having SDF_RD|SDF_WR|SDF_STATS|SDF_PERSIST|SDF_VOLATIL|SDF_RSTATS|SDF_PSTATS flag.
 *
 *  Esquema tabla webix:

    data:[
        {
            id: integer (index)
            name: string,
            type: string ("real" | "boolean" | "integer" | "string" | "json"),
            flag: string ("SDF_RD",...),
            description: string,
            stats: boolean,
            value: any
        },
        {   Metadata:
            "name", "__state__",
            "name", "__bottom__",
            "name", "__shortname__",
            "name", "__fullname__",
            "name", "__running__",
            "name", "__playing__",
            "name", "__service__",
            "name", "__unique__",
            "name", "__disabled__",
            "name", "__gobj_trace_level__",
            "name", "__gobj_no_trace_level__"
        }
    ]
 ***************************************************************************/
PUBLIC json_t *attr2json(hgobj gobj_)
{
    GObj_t * gobj = gobj_;
    json_t *jn_data = json_array();

    int id = 1;
    const sdata_desc_t *sdesc = sdata_schema(gobj_hsdata(gobj));
    const sdata_desc_t *it = sdesc;
    while(it && it->name) {
        if(it->flag & (SDF_RD|SDF_WR|SDF_STATS|SDF_PERSIST|SDF_VOLATIL|SDF_RSTATS|SDF_PSTATS)) {
            char *type;
            if(ASN_IS_REAL_NUMBER(it->type)) {
                type = "real";
            } else if(ASN_IS_BOOLEAN(it->type)) {
                type = "boolean";
            } else if(ASN_IS_NATURAL_NUMBER(it->type)) {
                type = "integer";
            } else if(ASN_IS_STRING(it->type)) {
                type = "string";
            } else if(ASN_IS_JSON(it->type)) {
                type = "json";
            } else {
                it++;
                continue;
            }

            GBUFFER *gbuf = get_sdata_flag_desc(it->flag);
            char *flag = gbuf?gbuf_cur_rd_pointer(gbuf):"";
            json_t *attr_dict = json_pack("{s:I, s:s, s:s, s:s, s:s, s:b}",
                "id", (json_int_t)id++,
                "name", it->name,
                "type", type,
                "flag", flag,
                "description", it->description?it->description:"",
                "stats", (it->flag & (SDF_STATS|SDF_RSTATS|SDF_PSTATS))?1:0
            );
            GBUF_DECREF(gbuf);

            if(ASN_IS_REAL_NUMBER(it->type)) {
                json_object_set_new(
                    attr_dict,
                    "value",
                    json_real(
                        gobj_read_real_attr(gobj, it->name)
                    )
                );
            } else if(ASN_IS_NATURAL_NUMBER(it->type)) {
                json_object_set_new(
                    attr_dict,
                    "value",
                    json_integer(
                        gobj_read_integer_attr(gobj, it->name)
                    )
                );
            } else if(ASN_IS_STRING(it->type)) {
                json_object_set_new(
                    attr_dict,
                    "value",
                    json_string(
                        gobj_read_str_attr(gobj, it->name)
                    )
                );
            } else if(ASN_IS_JSON(it->type)) {
                json_t *jn = gobj_read_json_attr(gobj, it->name);
                char *value = 0;
                if(jn) {
                    value = json_dumps(jn, JSON_ENCODE_ANY|JSON_COMPACT);
                }
                json_object_set_new(
                    attr_dict,
                    "value",
                    json_string(value?value:"")
                );
                if(value) {
                    gbmem_free(value);
                }
            }

            json_array_append_new(jn_data, attr_dict);
        }
        it++;
    }

    json_array_append_new(jn_data,
        json_pack("{s:I, s:s, s:s, s:s, s:s, s:b, s:s}",
            "id", (json_int_t)id++,
            "name", "__state__",
            "type", "string",
            "flag", "",
            "description", "SMachine'state of gobj",
            "stats", 0,
            "value", gobj_current_state(gobj)
        )
    );
    json_array_append_new(jn_data,
        json_pack("{s:I, s:s, s:s, s:s, s:s, s:b, s:s}",
            "id", (json_int_t)id++,
            "name", "__bottom__",
            "type", "string",
            "flag", "",
            "description", "Bottom gobj",
            "stats", 0,
            "value", gobj->bottom_gobj? gobj_short_name(gobj->bottom_gobj):""
        )
    );
    json_array_append_new(jn_data,
        json_pack("{s:I, s:s, s:s, s:s, s:s, s:b, s:s}",
            "id", (json_int_t)id++,
            "name", "__shortname__",
            "type", "string",
            "flag", "",
            "description", "Full name",
            "stats", 0,
            "value", gobj_short_name(gobj)
        )
    );
    json_array_append_new(jn_data,
        json_pack("{s:I, s:s, s:s, s:s, s:s, s:b, s:s}",
            "id", (json_int_t)id++,
            "name", "__fullname__",
            "type", "string",
            "flag", "",
            "description", "Full name",
            "stats", 0,
            "value", gobj_full_name(gobj)
        )
    );
    json_array_append_new(jn_data,
        json_pack("{s:I, s:s, s:s, s:s, s:s, s:b, s:I}",
            "id", (json_int_t)id++,
            "name", "__running__",
            "type", "boolean",
            "flag", "",
            "description", "Is running the gobj?",
            "stats", 0,
            "value", (json_int_t)(size_t)gobj_is_running(gobj)
        )
    );
    json_array_append_new(jn_data,
        json_pack("{s:I, s:s, s:s, s:s, s:s, s:b, s:I}",
            "id", (json_int_t)id++,
            "name", "__playing__",
            "type", "boolean",
            "flag", "",
            "description", "Is playing the gobj?",
            "stats", 0,
            "value", (json_int_t)(size_t)gobj_is_playing(gobj)
        )
    );
    json_array_append_new(jn_data,
        json_pack("{s:I, s:s, s:s, s:s, s:s, s:b, s:I}",
            "id", (json_int_t)id++,
            "name", "__service__",
            "type", "boolean",
            "flag", "",
            "description", "Is a service the gobj?",
            "stats", 0,
            "value", (json_int_t)(size_t)gobj_is_service(gobj)
        )
    );
    json_array_append_new(jn_data,
        json_pack("{s:I, s:s, s:s, s:s, s:s, s:b, s:I}",
            "id", (json_int_t)id++,
            "name", "__unique__",
            "type", "boolean",
            "flag", "",
            "description", "Is unique the gobj?",
            "stats", 0,
            "value", (json_int_t)(size_t)gobj_is_unique(gobj)
        )
    );
    json_array_append_new(jn_data,
        json_pack("{s:I, s:s, s:s, s:s, s:s, s:b, s:I}",
            "id", (json_int_t)id++,
            "name", "__disabled__",
            "type", "boolean",
            "flag", "",
            "description", "Is disabled the gobj?",
            "stats", 0,
            "value", (json_int_t)(size_t)gobj_is_disabled(gobj)
        )
    );
    json_array_append_new(jn_data,
        json_pack("{s:I, s:s, s:s, s:s, s:s, s:b, s:o}",
            "id", (json_int_t)id++,
            "name", "__gobj_trace_level__",
            "type", "array",
            "flag", "",
            "description", "Current trace level",
            "stats", 0,
            "value",  bit2level(
                s_global_trace_level,
                gobj->gclass->s_user_trace_level,
                gobj_trace_level(gobj)
            )
        )
    );

    json_array_append_new(jn_data,
        json_pack("{s:I, s:s, s:s, s:s, s:s, s:b, s:o}",
            "id", (json_int_t)id++,
            "name", "__gobj_no_trace_level__",
            "type", "array",
            "flag", "",
            "description", "Current no trace level",
            "stats", 0,
            "value",  bit2level(
                s_global_trace_level,
                gobj->gclass->s_user_trace_level,
                gobj_no_trace_level(gobj)
            )
        )
    );

    return jn_data;
}

/***************************************************************************
 *  View gclasses of the gobj's tree.
 *  Esquema tree webix:

        [
            {
                id: "snmp-id",
                value: "short-name",
                fullname: "full-name",
                data: [ // CHILDS
                    {
                        id: "snmp-name",
                        value: "short-name"
                        fullname: "full-name",
                        data: [ // CHILDS
                        ]
                    },
                    ...
                ]
            },
            ...
        ];

 ***************************************************************************/
PRIVATE int _add_webix_gobj(json_t *jn_list, GObj_t * gobj)
{
    json_t *jn_service = json_pack("{s:s, s:s, s:s}",
        "id", gobj_snmp_name(gobj),
        "value", gobj_short_name(gobj),
        "fullname", gobj_full_name(gobj)
    );
    json_array_append_new(jn_list, jn_service);

    if(gobj_child_size(gobj)>0) {
        json_t *jn_data = json_array();
        json_object_set_new(jn_service, "data", jn_data);

        GObj_t * child; rc_instance_t *i_child;
        i_child = gobj_first_child(gobj, (hgobj *)&child);

        while(i_child) {
            _add_webix_gobj(jn_data, child);
            i_child = gobj_next_child(i_child, (hgobj *)&child);
        }
    }
    return 0;
}
PUBLIC json_t *webix_gobj_tree(hgobj gobj)
{
    json_t *jn_tree = json_array();
    _add_webix_gobj(jn_tree, gobj);
    return jn_tree;
}

/***************************************************************************
 *  View a gobj tree, with states of running/playing and attributes
 ***************************************************************************/
PRIVATE int _add_gobj_tree(json_t *jn_list, GObj_t * gobj)
{
    json_t *jn_service = gobj2json(gobj);
    json_array_append_new(jn_list, jn_service);

    if(gobj_child_size(gobj)>0) {
        json_t *jn_data = json_array();
        json_object_set_new(jn_service, "childs", jn_data);

        GObj_t * child; rc_instance_t *i_child;
        i_child = gobj_first_child(gobj, (hgobj *)&child);

        while(i_child) {
            _add_gobj_tree(jn_data, child);
            i_child = gobj_next_child(i_child, (hgobj *)&child);
        }
    }
    return 0;
}
PUBLIC json_t *view_gobj_tree(hgobj gobj)
{
    json_t *jn_tree = json_array();
    _add_gobj_tree(jn_tree, gobj);
    return jn_tree;
}

/***************************************************************************
 *  Return list with child gobj's with gclass_name gclass
 *  with states of running/playing and attributes
 ***************************************************************************/
PRIVATE int _add_gclass_gobjs(json_t *jn_list, GObj_t * gobj, const char *gclass_name)
{
    if(!empty_string(gclass_name) && strcasecmp(gobj->gclass->gclass_name, gclass_name)==0) {
        json_t *jn_gobj = gobj2json(gobj);
        json_array_append_new(jn_list, jn_gobj);
    }

    if(gobj_child_size(gobj)>0) {
        GObj_t * child; rc_instance_t *i_child;
        i_child = gobj_first_child(gobj, (hgobj *)&child);

        while(i_child) {
            _add_gclass_gobjs(jn_list, child, gclass_name);
            i_child = gobj_next_child(i_child, (hgobj *)&child);
        }
    }
    return 0;
}
PUBLIC json_t *list_gclass_gobjs(hgobj gobj, const char *gclass_name)
{
    json_t *jn_tree = json_array();
    _add_gclass_gobjs(jn_tree, gobj, gclass_name);
    return jn_tree;
}

/***************************************************************************
 *  Return is NOT YOURS
 ***************************************************************************/
PUBLIC json_t *gobj_gobjs_treedb_schema(const char *topic_name)
{
    if(empty_string(topic_name)) {
        return jn_treedb_schema_gobjs;
    }
    json_t *topics = kw_get_list(jn_treedb_schema_gobjs, "topics", 0, KW_REQUIRED);

    int idx; json_t *topic;
    json_array_foreach(topics, idx, topic) {
        const char *topic_name_ = kw_get_str(topic, "topic_name", "", KW_REQUIRED);
        if(strcmp(topic_name, topic_name_)==0) {
            return topic;
        }
    }

    return 0;
}

/***************************************************************************
 *  To use in treedb style
 ***************************************************************************/
PRIVATE int _add_gobj_treedb_data(json_t *jn_list, json_t *parent, GObj_t *gobj)
{
    json_t *jn_dict = json_object();
    json_array_append_new(jn_list, jn_dict);

    json_object_set_new( // Webix id
        jn_dict,
        "id",
        json_string(gobj_snmp_name(gobj))
    );
    json_object_set_new(
        jn_dict,
        "name",
        json_string(gobj->name)
    );
    json_object_set_new(
        jn_dict,
        "shortname",
        json_string(gobj_short_name(gobj))
    );
    json_object_set_new(
        jn_dict,
        "fullname",
        json_string(gobj_full_name(gobj))
    );

    json_object_set_new(
        jn_dict,
        "gclass_name",
        json_string(gobj->gclass->gclass_name)
    );
    json_object_set_new(
        jn_dict,
        "running",
        gobj->running? json_true(): json_false()
    );
    json_object_set_new(
        jn_dict,
        "playing",
        gobj->playing? json_true(): json_false()
    );
    json_object_set_new(
        jn_dict,
        "service",
        gobj_is_service(gobj)? json_true(): json_false()
    );
    json_object_set_new(
        jn_dict,
        "unique",
        gobj_is_unique(gobj)? json_true(): json_false()
    );
    json_object_set_new(
        jn_dict,
        "disabled",
        gobj->disabled? json_true(): json_false()
    );
    json_object_set_new(
        jn_dict,
        "state",
        json_string(gobj_current_state(gobj))
    );
    json_object_set_new(
        jn_dict,
        "gobj_trace_level",
        json_integer(gobj_trace_level(gobj))
    );
    json_object_set_new(
        jn_dict,
        "gobj_no_trace_level",
        json_integer(gobj_no_trace_level(gobj))
    );
    json_object_set_new(
        jn_dict,
        "attrs",
        json_object()
    );
    json_object_set_new(
        jn_dict,
        "parent_id",
        parent? json_string(kw_get_str(parent, "id", "", KW_REQUIRED)):json_string("")
    );
    json_object_set_new( // Emulate treedb
        jn_dict,
        "childs",
        json_array()
    );

    if(gobj_child_size(gobj)>0) {

        GObj_t * child; rc_instance_t *i_child;
        i_child = gobj_first_child(gobj, (hgobj *)&child);

        while(i_child) {
            _add_gobj_treedb_data(jn_list, jn_dict, child);
            i_child = gobj_next_child(i_child, (hgobj *)&child);
        }
    }
    return 0;
}
PUBLIC json_t *gobj_gobjs_treedb_data(hgobj gobj)
{
    json_t *jn_list = json_array();

    if(!gobj) {
        gobj = __yuno__;
    }

    _add_gobj_treedb_data(jn_list, 0, gobj);

    return jn_list;
}

/***************************************************************************
 *  Set a message error in the gobj.
 ***************************************************************************/
PUBLIC int gobj_set_message_error(hgobj gobj_, const char *msg)
{
    GObj_t *gobj = gobj_;

    GBMEM_FREE(gobj->error_message);
    if(!empty_string(msg)) {
        gobj->error_message = gbmem_strdup(msg);
    }
    return 0;
}

/***************************************************************************
 *  Set a format message error in the gobj.
 ***************************************************************************/
PUBLIC int gobj_set_message_errorf(hgobj gobj_, const char *msg, ...)
{
    GObj_t *gobj = gobj_;

    GBMEM_FREE(gobj->error_message);
    if(empty_string(msg)) {
        return 0;
    }
    va_list ap;
    char temp[512];
    va_start(ap, msg);
    vsnprintf(temp, sizeof(temp), msg, ap);

    gobj->error_message = gbmem_strdup(temp);

    va_end(ap);

    return 0;
}

/***************************************************************************
 *  Read the error message of gobj
 ***************************************************************************/
PUBLIC const char * gobj_get_message_error(hgobj gobj_)
{
    GObj_t *gobj = gobj_;
    if(gobj->error_message)
        return gobj->error_message;
    else
        return "";
}




                    /*---------------------------------*
                     *  SECTION: Trace functions
                     *---------------------------------*/




/***************************************************************************
 *  Debug print global trace levels in json
 ***************************************************************************/
PUBLIC json_t * gobj_repr_global_trace_levels(void)
{
    json_t *jn_register = json_array();

    json_t *jn_internals = json_object();

    json_t *jn_trace_levels = json_object();
    json_object_set_new(
        jn_internals,
        "trace_levels",
        jn_trace_levels
    );

    for(int i=0; i<16; i++) {
        if(!s_global_trace_level[i].name)
            break;
        json_object_set_new(
            jn_trace_levels,
            s_global_trace_level[i].name,
            json_string(s_global_trace_level[i].description)
        );
    }
    json_array_append_new(jn_register, jn_internals);

    return jn_register;
}

/***************************************************************************
 *  Debug print gclass trace levels in json
 ***************************************************************************/
PUBLIC json_t * gobj_repr_gclass_trace_levels(const char *gclass_name)
{
    json_t *jn_register = json_array();

    if(!empty_string(gclass_name)) {
        GCLASS *gclass = gobj_find_gclass(gclass_name, FALSE);
        if(gclass) {
             json_t *jn_gclass = json_object();
             json_object_set_new(
                 jn_gclass,
                 "gclass",
                 json_string(gclass->gclass_name)
             );
            json_object_set_new(
                jn_gclass,
                "trace_levels",
                gobj_trace_level_list(gclass, TRUE)
            );

            json_array_append_new(jn_register, jn_gclass);
        }
        return jn_register;
    }

    gclass_register_t *gclass_reg = dl_first(&dl_gclass);
    while(gclass_reg) {
        if(gclass_reg->gclass) {
             json_t *jn_gclass = json_object();
             json_object_set_new(
                 jn_gclass,
                 "gclass",
                 json_string(gclass_reg->gclass->gclass_name)
             );
            json_object_set_new(
                jn_gclass,
                "trace_levels",
                gobj_trace_level_list(gclass_reg->gclass, TRUE)
            );

            json_array_append_new(jn_register, jn_gclass);
        }

        gclass_reg = dl_next(gclass_reg);
    }

    return jn_register;
}

/****************************************************************************
 *  Return list of trace levels
 *  Remember decref return
 ****************************************************************************/
PUBLIC json_t *gobj_trace_level_list(GCLASS *gclass, BOOL not_internals)
{
    json_t *jn_dict = json_object();
    if(!not_internals) {
        for(int i=0; i<16; i++) {
            if(!s_global_trace_level[i].name)
                break;
            json_object_set_new(
                jn_dict,
                s_global_trace_level[i].name,
                json_string(s_global_trace_level[i].description)
            );
        }
    }
    if(gclass->s_user_trace_level) {
        for(int i=0; i<16; i++) {
            if(!gclass->s_user_trace_level[i].name)
                break;
            json_object_set_new(
                jn_dict,
                gclass->s_user_trace_level[i].name,
                json_string(gclass->s_user_trace_level[i].description)
            );
        }
    }
    return jn_dict;
}

/****************************************************************************
 *  Return list of trace levels
 *  Remember decref return
 ****************************************************************************/
PUBLIC json_t *gobj_trace_level_list2(
    GCLASS *gclass,
    BOOL with_global_levels,
    BOOL with_gclass_levels
)
{
    json_t *jn_dict = json_object();
    if(with_global_levels) {
        for(int i=0; i<16; i++) {
            if(!s_global_trace_level[i].name)
                break;
            json_object_set_new(
                jn_dict,
                s_global_trace_level[i].name,
                json_string(s_global_trace_level[i].description)
            );
        }
    }
    if(with_gclass_levels) {
        if(gclass->s_user_trace_level) {
            for(int i=0; i<16; i++) {
                if(!gclass->s_user_trace_level[i].name)
                    break;
                json_object_set_new(
                    jn_dict,
                    gclass->s_user_trace_level[i].name,
                    json_string(gclass->s_user_trace_level[i].description)
                );
            }
        }
    }
    return jn_dict;
}

/****************************************************************************
 *  Convert 32 bit to string level
 ****************************************************************************/
PRIVATE json_t *bit2level(
    const trace_level_t *internal_trace_level,
    const trace_level_t *user_trace_level,
    uint32_t bit)
{
    json_t *jn_list = json_array();
    for(int i=0; i<16; i++) {
        if(!internal_trace_level[i].name) {
            break;
        }
        uint32_t bitmask = 1 << i;
        bitmask <<= 16;
        if(bit & bitmask) {
            json_array_append_new(
                jn_list,
                json_string(internal_trace_level[i].name)
            );
        }
    }
    if(user_trace_level) {
        for(int i=0; i<16; i++) {
            if(!user_trace_level[i].name) {
                break;
            }
            uint32_t bitmask = 1 << i;
            if(bit & bitmask) {
                json_array_append_new(
                    jn_list,
                    json_string(user_trace_level[i].name)
                );
            }
        }
    }
    return jn_list;
}

/****************************************************************************
 *  Convert string level to 32 bit
 ****************************************************************************/
PRIVATE uint32_t level2bit(
    const trace_level_t *internal_trace_level,
    const trace_level_t *user_trace_level,
    const char *level
)
{
    for(int i=0; i<16; i++) {
        if(!internal_trace_level[i].name) {
            break;
        }
        if(strcasecmp(level, internal_trace_level[i].name)==0) {
            uint32_t bitmask = 1 << i;
            bitmask <<= 16;
            return bitmask;
        }
    }
    if(user_trace_level) {
        for(int i=0; i<16; i++) {
            if(!user_trace_level[i].name) {
                break;
            }
            if(strcasecmp(level, user_trace_level[i].name)==0) {
                uint32_t bitmask = 1 << i;
                return bitmask;
            }
        }
    }

    // WARNING Don't log errors, this functions are called in main(), before logger is setup.
    return 0;
}

/****************************************************************************
 *  Set or Reset gobj trace level
 *  Call mt_trace_on/mt_trace_off
 ****************************************************************************/
PUBLIC int gobj_set_gobj_trace(hgobj gobj_, const char *level, BOOL set, json_t *kw)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        int ret = gobj_set_global_trace(level, set);
        KW_DECREF(kw);
        return ret;
    }

    if(_set_gobj_trace_level(gobj, level, set)<0) {
        KW_DECREF(kw);
        return -1;
    }
    if(set) {
        if(gobj->gclass->gmt.mt_trace_on) {
            return gobj->gclass->gmt.mt_trace_on(gobj, level, kw);
        }
    } else {
        if(gobj->gclass->gmt.mt_trace_off) {
            return gobj->gclass->gmt.mt_trace_off(gobj, level, kw);
        }
    }
    KW_DECREF(kw);
    return 0;
}

/****************************************************************************
 *  Set or Reset gclass trace level
 ****************************************************************************/
PUBLIC int gobj_set_gclass_trace(GCLASS *gclass, const char *level, BOOL set)
{
    uint32_t bitmask = 0;

    if(empty_string(level)) {
        bitmask = TRACE_USER_LEVEL;
    } else {
        if(isdigit(*((unsigned char *)level))) {
            bitmask = atoi(level);
        }
        if(!bitmask) {
            bitmask = level2bit(s_global_trace_level, gclass->s_user_trace_level, level);
            if(!bitmask) {
                if(__initialized__) {
                    log_error(0,
                        "gobj",         "%s", __FILE__,
                        "function",     "%s", __FUNCTION__,
                        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                        "msg",          "%s", "gclass trace level NOT FOUND",
                        "gclass",       "%s", gclass->gclass_name,
                        "level",        "%s", level,
                        NULL
                    );
                }
                return -1;
            }
        }
    }

    if(set) {
        /*
         *  Set
         */
        gclass->__gclass_trace_level__ |= bitmask;
    } else {
        /*
         *  Reset
         */
        gclass->__gclass_trace_level__ &= ~bitmask;
    }

    return 0;
}

/****************************************************************************
 *  Set panic trace
 ****************************************************************************/
PUBLIC int gobj_set_panic_trace(BOOL panic_trace)
{
    __panic_trace__ = panic_trace?TRUE:FALSE;

    return 0;
}

/***************************************************************************
 *  level 1 all but considering __gobj_no_trace_level__
 *  level >1 all
 ***************************************************************************/
PUBLIC int gobj_set_deep_tracing(int level)
{
    __deep_trace__ = level;

    return 0;
}
PUBLIC int gobj_get_deep_tracing(void)
{
    return __deep_trace__;
}

/****************************************************************************
 *  Set or Reset gobj trace level
 ****************************************************************************/
PRIVATE int _set_gobj_trace_level(GObj_t * gobj, const char *level, BOOL set)
{
    uint32_t bitmask = 0;

    if(empty_string(level)) {
        bitmask = TRACE_USER_LEVEL;
    } else {
        if(isdigit(*((unsigned char *)level))) {
            bitmask = atoi(level);
        }
        if(!bitmask) {
            bitmask = level2bit(s_global_trace_level, gobj->gclass->s_user_trace_level, level);
            if(!bitmask) {
                if(__initialized__) {
                    log_error(0,
                        "gobj",         "%s", __FILE__,
                        "function",     "%s", __FUNCTION__,
                        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                        "msg",          "%s", "gclass trace level NOT FOUND",
                        "gobj_name",    "%s", gobj_short_name(gobj),
                        "level",        "%s", level,
                        NULL
                    );
                }
                return -1;
            }
        }
    }

    if(set) {
        /*
         *  Set
         */
        gobj->__gobj_trace_level__ |= bitmask;
    } else {
        /*
         *  Reset
         */
        gobj->__gobj_trace_level__ &= ~bitmask;
    }

    return 0;
}

/****************************************************************************
 *  Set or Reset global trace level
 ****************************************************************************/
PUBLIC int gobj_set_global_trace(const char *level, BOOL set)
{
    uint32_t bitmask = 0;

    if(empty_string(level)) {
        bitmask = TRACE_GLOBAL_LEVEL;
    } else {
        if(isdigit(*((unsigned char *)level))) {
            bitmask = atoi(level);
        }
        if(!bitmask) {
            bitmask = level2bit(s_global_trace_level, 0, level);
            if(!bitmask) {
                if(__initialized__) {
                    log_error(0,
                        "gobj",         "%s", __FILE__,
                        "function",     "%s", __FUNCTION__,
                        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                        "msg",          "%s", "global trace level NOT FOUND",
                        "level",        "%s", level,
                        NULL
                    );
                }
                return -1;
            }
        }
    }

    if(set) {
        /*
         *  Set
         */
        __global_trace_level__ |= bitmask;
    } else {
        /*
         *  Reset
         */
        __global_trace_level__ &= ~bitmask;
    }
    return 0;
}

/****************************************************************************
 *  Set or Reset NO gclass trace level
 ****************************************************************************/
PUBLIC int gobj_set_gclass_no_trace(GCLASS *gclass, const char *level, BOOL set)
{
    uint32_t bitmask = 0;

    if(empty_string(level)) {
        bitmask = -1;
    } else {
        if(isdigit(*((unsigned char *)level))) {
            bitmask = atoi(level);
        }
        if(!bitmask) {
            bitmask = level2bit(s_global_trace_level, gclass->s_user_trace_level, level);
            if(!bitmask) {
                if(__initialized__) {
                    log_error(0,
                        "gobj",         "%s", __FILE__,
                        "function",     "%s", __FUNCTION__,
                        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                        "msg",          "%s", "gclass trace level NOT FOUND",
                        "gclass",       "%s", gclass->gclass_name,
                        "level",        "%s", level,
                        NULL
                    );
                }
                return -1;
            }
        }
    }

    if(set) {
        /*
         *  Set
         */
        gclass->__gclass_no_trace_level__ |= bitmask;
    } else {
        /*
         *  Reset
         */
        gclass->__gclass_no_trace_level__ &= ~bitmask;
    }

    return 0;
}

/****************************************************************************
 *  Set or Reset NO gobj trace level
 ****************************************************************************/
PRIVATE int _set_gobj_no_trace_level(hgobj gobj_, const char *level, BOOL set)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        return 0;
    }
    uint32_t bitmask = 0;

    if(empty_string(level)) {
        bitmask = -1;
    } else {
        if(isdigit(*((unsigned char *)level))) {
            bitmask = atoi(level);
        }
        if(!bitmask) {
            bitmask = level2bit(s_global_trace_level, gobj->gclass->s_user_trace_level, level);
            if(!bitmask) {
                if(__initialized__) {
                    log_error(0,
                        "gobj",         "%s", __FILE__,
                        "function",     "%s", __FUNCTION__,
                        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                        "msg",          "%s", "gclass trace level NOT FOUND",
                        "gobj_name",    "%s", gobj_short_name(gobj),
                        "level",        "%s", level,
                        NULL
                    );
                }
                return -1;
            }
        }
    }

    if(set) {
        /*
         *  Set
         */
        gobj->__gobj_no_trace_level__ |= bitmask;
    } else {
        /*
         *  Reset
         */
        gobj->__gobj_no_trace_level__ &= ~bitmask;
    }

    return 0;
}

/****************************************************************************
 *  Set or Reset NO gobj trace level
 ****************************************************************************/
PUBLIC int gobj_set_gobj_no_trace(hgobj gobj_, const char *level, BOOL set)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        return 0;
    }
    _set_gobj_no_trace_level(gobj, level, set);

    return 0;
}

/****************************************************************************
 *  Return gobj trace level
 ****************************************************************************/
PUBLIC uint32_t gobj_trace_level(hgobj gobj_)
{
    GObj_t * gobj = gobj_;

    if(__deep_trace__ || __panic_trace__) {
        return -1;
    }
    uint32_t bitmask = __global_trace_level__;
    if(gobj) {
        bitmask |= gobj->__gobj_trace_level__;
        if(gobj->gclass) {
            bitmask |= gobj->gclass->__gclass_trace_level__;
        }
    }

    return bitmask;
}

/****************************************************************************
 *  Return gobj no trace level
 ****************************************************************************/
PUBLIC uint32_t gobj_no_trace_level(hgobj gobj_)
{
    GObj_t * gobj = gobj_;
    if(!gobj || !gobj->gclass) {
        return 0;
    }
    uint32_t bitmask = gobj->__gobj_no_trace_level__ | gobj->gclass->__gclass_no_trace_level__;

    return bitmask;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_get_gclass_trace_level_list(GCLASS *gclass)
{
    json_t *jn_list = json_array();

    if(gclass) {
        json_t *jn_levels = gobj_get_gclass_trace_level(gclass);
        if(json_array_size(jn_levels)) {
            json_t *jn_gclass = json_object();
            json_object_set_new(
                jn_gclass,
                "gclass",
                json_string(gclass->gclass_name)
            );
            json_object_set_new(
                jn_gclass,
                "trace_levels",
                jn_levels
            );
            json_array_append_new(jn_list, jn_gclass);
        } else {
            JSON_DECREF(jn_levels);
        }
        return jn_list;
    }

    gclass_register_t *gclass_reg = dl_first(&dl_gclass);
    while(gclass_reg) {
        if(gclass_reg->gclass) {
            json_t *jn_levels = gobj_get_gclass_trace_level(gclass_reg->gclass);
            if(json_array_size(jn_levels)) {
                json_t *jn_gclass = json_object();
                json_object_set_new(
                    jn_gclass,
                    "gclass",
                    json_string(gclass_reg->gclass->gclass_name)
                );
                json_object_set_new(
                    jn_gclass,
                    "trace_levels",
                    jn_levels
                );
                json_array_append_new(jn_list, jn_gclass);
            } else {
                JSON_DECREF(jn_levels);
            }
        }

        gclass_reg = dl_next(gclass_reg);
    }

    return jn_list;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_get_gclass_no_trace_level_list(GCLASS *gclass)
{
    json_t *jn_list = json_array();

    if(gclass) {
        json_t *jn_levels = gobj_get_gclass_no_trace_level(gclass);
        if(json_array_size(jn_levels)) {
            json_t *jn_gclass = json_object();
            json_object_set_new(
                jn_gclass,
                "gclass",
                json_string(gclass->gclass_name)
            );
            json_object_set_new(
                jn_gclass,
                "trace_levels",
                jn_levels
            );
            json_array_append_new(jn_list, jn_gclass);
        } else {
            JSON_DECREF(jn_levels);
        }
        return jn_list;
    }

    gclass_register_t *gclass_reg = dl_first(&dl_gclass);
    while(gclass_reg) {
        if(gclass_reg->gclass) {
            json_t *jn_levels = gobj_get_gclass_no_trace_level(gclass_reg->gclass);
            if(json_array_size(jn_levels)) {
                json_t *jn_gclass = json_object();
                json_object_set_new(
                    jn_gclass,
                    "gclass",
                    json_string(gclass_reg->gclass->gclass_name)
                );
                json_object_set_new(
                    jn_gclass,
                    "no_trace_levels",
                    jn_levels
                );
                json_array_append_new(jn_list, jn_gclass);
            } else {
                JSON_DECREF(jn_levels);
            }
        }

        gclass_reg = dl_next(gclass_reg);
    }

    return jn_list;
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE int cb_set_xxx_childs(rc_instance_t *i_child, hgobj child, void *user_data, void *user_data2, void *user_data3)
{
    json_t *jn_list = user_data;
    json_t *jn_level = gobj_get_gobj_trace_level(child);
    if(json_array_size(jn_level)) {
        json_t *jn_o = json_object();

        json_object_set_new(
            jn_o,
            "gobj",
            json_string(gobj_short_name(child))
        );
        json_object_set_new(
            jn_o,
            "trace_levels",
            jn_level
        );
        json_array_append_new(jn_list, jn_o);
    } else {
        JSON_DECREF(jn_level);
    }
    return 0;
}
PUBLIC json_t *gobj_get_gobj_trace_level_tree(hgobj gobj)
{
    json_t *jn_list = json_array();
    cb_set_xxx_childs(0, gobj, jn_list, 0, 0);
    gobj_walk_gobj_childs_tree(gobj, WALK_TOP2BOTTOM, cb_set_xxx_childs, jn_list, 0, 0);
    return jn_list;
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE int cb_set_no_xxx_childs(rc_instance_t *i_child, hgobj child, void *user_data, void *user_data2, void *user_data3)
{
    json_t *jn_list = user_data;
    json_t *jn_level = gobj_get_gobj_no_trace_level(child);
    if(json_array_size(jn_level)) {
        json_t *jn_o = json_object();

        json_object_set_new(
            jn_o,
            "gobj",
            json_string(gobj_short_name(child))
        );
        json_object_set_new(
            jn_o,
            "no_trace_levels",
            jn_level
        );
        json_array_append_new(jn_list, jn_o);
    } else {
        JSON_DECREF(jn_level);
    }
    return 0;
}
PUBLIC json_t *gobj_get_gobj_no_trace_level_tree(hgobj gobj)
{
    json_t *jn_list = json_array();
    cb_set_no_xxx_childs(0, gobj, jn_list, 0, 0);
    gobj_walk_gobj_childs_tree(gobj, WALK_TOP2BOTTOM, cb_set_no_xxx_childs, jn_list, 0, 0);
    return jn_list;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_get_global_trace_level(void)
{
    json_t *jn_list;
    jn_list = bit2level(
        s_global_trace_level,
        0,
        __global_trace_level__
    );
    return jn_list;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_get_gclass_trace_level(GCLASS *gclass)
{
    json_t *jn_list = bit2level(
        s_global_trace_level,
        gclass->s_user_trace_level,
        gclass->__gclass_trace_level__ | __global_trace_level__
    );
    return jn_list;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_get_gclass_no_trace_level(GCLASS *gclass)
{
    json_t *jn_list = bit2level(
        s_global_trace_level,
        gclass->s_user_trace_level,
        gclass->__gclass_no_trace_level__
    );
    return jn_list;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_get_gobj_trace_level(hgobj gobj_)
{
    GObj_t * gobj = gobj_;
    json_t *jn_list;
    if(gobj) {
        jn_list = bit2level(
            s_global_trace_level,
            gobj->gclass->s_user_trace_level,
            gobj_trace_level(gobj)
        );
    } else {
        jn_list = bit2level(
            s_global_trace_level,
            0,
            __global_trace_level__
        );
    }
    return jn_list;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *gobj_get_gobj_no_trace_level(hgobj gobj_)
{
    GObj_t * gobj = gobj_;
    json_t *jn_list = bit2level(
        s_global_trace_level,
        gobj->gclass->s_user_trace_level,
        gobj_no_trace_level(gobj)
    );
    return jn_list;
}

/***************************************************************************
 *  Must trace?
 ***************************************************************************/
PRIVATE inline BOOL is_machine_tracing(GObj_t * gobj)
{
    if(__deep_trace__ || __panic_trace__) {
        return TRUE;
    }
    if(!gobj) {
        return FALSE;
    }
    int trace = __global_trace_level__ & TRACE_MACHINE ||
        gobj->__gobj_trace_level__ & TRACE_MACHINE ||
        gobj->gclass->__gclass_trace_level__ & TRACE_MACHINE;

    return trace?TRUE:FALSE;
}

/***************************************************************************
 *  Must no trace?
 ***************************************************************************/
PRIVATE inline BOOL is_machine_not_tracing(GObj_t * gobj)
{
    if(abs(__deep_trace__) > 1 || __panic_trace__) {
        return FALSE;
    }
    if(!gobj) {
        return TRUE;
    }
    int no_trace = gobj->__gobj_no_trace_level__ & TRACE_MACHINE ||
        gobj->gclass->__gclass_no_trace_level__ & TRACE_MACHINE;

    return no_trace?TRUE:FALSE;
}

/****************************************************************************
 *  Indent, return spaces multiple of depth level gobj.
 *  With this, we can see the trace messages indenting according
 *  to depth level.
 ****************************************************************************/
PRIVATE char *tab(char *bf, int bflen)
{
    int i;

    bf[0]=' ';
    for(i=1; i<__inside__*2 && i<bflen-2; i++)
        bf[i] = ' ';
    bf[i] = '\0';
    return bf;
}

/****************************************************************************
 *  Trace machine function
 ****************************************************************************/
PUBLIC void trace_machine(const char *fmt, ...)
{
    va_list ap;
    char bf[4*1024];
    tab(bf, sizeof(bf));

    va_start(ap, fmt);
    int len = strlen(bf);
    vsnprintf(bf+len, sizeof(bf)-len, fmt, ap);
    va_end(ap);

    trace_msg("%s", bf);
}




                    /*------------------------------------*
                     *  SECTION: Authorization functions
                     *------------------------------------*/




/***************************************************************************
 *  Authenticate
 *  Return json response
 *
        {
            "result": 0,        // 0 successful authentication, -1 error
            "comment": "",
            "username": ""      // username authenticated
        }

 *  HACK if there is no authentication parser the authentication is TRUE
    and the username is the current system user
 *  WARNING Becare and use no parser only in local services!
 ***************************************************************************/
PUBLIC json_t *gobj_authenticate(hgobj gobj_, json_t *kw, hgobj src)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        return 0;
    }

    /*---------------------------------------------*
     *  The local mt_authenticate has preference
     *---------------------------------------------*/
    if(gobj->gclass->gmt.mt_authenticate) {
        return gobj->gclass->gmt.mt_authenticate(gobj, kw, src);
    }

    /*-----------------------------------------------*
     *  Then use the global authzs parser
     *-----------------------------------------------*/
    if(!__global_authenticate_parser_fn__) {
#ifdef __linux__
        struct passwd *pw = getpwuid(getuid());

        KW_DECREF(kw)
        return json_pack("{s:i, s:s, s:s}",
            "result", 0,
            "comment", "Working without authentication",
            "username", pw->pw_name
        );
#else
        KW_DECREF(kw)
        return json_pack("{s:i, s:s, s:s}",
            "result", 0,
            "comment", "Working without authentication",
            "username", "yuneta"
        );
#endif
    }

    return __global_authenticate_parser_fn__(gobj, kw, src);
}

/****************************************************************************
 *  list authzs of gobj
 ****************************************************************************/
PUBLIC json_t *gobj_authzs(
    hgobj gobj  // If null return global authzs
)
{
    return authzs_list(gobj, "");
}

/****************************************************************************
 *  list authzs of gobj
 ****************************************************************************/
PUBLIC json_t *gobj_authz(
    hgobj gobj_,
    const char *authz // return all list if empty string, else return authz desc
)
{
    GObj_t *gobj = gobj_;
    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        return 0;
    }
    return authzs_list(gobj, authz);
}

/****************************************************************************
 *  Return if user has authz in gobj in context
 *  HACK if there is no authz checker the authz is TRUE
 ****************************************************************************/
PUBLIC BOOL gobj_user_has_authz(
    hgobj gobj_,
    const char *authz,
    json_t *kw,
    hgobj src
)
{
    GObj_t * gobj = gobj_;

    if(!gobj || gobj->obflag & obflag_destroyed) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hgobj NULL or DESTROYED",
            NULL
        );
        KW_DECREF(kw);
        return FALSE;
    }

    /*----------------------------------------------------*
     *  The local mt_authz_checker has preference
     *----------------------------------------------------*/
    if(gobj->gclass->gmt.mt_authz_checker) {
        BOOL has_permission = gobj->gclass->gmt.mt_authz_checker(gobj, authz, kw, src);
        if(__trace_gobj_authzs__(gobj)) {
            log_debug_json(0, kw,
                "local authzs 🔑🔑 %s => %s",
                gobj_short_name(gobj),
                has_permission?"👍":"🚫"
            );
        }
        return has_permission;
    }

    /*-----------------------------------------------*
     *  Then use the global authz checker
     *-----------------------------------------------*/
    if(__global_authz_checker_fn__) {
        BOOL has_permission = __global_authz_checker_fn__(gobj, authz, kw, src);
        if(__trace_gobj_authzs__(gobj)) {
            log_debug_json(0, kw,
                "global authzs 🔑🔑 %s => %s",
                gobj_short_name(gobj),
                has_permission?"👍":"🚫"
            );
        }
        return has_permission;
    }

    KW_DECREF(kw);
    return TRUE; // HACK if there is no authz checker the authz is TRUE
}

/****************************************************************************
 *
 ****************************************************************************/
PUBLIC const sdata_desc_t *gobj_get_global_authz_table(void)
{
    return global_authz_table;
}




                    /*---------------------------------*
                     *  SECTION: MONITOR functions
                     *---------------------------------*/




/*
 *  To monitor gobj and event activities.
 *  Monitor transmit the data in udp protocol, without assured delivery.
 *  To send monitoring data to another host,
 *  you can point urlMonitor to a localhost relay yuno,
 *  and then use a reliable udp protocol to another host.
 */

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE void monitor_gobj(
    monitor_gobj_t monitor_gobj,
    GObj_t *gobj)
{
    switch(monitor_gobj) {
    case MTOR_GOBJ_YUNO_CREATED:
        log_monitor(0,
            "op",           "%s", "GOBJ_YUNO_CREATED",
            "yuno_id",      "%p", gobj_yuno(),
            "role",         "%s", __yuno_role__,
            "gclass",       "%s", gobj_gclass_name(gobj),
            "id",           "%s", __yuno_id__,
            "name",         "%s", __yuno_name__,
            "level",        "%d", _gobj_level(gobj),
            "parent_id",    "%p", gobj->__parent__,
            "gobj_id",      "%p", gobj,
            NULL
        );
        break;

    case MTOR_GOBJ_DEFAULT_SERVICE_CREATED:
        log_monitor(0,
            "op",           "%s", "GOBJ_DEFAULT_SERVICE_CREATED",
            "yuno_id",      "%p", gobj_yuno(),
            "role",         "%s", __yuno_role__,
            "gclass",       "%s", gobj_gclass_name(gobj),
            "id",           "%s", __yuno_id__,
            "name",         "%s", __yuno_name__,
            "level",        "%d", _gobj_level(gobj),
            "parent_id",    "%p", gobj->__parent__,
            "gobj_id",      "%p", gobj,
            NULL
        );
        break;
    case MTOR_GOBJ_SERVICE_CREATED:
        log_monitor(0,
            "op",           "%s", "GOBJ_SERVICE_CREATED",
            "yuno_id",      "%p", gobj_yuno(),
            "role",         "%s", __yuno_role__,
            "gclass",       "%s", gobj_gclass_name(gobj),
            "id",           "%s", __yuno_id__,
            "name",         "%s", __yuno_name__,
            "level",        "%d", _gobj_level(gobj),
            "parent_id",    "%p", gobj->__parent__,
            "gobj_id",      "%p", gobj,
            NULL
        );
        break;
    case MTOR_GOBJ_UNIQUE_CREATED:
        log_monitor(0,
            "op",           "%s", "GOBJ_UNIQUE_CREATED",
            "yuno_id",      "%p", gobj_yuno(),
            "gclass",       "%s", gobj_gclass_name(gobj),
            "id",           "%s", __yuno_id__,
            "name",         "%s", gobj_name(gobj),
            "level",        "%d", _gobj_level(gobj),
            "parent_id",    "%p", gobj->__parent__,
            "gobj_id",      "%p", gobj,
            NULL
        );
        break;
    case MTOR_GOBJ_CREATED:
        log_monitor(0,
            "op",           "%s", "GOBJ_CREATED",
            "yuno_id",      "%p", gobj_yuno(),
            "gclass",       "%s", gobj_gclass_name(gobj),
            "name",         "%s", gobj_name(gobj),
            "level",        "%d", _gobj_level(gobj),
            "parent_id",    "%p", gobj->__parent__,
            "gobj_id",      "%p", gobj,
            NULL
        );
        break;
    case MTOR_GOBJ_DESTROYED:
        log_monitor(0,
            "op",           "%s", "GOBJ_DESTROYED",
            "yuno_id",      "%p", gobj_yuno(),
            "gobj_id",      "%p", gobj,
            NULL
        );
        break;
    case MTOR_GOBJ_START:
        log_monitor(0,
            "op",           "%s", "GOBJ_START",
            "yuno_id",      "%p", gobj_yuno(),
            "gobj_id",      "%p", gobj,
            NULL
        );
        break;
    case MTOR_GOBJ_STOP:
        log_monitor(0,
            "op",           "%s", "GOBJ_STOP",
            "yuno_id",      "%p", gobj_yuno(),
            "gobj_id",      "%p", gobj,
            NULL
        );
        break;
    case MTOR_GOBJ_PLAY:
        log_monitor(0,
            "op",           "%s", "GOBJ_PLAY",
            "yuno_id",      "%p", gobj_yuno(),
            "gobj_id",      "%p", gobj,
            NULL
        );
        break;
    case MTOR_GOBJ_PAUSE:
        log_monitor(0,
            "op",           "%s", "GOBJ_PAUSE",
            "yuno_id",      "%p", gobj_yuno(),
            "gobj_id",      "%p", gobj,
            NULL
        );
        break;
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE void monitor_event(
    monitor_event_t monitor_event,
    const char *event,
    GObj_t *src,
    GObj_t *dst)
{
    switch(monitor_event) {
    case MTOR_EVENT_ACCEPTED:
        log_monitor(0,
            "op",           "%s", "EVENT_ACCEPTED",
            "src_id",       "%p", src,
            "dst_id",       "%p", dst,
            NULL
        );
        break;
    case MTOR_EVENT_REFUSED:
        log_monitor(0,
            "op",           "%s", "EVENT_REFUSED",
            "src_id",       "%p", src,
            "dst_id",       "%p", dst,
            NULL
        );
        break;
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE void monitor_state(GObj_t *gobj)
{
    log_monitor(0,
        "op",           "%s", "NEW_STATE",
        "gobj_id",      "%p", gobj,
        NULL
    );
}




                    /*---------------------------------*
                     *  SECTION: Print/debug functions
                     *---------------------------------*/




/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_print_subscriptions(hgobj gobj_)
{
    GObj_t * gobj = gobj_;
    rc_instance_t *i_subs; hsdata subs;

    trace_msg("========> Subscriptions %s\n", gobj_short_name(gobj));
    i_subs = rc_first_instance(&gobj->dl_subscriptions, (rc_resource_t **)&subs);
    while(i_subs) {
        hgobj publisher_ = sdata_read_pointer(subs, "publisher");
        hgobj subscriber_ = sdata_read_pointer(subs, "subscriber");

        json_t *jn = sdata2json(subs, -1, 0);
        log_debug_json(0, jn, "GOBJ %s, PUBLISHER: %s, SUBSCRIBER: %s",
            gobj_short_name(gobj),
            gobj_short_name(publisher_),
            gobj_short_name(subscriber_));
        json_decref(jn);
        i_subs = rc_next_instance(i_subs, (rc_resource_t **)&subs);
    }

    trace_msg("========> Subscribings %s\n", gobj_short_name(gobj));
    i_subs = rc_first_instance(&gobj->dl_subscribings, (rc_resource_t **)&subs);
    while(i_subs) {
        hgobj publisher_ = sdata_read_pointer(subs, "publisher");
        hgobj subscriber_ = sdata_read_pointer(subs, "subscriber");

        json_t *jn = sdata2json(subs, -1, 0);
        log_debug_json(0, jn, "GOBJ %s, PUBLISHER: %s, SUBSCRIBER: %s",
            gobj_short_name(gobj),
            gobj_short_name(publisher_),
            gobj_short_name(subscriber_));
        json_decref(jn);
        i_subs = rc_next_instance(i_subs, (rc_resource_t **)&subs);
    }
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_print_childs(dl_list_t *dl_childs, int verbose_level)
{
    hgobj child; rc_instance_t *i_hs;
    i_hs = rc_first_instance(dl_childs, (rc_resource_t **)&child);
    while(i_hs) {
        log_debug_printf(0, "GObj %s", gobj_short_name(child));

        i_hs = rc_next_instance(i_hs, (rc_resource_t **)&child);
    }

    return 0;
}




                    /*---------------------------------*
                     *  SECTION: Stats
                     *---------------------------------*/




/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_int_t gobj_set_stat(hgobj gobj_, const char *path, json_int_t value)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        return 0;
    }
    json_int_t old_value = kw_get_int(gobj->jn_stats, path, 0, 0);
    kw_set_dict_value(gobj->jn_stats, path, json_integer(value));

    return old_value;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_int_t gobj_incr_stat(hgobj gobj_, const char *path, json_int_t value)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        return 0;
    }

    json_int_t cur_value = kw_get_int(gobj->jn_stats, path, 0, 0);
    cur_value += value;
    kw_set_dict_value(gobj->jn_stats, path, json_integer(cur_value));

    return cur_value;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_int_t gobj_decr_stat(hgobj gobj_, const char *path, json_int_t value)
{
    GObj_t * gobj = gobj_;
    if(!gobj) {
        return 0;
    }
    json_int_t cur_value = kw_get_int(gobj->jn_stats, path, 0, 0);
    cur_value -= value;
    kw_set_dict_value(gobj->jn_stats, path, json_integer(cur_value));

    return cur_value;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_int_t gobj_get_stat(hgobj gobj, const char *path)
{
    if(!gobj) {
        return 0;
    }
    return kw_get_int(((GObj_t *)gobj)->jn_stats, path, 0, 0);
}

/***************************************************************************
 *  WARNING the json return is NOT YOURS!
 ***************************************************************************/
PUBLIC json_t *gobj_jn_stats(hgobj gobj)
{
    return ((GObj_t *)gobj)->jn_stats;
}




                    /*---------------------------------*
                     *  SECTION: 2key
                     *---------------------------------*/




/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_2key_register(const char *key1, const char *key2, json_t *value)
{
    if(empty_string(key1)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "key1 empty",
            NULL
        );
        return -1;
    }
    if(empty_string(key2)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "key2 empty",
            "key",          "%s", key1,
            NULL
        );
        return -1;
    }

    json_t *jn_key1 = kw_get_dict(__2key__, key1, json_object(), KW_CREATE);
    if(!jn_key1) {
        // Error already logged
        return -1;
    }
    json_t *jn_key2 = kw_get_dict(jn_key1, key2, kw_incref(value), KW_CREATE);
    if(jn_key2) {
        return 0;
    } else {
        return -1;
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int gobj_2key_deregister(const char *key1, const char *key2)
{
    if(empty_string(key1)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "key1 empty",
            NULL
        );
        return -1;
    }
    if(empty_string(key2)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "key2 empty",
            "key",          "%s", key1,
            NULL
        );
        return -1;
    }

    json_t *jn_key1 = kw_get_dict(__2key__, key1, 0, 0);
    if(!jn_key1) {
        return -1;
    }
    return kw_delete(jn_key1, key2);
}

/***************************************************************************
 *  retorna {k1:{k2:type,},}, sin `value`s!, y en type: "",[],{}
 ***************************************************************************/
PUBLIC json_t *gobj_2key_get_schema(void)
{
    json_t *jn_schema = json_object();

    const char *key1; json_t *jn_key1;
    json_object_foreach(__2key__, key1, jn_key1) {
        json_t *jn_l2 = json_object();
        json_object_set_new(jn_schema, key1, jn_l2);

        const char *key2; json_t *jn_value;
        json_object_foreach(jn_key1, key2, jn_value) {
            if(json_is_string(jn_value)) {
                json_object_set_new(jn_l2, key2, json_string(""));
            } else if(json_is_integer(jn_value)) {
                json_object_set_new(jn_l2, key2, json_integer(0));
            } else if(json_is_object(jn_value)) {
                json_object_set_new(jn_l2, key2, json_object());
            } else if(json_is_array(jn_value)) {
                json_object_set_new(jn_l2, key2, json_array());
            } else if(json_is_boolean(jn_value)) {
                json_object_set_new(jn_l2, key2, json_true());
            } else if(json_is_real(jn_value)) {
                json_object_set_new(jn_l2, key2, json_real(0.0));
            } else if(json_is_null(jn_value)) {
                json_object_set_new(jn_l2, key2, json_null());
            } else {
                json_object_set_new(jn_l2, key2, json_string("MERDE"));
            }
        }
    }
    return jn_schema;
}

/***************************************************************************
 *  WARNING the json return is NOT YOURS!
 ***************************************************************************/
PUBLIC json_t *gobj_2key_get_value(const char *key1, const char *key2)
{
    return kw_get_subdict_value(__2key__, key1, key2, 0, KW_REQUIRED);
}
