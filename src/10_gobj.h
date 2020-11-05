/****************************************************************************
 *  GOBJ.H
 *  Object with:
 *      - an internal simple-machine the defines its behaviour.
 *      - a flexible attributes system
 *  Communication between gobjs happens through events.
 *
 *  GObj's are organized as a tree.
 *
 *  Copyright (c) 1996-2017 Niyamaka.
 *  All Rights Reserved.
 ****************************************************************************/

#ifndef _GOBJ_H
#define _GOBJ_H 1

#include <stdarg.h>
#include <stdio.h>
#include <uv.h>
#include <ghelpers.h>
#include "01_sdata.h"

#ifdef __cplusplus
extern "C"{
#endif

/*********************************************************************
 *      Constants
 *********************************************************************/
#define ATTR_WRITABLE (SDF_WR|SDF_PERSIST)
#define ATTR_READABLE (SDF_RD|SDF_WR|SDF_PERSIST|SDF_STATS|SDF_VOLATIL|SDF_RSTATS|SDF_PSTATS)
/*
 *  Macros to assign attr data to priv data.
 */
#define IF_EQ_SET_PRIV(__name__, __func__) \
    if(strcmp(path, #__name__)==0) {\
        priv->__name__ = __func__(gobj, #__name__);
#define ELIF_EQ_SET_PRIV(__name__, __func__) \
    } else if(strcmp(path, #__name__)==0) {\
        priv->__name__ = __func__(gobj, #__name__);
#define END_EQ_SET_PRIV() \
    }

#define SET_PRIV(__name__, __func__) \
    priv->__name__ = __func__(gobj, #__name__);

/*
 *  Special principal. Also you can speficy strings of id's or group id's.
 */

/*
 * The special principal id named 'Everyone'.
 * This principal id is granted to all requests.
 * Its actual value is the string 'system.Everyone'.
 */
#define Everyone "Everyone"


/*
 * The special principal id named 'Authenticated'.
 * This principal id is granted to all requests which contain
 * any other non-Everyone principal id (according to the authentication policy).
 * Its actual value is the string 'system.Authenticated'.
 */
#define Authenticated "Authenticated"

/*
 * An object that can be used as the permission member of an ACE
 * which matches all permissions unconditionally.
 * For example, an ACE that uses ALL_PERMISSIONS might be composed
 * like so: ('Deny', 'system.Everyone', ALL_PERMISSIONS).
 */
#define ALL_PERMISSIONS "ALL_PERMISSIONS"

/*
 * A convenience shorthand ACE that defines
 * ('Deny', 'system.Everyone', ALL_PERMISSIONS).
 * This is often used as the last ACE in an ACL in systems
 * that use an "inheriting" security policy,
 * representing the concept "don't inherit any other ACEs".
 */
#define DENY_ALL {Deny, "system.Everyone", ALL_PERMISSIONS}

/*
 * A special permission which indicates that the view should always be
 * executable by entirely anonymous users, regardless of the default permission,
 * bypassing any authorization policy that may be in effect.
 * Its actual value is the string '__no_permission_required__'.
 */
#define NO_PERMISSION_REQUIRED "__no_permission_required__"


/*********************************************************************
 *      Structures
 *********************************************************************/
/*
 *  Yuneta types
 */
typedef void * hgobj;

typedef int (*gobj_action_fn)(
    hgobj gobj,
    const char *event,
    json_t *kw,
    hgobj src
);

typedef struct {
    const char *event;
    gobj_action_fn action;
    const char *next_state;
} EV_ACTION;

typedef enum {
/*
 *  Event with as "kw_writting": the kw is returned to sender with the kw modified by receiver.
 *  This event is to be used locally, not valid for inter-events.
 *  The parent must decrease the kw after return of send_event() function.
 *  The action receiving a kw_writting event must work as usual, clearing his kw copy.
 *  The kw in gobj_send_event is increased automatically by ginsfsm. The action must decref it always
 */
    EVF_KW_WRITING      = 0x0001,
    EVF_PUBLIC_EVENT    = 0x0002,   // You should document a public event, it's the API
    EVF_NO_WARN_SUBS    = 0x0004,   // Don't warn of "Publish event WITHOUT subscribers"
} event_flag_t;

typedef struct {
    const char *event;
    event_flag_t flag;
    const char *permission;
    const char *description;
} EVENT;

typedef struct {
    const EVENT *input_events;    //const char **event_names;
    const EVENT *output_events;   //const char **output_event_list;
    const char **state_names;
    EV_ACTION **states;
} FSM;

typedef int   (*mt_start_fn)(hgobj gobj);
typedef int   (*mt_stop_fn)(hgobj gobj);
typedef int   (*mt_play_fn)(hgobj gobj);
typedef int   (*mt_pause_fn)(hgobj gobj);
typedef void  (*mt_create_fn)(hgobj gobj);
typedef void  (*mt_create2_fn)(hgobj gobj, json_t *kw);
typedef void  (*mt_destroy_fn)(hgobj gobj);
typedef void  (*mt_writing_fn)(hgobj gobj, const char *name);
typedef SData_Value_t (*mt_reading_fn)(hgobj gobj, const char *name, int type, SData_Value_t data);
typedef int   (*mt_subscription_added_fn)(hgobj gobj, hsdata subs);
typedef int   (*mt_subscription_deleted_fn)(hgobj gobj, hsdata subs);

/*
 *  These three functions return -1 (broke), 0 continue without publish, 1 continue and publish
 */
typedef BOOL  (*mt_publish_event_fn) ( // Return -1,0,1
    hgobj gobj,
    const char *event,
    json_t *kw  // NOT owned! you can modify this publishing kw
);
typedef BOOL  (*mt_publication_pre_filter_fn) ( // Return -1,0,1
    hgobj gobj,
    hsdata subs,
    const char *event,
    json_t *kw  // NOT owned! you can modify this publishing kw
);
typedef BOOL  (*mt_publication_filter_fn) ( // Return -1,0,1
    hgobj gobj,
    const char *event,
    json_t *kw,  // NOT owned! you can modify this publishing kw
    hgobj subscriber
);
typedef int   (*mt_child_added_fn)(hgobj gobj, hgobj child);
typedef int   (*mt_child_removed_fn)(hgobj gobj, hgobj child);

typedef hsdata (*mt_create_resource_fn)(hgobj gobj, const char *resource, json_t *kw);
typedef dl_list_t * (*mt_list_resource_fn)(hgobj gobj, const char *resource, json_t *kw);
typedef int   (*mt_update_resource_fn)(hgobj gobj, hsdata hs);
typedef int   (*mt_delete_resource_fn)(hgobj gobj, hsdata hs);
typedef int   (*mt_add_child_resource_link_fn)(hgobj gobj, hsdata hs_parent, hsdata hs_child);
typedef int   (*mt_delete_child_resource_link_fn)(hgobj gobj, hsdata hs_parent, hsdata hs_child);
typedef hsdata (*mt_get_resource_fn)(hgobj gobj, const char *resource, json_int_t parent_id, json_int_t id);

typedef json_t *(*mt_create_node_fn)(hgobj gobj, const char *topic_name, json_t *kw, const char *options);
typedef int (*mt_save_node_fn)(hgobj gobj, json_t *node);
typedef json_t *(*mt_update_node_fn)(hgobj gobj, const char *topic_name, json_t *kw, const char *options);
typedef int   (*mt_delete_node_fn)(hgobj gobj, const char *topic_name, json_t *kw, const char *options);
typedef int   (*mt_link_nodes_fn)(hgobj gobj, const char *hook, json_t *parent, json_t *child);
typedef int   (*mt_link_nodes2_fn)(hgobj gobj, const char *parent_ref, const char *child_ref);
typedef int   (*mt_unlink_nodes_fn)(hgobj gobj, const char *hook, json_t *parent, json_t *child);
typedef int   (*mt_unlink_nodes2_fn)(hgobj gobj, const char *parent_ref, const char *child_ref);
typedef json_t *(*mt_get_node_fn)(hgobj gobj, const char *topic_name, const char *id, json_t *options);
typedef json_t *(*mt_list_nodes_fn)(hgobj gobj, const char *topic_name, json_t *jn_filter, json_t *options);
typedef int   (*mt_shoot_snap_fn)(hgobj gobj, const char *tag);
typedef int   (*mt_activate_snap_fn)(hgobj gobj, const char *tag);
typedef json_t *(*mt_list_snaps_fn)(hgobj gobj, json_t *filter);

typedef json_t *(*mt_treedbs_fn)(hgobj gobj);
typedef json_t *(*mt_treedb_topics_fn)(hgobj gobj, const char *treedb_name, const char *options);
typedef json_t *(*mt_topic_desc_fn)(hgobj gobj, const char *topic_name);
typedef json_t *(*mt_topic_links_fn)(hgobj gobj, const char *treedb_name, const char *topic_name);
typedef json_t *(*mt_topic_hooks_fn)(hgobj gobj, const char *treedb_name, const char *topic_name);
typedef json_t *(*mt_node_parents_fn)(
    hgobj gobj, const char *topic_name, const char *id, const char *link, json_t *options
);
typedef json_t *(*mt_node_childs_fn)(
    hgobj gobj, const char *topic_name, const char *id, const char *link, json_t *options
);
typedef json_t *(*mt_node_instances_fn)(
    hgobj gobj,
    const char *topic_name,
    const char *pkey2_field,
    json_t *jn_filter,
    json_t *jn_options
);


typedef void *(*internal_method_fn)(hgobj gobj, void *data);
typedef json_t *(*mt_authenticate_fn)(hgobj gobj, const char *service, json_t *kw, hgobj src);
typedef json_t *(*mt_list_childs_fn)(hgobj gobj, const char *child_gclass, const char **attributes);
typedef int (*mt_stats_updated_fn)(
    hgobj gobj,
    hgobj stats_gobj,
    const char *attr_name,
    int type, // my ASN1 types
    SData_Value_t old_v,
    SData_Value_t new_v
);
typedef int (*mt_disable_fn)(hgobj gobj);
typedef int (*mt_enable_fn)(hgobj gobj);
typedef int (*mt_trace_on_fn)(hgobj gobj, const char *level, json_t *kw);
typedef int (*mt_trace_off_fn)(hgobj gobj, const char *level, json_t *kw);

typedef void (*mt_gobj_created_fn)(hgobj gobj, hgobj gobj_created);

typedef int (*future_method_fn)(hgobj gobj, void *data, int c, char x);

/*
 *  mt_command:
 *  First find command in gclass.cmds and execute it if found.
 *  If it doesn't exist then execute mt_command if it exists.
 *  Must return a json dict with "result", and "data" keys.
 *  Use the build_webix() helper to build the json.
 *  On error "result" will be a negative value, and "data" a string with the error description
 *  On successful "data" must return an any type of json data.
 *
 *  mt_stats or mt_command return 0 if there is a asynchronous response.
 *  For examples these will ocurr when the command is redirect to a event.
 */
typedef struct { // GClass methods (Yuneta framework methods)
    mt_create_fn mt_create;
    mt_create2_fn mt_create2;
    mt_destroy_fn mt_destroy;
    mt_start_fn mt_start;   // inside mt_start the gobj is already running
    mt_stop_fn mt_stop;    // inside mt_stop the gobj is already stopped
    mt_play_fn mt_play;
    mt_pause_fn mt_pause;
    mt_writing_fn mt_writing; // someone has written in the `name` attr. When path is 0, the gobj has been created with default values
    mt_reading_fn mt_reading;
    mt_subscription_added_fn mt_subscription_added;      // refcount: 1 on the first subscription of this event.
    mt_subscription_deleted_fn mt_subscription_deleted;    // refcount: 1 on the last subscription of this event.
    mt_child_added_fn mt_child_added;     // called when the child is built, just after mt_create call.
    mt_child_removed_fn mt_child_removed;   // called when the child is almost live
    json_function_t mt_stats;           // must return a webix json object 0 if asynchronous response, like all below. HACK match the stats's prefix only.
    json_function_t mt_command_parser;  // User command parser. Preference over gclass.command_table. Return webix.
    gobj_action_fn mt_inject_event;     // Won't use the static built-in gclass machine? process yourself your events.
    mt_create_resource_fn mt_create_resource;
    mt_list_resource_fn mt_list_resource; // Must return an iter, although empty
    mt_update_resource_fn mt_update_resource;
    mt_delete_resource_fn mt_delete_resource;
    mt_add_child_resource_link_fn mt_add_child_resource_link;
    mt_delete_child_resource_link_fn mt_delete_child_resource_link;
    mt_get_resource_fn mt_get_resource;
    future_method_fn mt_future24;
    mt_authenticate_fn mt_authenticate; // Return webix
    mt_list_childs_fn mt_list_childs;
    mt_stats_updated_fn mt_stats_updated;       // Return 0 if own the stats, or -1 if not.
    mt_disable_fn mt_disable;
    mt_enable_fn mt_enable;
    mt_trace_on_fn mt_trace_on;                 // Return webix
    mt_trace_off_fn mt_trace_off;               // Return webix
    mt_gobj_created_fn mt_gobj_created;         // ONLY for __yuno__.
    future_method_fn mt_future33;
    future_method_fn mt_future34;
    mt_publish_event_fn mt_publish_event;  // Return -1 (broke), 0 continue without publish, 1 continue and publish
    mt_publication_pre_filter_fn mt_publication_pre_filter; // Return -1,0,1
    mt_publication_filter_fn mt_publication_filter; // Return -1,0,1
    future_method_fn mt_future38;
    future_method_fn mt_future39;
    mt_create_node_fn mt_create_node;
    mt_update_node_fn mt_update_node;
    mt_delete_node_fn mt_delete_node;
    mt_link_nodes_fn mt_link_nodes;
    mt_link_nodes2_fn mt_link_nodes2;
    mt_unlink_nodes_fn mt_unlink_nodes;
    mt_unlink_nodes2_fn mt_unlink_nodes2;
    mt_get_node_fn mt_get_node;
    mt_list_nodes_fn mt_list_nodes;
    mt_shoot_snap_fn mt_shoot_snap;
    mt_activate_snap_fn mt_activate_snap;
    mt_list_snaps_fn mt_list_snaps;
    mt_treedbs_fn mt_treedbs;
    mt_treedb_topics_fn mt_treedb_topics;
    mt_topic_desc_fn mt_topic_desc;
    mt_topic_links_fn mt_topic_links;
    mt_topic_hooks_fn mt_topic_hooks;
    mt_node_parents_fn mt_node_parents;
    mt_node_childs_fn mt_node_childs;
    mt_node_instances_fn mt_node_instances;
    mt_save_node_fn mt_save_node;
    future_method_fn mt_future61;
    future_method_fn mt_future62;
    future_method_fn mt_future63;
    future_method_fn mt_future64;
} GMETHODS;

typedef struct { // Internal methods
    const char *lname;
    const internal_method_fn lm;
    const char *permission;
} LMETHOD;

/* Same as Python Pyramid framework
    __acl__ = [
##        (Allow, Authenticated, 'view'),
##        (Allow, Everyone, 'view'),
        (Allow, 'group:WebStratusLog', ('view', 'log')),
        (Allow, 'group:WebStratusControl', ('view', 'action', 'log')),
        (Allow, 'group:WebStratusAdministracion', ('view', 'new', 'action', 'edit', 'delete', 'log')),
        DENY_ALL
        ]
*/

typedef struct {
    const char *name;
    const char *description;
} trace_level_t;

typedef enum { // WARNING strings in s_gcflag
    gcflag_manual_start             = 0x0001,   // gobj_start_tree() don't start gobjs of this /gclass.
    gcflag_no_check_ouput_events    = 0x0002,   // When publishing don't check events in output_event_list.
    gcflag_ignore_unkwnow_attrs     = 0x0004,   // When creating a gobj, ignore not existing attrs
    gcflag_required_start_to_play   = 0x0008,   // Don't to play if no start done.
} gcflag_t;

typedef struct _GCLASS {
    struct _GCLASS *base;   // gclass inheritance
    const char *gclass_name;
    FSM *fsm;
    GMETHODS gmt;
    const LMETHOD *lmt;
    const sdata_desc_t *tattr_desc;
    size_t priv_size;
    __ace__ ** __acl__; // list of Access Control List: sequence of __ace__ tuples.
    /*
     *  16 levels of user trace, with name and description, applicable to gobj or gclass (all his instances)
     *  More until 16 internal system levels.
     */
    const trace_level_t *s_user_trace_level;    // up to 16
    const sdata_desc_t *command_table; // if it exits then mt_command is not used. Both must return a webix json.
    gcflag_t gcflag;

    /*
     *  Private
     */
    uint32_t __instances__;      // instances of this gclass
    uint32_t __gclass_trace_level__;
    uint32_t __gclass_no_trace_level__;
    BOOL fsm_checked;
} GCLASS;


/*********************************************************************
 *      Prototypes
 *********************************************************************/
/*--------------------------------------------*
 *  Start up functions
 *--------------------------------------------*/
PUBLIC int gobj_start_up(
    json_t *jn_global_settings,
    int (*load_persistent_attrs_fn)(hgobj gobj),
    int (*save_persistent_attrs_fn)(hgobj gobj),
    int (*remove_persistent_attrs_fn)(hgobj gobj),
    json_t * (*list_persistent_attrs_fn)(void),
    json_function_t global_command_parser_fn,
    json_function_t global_stats_parser_fn
);
PUBLIC void gobj_shutdown(void);
PUBLIC BOOL gobj_is_shutdowning(void);
PUBLIC void gobj_end(void);

/*--------------------------------------------*
 *  Register functions
 *--------------------------------------------*/
PUBLIC int gobj_register_yuno(
    const char *yuno_role,
    GCLASS *gclass,
    BOOL to_free
);
PUBLIC hgobj gobj_yuno_factory(
    const char *realm_name,
    const char *yuno_name,
    const char *yuno_alias,
    json_t *jn_yuno_settings // own
);
PUBLIC int gobj_register_gclass(GCLASS *gclass);
PUBLIC GCLASS * gobj_find_gclass(const char *gclass_name, BOOL verbose);
PUBLIC int gobj_walk_gclass_list(
    int (*cb_walking)(GCLASS *gclass, void *user_data),
    void *user_data
);

/*
 *  gobj_repr_gclass_register():
 *      Return [{gclass:s, role:s}]
 *
 *  gobj_repr_service_register():
 *      Return [{gclass:s, service:s}]
 *
 *  gobj_repr_unique_register():
 *      Return [s], list with the unique gobj names
 *
 */
PUBLIC json_t * gobj_repr_gclass_register(void);
PUBLIC json_t * gobj_repr_service_register(void);
PUBLIC json_t * gobj_repr_unique_register(void);

/*
 *  Factory to create service gobj
 *  Used in entry_point, to run services
 *  Internally it uses gobj_create_tree()
 */
PUBLIC hgobj gobj_service_factory(
    const char *name,
    json_t * jn_service_config // owned
);

/*
 *  Return webix response ({"result": 0,...} 0 successful authentication, -1 error)
 */
PUBLIC json_t *gobj_authenticate(
    hgobj gobj,
    const char *service,
    json_t *kw,
    hgobj src
);

/*--------------------------------------------*
 *  Creation functions
 *--------------------------------------------*/
PUBLIC hgobj gobj_create(
    const char *name,
    GCLASS *gclass,
    json_t *kw,
    hgobj parent
);
PUBLIC hgobj gobj_create_unique(
    const char *name,
    GCLASS *gclass,
    json_t *kw,
    hgobj parent
);
PUBLIC hgobj gobj_create_volatil(
    const char *name,
    GCLASS *gclass,
    json_t *kw,
    hgobj parent
);
PUBLIC hgobj gobj_create_service(
    const char *name,
    GCLASS *gclass,
    json_t *kw,
    hgobj parent
);

/*
 * Json config of tree
 *
{
    'name': 'x',
    'gclass': 'X',
    'autostart': true,
    'autoplay': true,
    'kw': {
    }
    'zchilds': [
        {
            'name': 'xx',
            'gclass': 'Xx',
            'autostart': true,
            'kw': {
            }
            'zchilds': [
            ]
        }
    ]
}

When a child is created a EV_ON_SETUP event is sended to parent, if admitted.
When all children are created a EV_ON_SETUP_COMPLETE is sended to parent, if admitted.
(if admitted = if event is in the input_event_list)
*/
PUBLIC hgobj gobj_create_tree( // TOO implicit: if no subscriber key is given, the parent is the subscriber.
    hgobj parent,
    const char *tree_config,    // It can be json_config.
    const char *json_config_variables,
    const char *ev_on_setup,
    const char *ev_on_setup_complete
);

//FUTURE implement incref/decref
PUBLIC void gobj_destroy(hgobj gobj);  // must be compatible free()

PUBLIC int gobj_destroy_named_childs(hgobj gobj, const char *name); // with auto pause/stop
PUBLIC void gobj_destroy_childs(hgobj gobj);
PUBLIC GCLASS *gobj_subclass_gclass(GCLASS *base, const char *gclass_name);

/*--------------------------------------------*
 *  Resource functions DEPRECATED
 *--------------------------------------------*/
PUBLIC hsdata gobj_create_resource(
    hgobj gobj,
    const char *resource,
    json_t *kw  // owned
);
PUBLIC int gobj_update_resource(hgobj gobj, hsdata hs);
PUBLIC int gobj_delete_resource(hgobj gobj, hsdata hs);
PUBLIC int gobj_add_child_resource_link(hgobj gobj, hsdata hs_parent, hsdata hs_child);
PUBLIC int gobj_delete_child_resource_link(hgobj gobj, hsdata hs_parent, hsdata hs_child);

/*
 *  Remember free with rc_free_iter(iter, TRUE, 0);
 */
PUBLIC dl_list_t * gobj_list_resource( // WARNING free return with rc_free_iter(iter, TRUE, 0);
    hgobj gobj,
    const char *resource,
    json_t *jn_filter  // owned
);
PUBLIC hsdata gobj_get_resource(
    hgobj gobj,
    const char *resource,
    json_int_t parent_id,
    json_int_t id
);


/*--------------------------------------------------*
 *  Node functions. Don't use resource, use this
 *--------------------------------------------------*/
PUBLIC json_t *gobj_treedbs( // Return a list with treedb names
    hgobj gobj
);

PUBLIC json_t *gobj_treedb_topics(
    hgobj gobj,
    const char *treedb_name,
    const char *options // "dict" return list of dicts, otherwise return list of strings
);

PUBLIC json_t *gobj_topic_desc(
    hgobj gobj,
    const char *topic_name
);

PUBLIC json_t *gobj_topic_links(
    hgobj gobj,
    const char *treedb_name,
    const char *topic_name
);

PUBLIC json_t *gobj_topic_hooks(
    hgobj gobj,
    const char *treedb_name,
    const char *topic_name
);

PUBLIC json_t *gobj_create_node( // Return is NOT YOURS, WARNING This does NOT auto build links
    hgobj gobj,
    const char *topic_name,
    json_t *kw, // owned
    const char *options // "permissive"
);

PUBLIC int gobj_save_node( // Direct saving to tranger. WARNING be care, must be a pure node
    hgobj gobj,
    json_t *node // not owned
);

PUBLIC json_t *gobj_update_node( // Return is NOT YOURS, High level: DOES auto build links
    hgobj gobj,
    const char *topic_name,
    json_t *kw,    // owned
    const char *options // "create" ["permissive"], "clean"
);

PUBLIC int gobj_delete_node(
    hgobj gobj,
    const char *topic_name,
    json_t *kw,    // owned
    const char *options // "force"
);

PUBLIC int gobj_link_nodes(
    hgobj gobj,
    const char *hook,
    json_t *parent_node,    // not owned
    json_t *child_node      // not owned
);

PUBLIC int gobj_link_nodes2(
    hgobj gobj,
    const char *parent_ref,     // parent_topic_name^parent_id^hook_name
    const char *child_ref       // child_topic_name^child_id
);

PUBLIC int gobj_unlink_nodes(
    hgobj gobj,
    const char *hook,
    json_t *parent_node,    // not owned
    json_t *child_node      // not owned
);

PUBLIC int gobj_unlink_nodes2(
    hgobj gobj,
    const char *parent_ref,     // parent_topic_name^parent_id^hook_name
    const char *child_ref       // child_topic_name^child_id
);

PUBLIC json_t *gobj_get_node( // Return is NOT YOURS
    hgobj gobj,
    const char *topic_name,
    const char *id,
    json_t *jn_options  // owned "collapsed"
);

PUBLIC json_t *gobj_list_nodes( // Return MUST be decref
    hgobj gobj,
    const char *topic_name,
    json_t *jn_filter,  // owned
    json_t *jn_options  // owned "collapsed"
);

PUBLIC json_t *gobj_node_parents( // Return MUST be decref
    hgobj gobj,
    const char *topic_name,
    const char *id,
    const char *link,
    json_t *jn_options  // owned "collapsed"
);

PUBLIC json_t *gobj_node_childs( // Return MUST be decref
    hgobj gobj,
    const char *topic_name,
    const char *id,
    const char *hook,
    json_t *jn_options  // owned "collapsed"
);

PUBLIC json_t *gobj_node_instances(
    hgobj gobj,
    const char *topic_name,
    const char *pkey2_field,
    json_t *jn_filter,  // owned
    json_t *jn_options // owned, "collapsed"
);

PUBLIC int gobj_shoot_snap(hgobj gobj, const char *tag); // tag the current tree db
PUBLIC int gobj_activate_snap(hgobj gobj, const char *tag); // Activate tag (stop/start the gobj)
PUBLIC json_t *gobj_list_snaps(hgobj gobj, json_t *filter); // Return MUST be decref, list of snaps


/*--------------------------------------------*
 *  Operational functions
 *--------------------------------------------*/
PUBLIC void *gobj_exec_internal_method(hgobj gobj, const char *lmethod, void *data);

PUBLIC int gobj_start(hgobj gobj);
PUBLIC int gobj_start_childs(hgobj gobj);   // only direct childs
PUBLIC int gobj_start_tree(hgobj gobj);     // childs with gcflag_manual_start flag are not started.
PUBLIC int gobj_stop(hgobj gobj);
PUBLIC int gobj_stop_childs(hgobj gobj);    // only direct childs
PUBLIC int gobj_stop_tree(hgobj gobj);      // all tree of childs

PUBLIC int gobj_play(hgobj gobj);
PUBLIC int gobj_pause(hgobj gobj);

/*
 *  If service has mt_play then start only the service gobj.
 *      (Let mt_play be responsible to start their tree)
 *  If service has not mt_play then start the tree with gobj_start_tree().
 */
PUBLIC int gobj_autostart_services(void);
PUBLIC int gobj_autoplay_services(void);
PUBLIC int gobj_stop_services(void);

PUBLIC int gobj_disable(hgobj gobj); // if not exists mt_disable() then gobj_stop_tree()
PUBLIC int gobj_enable(hgobj gobj); // if not exists mt_enable() then gobj_start_tree()

PUBLIC void gobj_set_yuno_must_die(void);
PUBLIC BOOL gobj_get_yuno_must_die(void);
PUBLIC void gobj_set_exit_code(int exit_code);
PUBLIC int gobj_get_exit_code(void);

PUBLIC hgobj gobj_default_service(void);
PUBLIC hgobj gobj_find_service(const char *service, BOOL verbose);
PUBLIC hgobj gobj_nearest_top_service(hgobj gobj); // Return nearest top service of gobj (service or __yuno__)

PUBLIC hgobj gobj_find_gobj(const char *gobj_path); // find gobj by path (name or oid)
/*
 *  WARNING: don't use gobj_find_unique_gobj() for find services.
 *  Better use always gobj_find_service(), and don't save locally the service's gobj.
 */
PUBLIC hgobj gobj_find_unique_gobj(const char *unique_name, BOOL verbose);

PUBLIC rc_instance_t * gobj_first_child(hgobj gobj, hgobj *child);
PUBLIC rc_instance_t * gobj_last_child(hgobj gobj, hgobj *child);
PUBLIC rc_instance_t * gobj_next_child(rc_instance_t *iter, hgobj *child);
PUBLIC rc_instance_t * gobj_prev_child(rc_instance_t *iter, hgobj *child);
PUBLIC rc_instance_t * gobj_child_by_index(hgobj gobj, size_t index, hgobj *child); // relative to 1
PUBLIC size_t gobj_child_index(hgobj parent, hgobj child);

PUBLIC hgobj gobj_child_by_name(hgobj gobj_, const char *name, rc_instance_t** i_child);
PUBLIC size_t gobj_child_size(hgobj gobj);

PUBLIC int gobj_walk_gobj_childs(
    hgobj gobj,
    walk_type_t walk_type,
    int (*cb_walking)(rc_instance_t *i_gobj, hgobj gobj, void *user_data, void *user_data2, void *user_data3),
    void *user_data,
    void *user_data2,
    void *user_data3
);
PUBLIC int gobj_walk_gobj_childs_tree(
    hgobj gobj,
    walk_type_t walk_type,
    int (*cb_walking)(rc_instance_t *i_gobj, hgobj gobj, void *user_data, void *user_data2, void *user_data3),
    void *user_data,
    void *user_data2,
    void *user_data3
);

/*
    jn_filter: a dict with names of attributes with the value to mach,
               or a system name like:

                    '__inherited_gclass_name__'
                    '__gclass_name__'
                    '__gobj_name__'
                    '__prefix_gobj_name__'
                    '__state__'
                    '__disabled__'

    gobj_find_child() returns the first matched child.

 */
PUBLIC hgobj gobj_find_child(
    hgobj gobj,
    json_t *jn_filter // owned
);

/*
 *  Returns the first matched child of gclass in bottom line.
 */
PUBLIC hgobj gobj_find_bottom_child_by_gclass(
    hgobj gobj,
    const char *gclass_name
);

/*
 *  Returns a list (iter) with all matched childs.
 *  If dl_list is null a dynamic dl_list (iter) will be created and returned,
 *      and you must free with rc_free_iter(dl_list, TRUE, 0);
 *  If you pass the dl_list
 *      you must free with the iter with rc_free_iter(dl_list, FALSE, 0);
 *
 *  Check ONLY first level of childs.
 */
PUBLIC dl_list_t *gobj_match_childs(
    hgobj gobj,
    dl_list_t *dl_list,
    json_t *jn_filter   // owned
);

/*
 *  Returns a list (iter) with all matched childs.
 *  If dl_list is null a dynamic dl_list (iter) will be created and returned,
 *      and you must free with rc_free_iter(dl_list, TRUE, 0);
 *  If you pass the dl_list
 *      you must free with the iter with rc_free_iter(dl_list, FALSE, 0);
 *
 *  Check deep levels of childs
 */
PUBLIC dl_list_t *gobj_match_childs_tree(
    hgobj gobj,
    dl_list_t *dl_list,
    json_t *jn_filter   // owned
);

/*
 *  Returns a list (iter) with all matched childs.
 *  If dl_list is null a dynamic dl_list (iter) will be created and returned,
 *      and you must free with rc_free_iter(dl_list, TRUE, 0);
 *  If you pass the dl_list
 *      you must free with the iter with rc_free_iter(dl_list, FALSE, 0);
 *
 *  Check ONLY first level of childs.
 */
PUBLIC dl_list_t *gobj_match_childs_by_strict_gclass(hgobj gobj, const char *gclass_name);
PUBLIC dl_list_t *gobj_match_childs_by_inherited_gclass(hgobj gobj, const char *gclass_name);
/*
 *  Returns a list (iter) with all matched childs.
 *  If dl_list is null a dynamic dl_list (iter) will be created and returned,
 *      and you must free with rc_free_iter(dl_list, TRUE, 0);
 *  If you pass the dl_list
 *      you must free with the iter with rc_free_iter(dl_list, FALSE, 0);
 *
 *  Check deep levels of childs
 */
PUBLIC dl_list_t *gobj_match_childs_tree_by_strict_gclass(hgobj gobj, const char *gclass_name);

/*
 *  Returns a list (iter) with all matched childs with regular expression.
 *  If dl_list is null a dynamic dl_list (iter) will be created and returned,
 *      and you must free with rc_free_iter(dl_list, TRUE, 0);
 *  If you pass the dl_list
 *      you must free with the iter with rc_free_iter(dl_list, FALSE, 0);
 *
 *  Check ONLY first level of childs.
 */
PUBLIC dl_list_t *gobj_filter_childs_by_re_name(dl_list_t *dl_childs, const char *re_name);

/*
 *  SDATA information of subscription resource, used by functions:
 *
 *      gobj_find_subscriptions()
 *      gobj_subscribe_event()
 *      gobj_unsubscribe_event()
 *

SDATA Schema of subs (subscription)
===================================
"publisher":            (pointer)int    // publisher gobj
"subscriber:            (pointer)int    // subscriber gobj
"event":                str             // event name subscribed
"rename_event_name":    str             // publish with other event name
"subs_flag":            int             // subscription flag. See subs_flag
"__config__":           json            // subscription config.
"__global__":           json            // global event kw. This jn_global is extended with publishing kw.
"__local__":            json            // local event kw. This jn_local is removed from publishing kw.
"__filter__":           json            // filter event kw. This jn_filter will be applied in Subscribing.


Possible values for config arguments:
-------------------------------------

* "__rename_event_name__": "new event name"

    You can rename the output original event name.
    The :attr:`original_event_name` attribute is added to
    the sent event with the value of the original event name.


* "__hard_subscription__":  bool

    True for permanent subscription.

    This subscription cannot be remove,
    (Well, with force you can)

* "__first_shot__": bool

    If True then subscriber wants receive the event on subscription.
    Subscribing gobj mt_subscription_added() will be called
    and it's his responsability to check the flag and to emit the event.

* "__share_kw__": bool
    Use the same kw to all publications. This let subscribers modify the same kw.
    If not __share_kw__ then each publication receives a twin of kw.

* "__trans_filter__": string or string's list
    Transform kw to publish with transformation filters


Subdictionaries to use in gobj_subscribe_event() kw
===================================================

HACK Only these four subdictionaries keys are let, remain of keywords will be ignored!

    __config__   Parameters to custom gobj_subscribe_event() behaviour.

    __global__  Base kw to be merged with the publishing kw (base kw will be updated by publishing kw).
                It's the common data received in each publication. Constants, global variables!

    __local__   dictionary or list with Keys (path) to be deleted from kw before to publish.

    __filter__  Selection Filter: Enable to publish only messages matching the filter.
                Used by gobj_publish_event().


*/

typedef enum {
    __rename_event_name__   = 0x00000001,
    __hard_subscription__   = 0x00000002,
    __first_shot__          = 0x00000004,
    __share_kw__            = 0x00000008,   // Don't twin kw, use the same.
} subs_flag_t;


/*
 *  Return schema of subcriptions hsdata
 */
PUBLIC const sdata_desc_t *gobj_subscription_schema(void);

/*
 *  Return number of subcriptions
 */
PUBLIC size_t gobj_subscriptions_size(hgobj publisher);

/*
 *  Return number of subcribings
 */
PUBLIC size_t gobj_subscribings_size(hgobj subscriber);

/*
 *  Return a iter of subscriptions (sdata) in the publisher gobj,
 *  filtering by matching: event,kw (__config__, __global__, __local__, __filter__),subscriber
 *  Free return with rc_free_iter(iter, TRUE, FALSE);
 */
PUBLIC dl_list_t *gobj_find_subscriptions(
    hgobj publisher,
    const char *event,
    json_t *kw,             // kw (__config__, __global__, __local__, __filter__)
    hgobj subscriber
);
/*
 *  Return a iter of subscribings (sdata) of the subcriber gobj,
 *  filtering by matching: event,kw (__config__, __global__, __local__, __filter__), publisher
 *  Free return with rc_free_iter(iter, TRUE, FALSE);
 */
PUBLIC dl_list_t *gobj_find_subscribings(
    hgobj subscriber,
    const char *event,
    json_t *kw,             // kw (__config__, __global__, __local__, __filter__)
    hgobj publisher
);

PUBLIC hsdata gobj_subscribe_event( // Idempotent function
    hgobj publisher,
    const char *event,
    json_t *kw, // kw (__config__, __global__, __local__, __filter__)
    hgobj subscriber
);
PUBLIC int gobj_unsubscribe_event(
    hgobj publisher,
    const char *event,
    json_t *kw, // kw (__config__, __global__, __local__, __filter__)
    hgobj subscriber
);
PUBLIC int gobj_unsubscribe_event2(
    hsdata subs
);
PUBLIC int gobj_unsubscribe_list(
    dl_list_t *dl_subs, // iter of subscription hsdata
    BOOL free_iter,
    BOOL force  // delete hard_subscription subs too
);

/*
 *  Set funtion to be applied in Selection Filter __filter__
 *  Default is kw_match_simple()
 *  Return old function
 */
PUBLIC kw_match_fn gobj_set_publication_selection_filter_fn(kw_match_fn kw_match);

/*
 *  Add transformation filter function,
 *  to be applied with __config__`__trans_filter__
 *
 *  There is a internal transformation filter setup 'webix',
 *  that transform kw in:
     {
        "result": 0,
        "comment": null,
        "schema": null,
        "data": kw
    }
 *
 */
PUBLIC int gobj_add_publication_transformation_filter_fn(
    const char *name,
    json_t * (*transformation_fn)(
        json_t *kw // owned
    )
);

/*
 *  The kw process to publish is:
 *  Apply
 *      1) __filter__ or mt_publication_filter() to select publication
 *      2) __local__ to remove local keys from kw
 *      3) __trans_filter__ to transform kw
 *      4) __global__ to add global keys
 *
 */
PUBLIC int gobj_publish_event( // Return the sum of sent events returns.
    hgobj publisher,
    const char *event,
    json_t *kw  // this kw extends kw_request. It can be modified in mt_future24.
);

PUBLIC int gobj_send_event(
    hgobj dst,
    const char *event,
    json_t *kw,
    hgobj src
);
PUBLIC int gobj_send_event_to_gclass_instances(
    hgobj gobj,
    const char *gclass_name,
    const char *event,
    json_t *kw,
    hgobj src
);
PUBLIC int gobj_send_event_to_named_child(  // to child with name ``name``
    hgobj gobj,
    const char *name,
    const char *event,
    json_t *kw,
    hgobj src
);
PUBLIC int gobj_send_event_to_childs(  // only to childs supporting the event
    hgobj gobj,
    const char *event,
    json_t *kw,
    hgobj src
);
PUBLIC int gobj_send_event_to_childs_tree( // same as gobj_send_event_to_childs but recursive
    hgobj gobj,
    const char *event,
    json_t *kw,
    hgobj src
);
// same as gobj_send_event_to_childs but recursive by levels instead of traverse the tree.
// by levels: first all childs, next all childs of the childs, etc
PUBLIC int gobj_send_event_to_childs_tree_by_level(
    hgobj gobj,
    const char *event,
    json_t *kw,
    hgobj src
);


/*--------------------------------------------*
 *  Organization functions
 *--------------------------------------------*/
PUBLIC hgobj gobj_set_bottom_gobj(hgobj gobj, hgobj bottom_gobj); // inherit attributes
PUBLIC hgobj gobj_last_bottom_gobj(hgobj gobj);
PUBLIC hgobj gobj_bottom_gobj(hgobj gobj);

/*--------------------------------------------*
 *  Attributes functions
 *--------------------------------------------*/
/*
 *  gobj_load_persistent_attrs() is automatically executed on creation of gobj.
 *  The persistent attributes saved have the more prevalence over other json configuration
 *  ONLY unique gobjs can load/save persistent attributes.
 *  gobj_save_persistent_attrs() must be manually executed.
 */
PUBLIC int gobj_load_persistent_attrs(hgobj gobj);
PUBLIC int gobj_save_persistent_attrs(hgobj gobj);
PUBLIC int gobj_remove_persistent_attrs(hgobj gobj, BOOL recursive);
PUBLIC json_t * gobj_list_persistent_attrs(void);

/*
 *  Attribute functions WITHOUT bottom inheritance
 */
PUBLIC hsdata gobj_hsdata(hgobj gobj);
PUBLIC const sdata_desc_t * gobj_tattr_desc(hgobj gobj);
PUBLIC const sdata_desc_t * gobj_attr_desc(hgobj gobj, const char *name);
PUBLIC const sdata_desc_t * gobj_bottom_attr_desc(hgobj gobj, const char *name);

PUBLIC BOOL gclass_has_attr(GCLASS *gclass, const char *name);
PUBLIC BOOL gobj_has_attr(hgobj gobj, const char *name);
PUBLIC BOOL gobj_has_bottom_attr(hgobj gobj_, const char *name);
PUBLIC int gobj_attr_type(hgobj gobj, const char *name); // See 00_asn1_snmp.h for types

/*
 *  Attribute functions WITH bottom inheritance
 */
/*
 *  Attributes low level functions
 *  Be care with this function. If you don't cast well: DANGER overflow!
 *  Acceso directo a la variable (con herencia de bottoms): puntero y descripción del atributo.
 */
PUBLIC hsdata gobj_hsdata2(hgobj gobj, const char *name, BOOL verbose);
PUBLIC void *gobj_danger_attr_ptr(hgobj gobj, const char *name);
PUBLIC void *gobj_danger_attr_ptr2(hgobj gobj_, const char *name, const sdata_desc_t **pit);

/*------------------------------*
 *  Attributes read functions
 *------------------------------*/

PUBLIC json_t *gobj_read_attr( // Return is yours! Must be decref.
    hgobj gobj,
    const char *name
);
PUBLIC json_t *gobj_read_user_data(
    hgobj gobj,
    const char *name
);
PUBLIC json_t *gobj_kw_get_user_data(
    hgobj gobj,
    const char *path,
    json_t *default_value,
    kw_flag_t flag
);

PUBLIC const char *gobj_read_str_attr(hgobj gobj, const char *name);
PUBLIC BOOL gobj_read_bool_attr(hgobj gobj, const char *name);
PUBLIC int32_t gobj_read_int32_attr(hgobj gobj, const char *name);
PUBLIC uint32_t gobj_read_uint32_attr(hgobj gobj, const char *name);
PUBLIC int64_t gobj_read_int64_attr(hgobj gobj, const char *name);
PUBLIC uint64_t gobj_read_uint64_attr(hgobj gobj, const char *name);
PUBLIC uint64_t gobj_read_integer_attr(hgobj gobj, const char *name);
PUBLIC double gobj_read_real_attr(hgobj gobj, const char *name);
PUBLIC json_t *gobj_read_json_attr(hgobj gobj, const char *name); // WARNING not incref, it's not your own.
PUBLIC void *gobj_read_pointer_attr(hgobj gobj, const char *name);
PUBLIC dl_list_t *gobj_read_dl_list_attr(hgobj gobj, const char *name);
PUBLIC dl_list_t *gobj_read_iter_attr(hgobj gobj, const char *name);
PUBLIC const sdata_desc_t * gobj_read_iter_schema(hgobj gobj, const char *name);
PUBLIC BOOL gobj_is_readable_attr(hgobj gobj, const char *name); // True is attr is SDF_RD (public readable)
PUBLIC SData_Value_t gobj_read_default_attr_value(hgobj gobj, const char* name);  // Only for basic types

/*-------------------------------------------------------*
 *  Attributes write functions
 *  The write functions will cause mt_writing() call.
 *-------------------------------------------------------*/

PUBLIC int gobj_write_attr(
    hgobj gobj,
    const char *name,
    json_t *value  // owned
);
PUBLIC int gobj_write_user_data(
    hgobj gobj,
    const char *name,
    json_t *value // owned
);
PUBLIC int gobj_kw_set_user_data(
    hgobj gobj,
    const char *path,   // The last word after . is the key
    json_t *value // owned
);

PUBLIC int gobj_write_str_attr(hgobj gobj, const char *name, const char *value);
PUBLIC int gobj_write_bool_attr(hgobj gobj, const char *name, BOOL value);
PUBLIC int gobj_write_int32_attr(hgobj gobj, const char *name, int32_t value);
PUBLIC int gobj_write_uint32_attr(hgobj gobj, const char *name, uint32_t value);
PUBLIC int gobj_write_int64_attr(hgobj gobj, const char *name, int64_t value);
PUBLIC int gobj_write_uint64_attr(hgobj gobj, const char *name, uint64_t value);
PUBLIC int gobj_write_integer_attr(hgobj gobj, const char *name, uint64_t value);
PUBLIC int gobj_write_real_attr(hgobj gobj, const char *name, double value);
PUBLIC int gobj_write_json_attr(hgobj gobj, const char *name, json_t *value); // WARNING json is incref (new V2)
PUBLIC int gobj_write_pointer_attr(hgobj gobj, const char *name, void *value);
PUBLIC BOOL gobj_is_writable_attr(hgobj gobj, const char *name);  // True is attr is SDF_WR or SDF_PERSIST (public writable)
PUBLIC json_t *gobj_get_writable_attrs(hgobj gobj); // Return is yours, decref!
PUBLIC int gobj_update_writable_attrs(
    hgobj gobj,
    json_t *jn_attrs // owned
);


/*--------------------------------------------*
 *  Info functions
 *--------------------------------------------*/
PUBLIC const char *gobj_yuno_realm_name(void);
PUBLIC const char *gobj_yuno_role(void);
PUBLIC const char *gobj_yuno_name(void);
PUBLIC const char *gobj_yuno_role_plus_name(void);
PUBLIC const char *gobj_yuno_alias(void);

PUBLIC hgobj gobj_yuno(void);
PUBLIC const char * gobj_name(hgobj gobj);
PUBLIC const char * gobj_gclass_name(hgobj gobj);
PUBLIC GCLASS * gobj_gclass(hgobj gobj);
PUBLIC int gobj_instances(hgobj gobj);
PUBLIC const char * gobj_full_name(hgobj gobj);
PUBLIC char *gobj_full_name2(hgobj gobj, char *bf, int bfsize); // Full name  with states of running/playing
PUBLIC char *gobj_full_bottom_name(hgobj gobj, char *bf, int bfsize); // Full tree bottom names with states of running/playing
PUBLIC const char * gobj_short_name(hgobj gobj);    // (gclass^name)
PUBLIC const char * gobj_escaped_short_name(hgobj gobj);    // (gclass^name)
PUBLIC const char * gobj_snmp_name(hgobj gobj);
PUBLIC BOOL gobj_is_volatil(hgobj gobj);

PUBLIC BOOL gobj_typeof_gclass(hgobj gobj, const char *gclass_name);            /* strict same gclass */
PUBLIC BOOL gobj_typeof_subgclass(hgobj gobj, const char *gclass_name);         /* check base gclass */
PUBLIC BOOL gobj_typeof_inherited_gclass(hgobj gobj, const char *gclass_name);  /* check inherited (bottom) gclass */

PUBLIC BOOL gobj_change_state(hgobj gobj, const char *new_state);
PUBLIC const char * gobj_current_state(hgobj gobj);
PUBLIC BOOL gobj_in_this_state(hgobj gobj, const char *state);
PUBLIC int gobj_cmp_current_state(hgobj gobj, const char *state);
PUBLIC const EVENT * gobj_input_event(hgobj gobj, const char* event);
PUBLIC const EVENT * gobj_output_event(hgobj gobj, const char* event);
PUBLIC const EVENT * gobj_output_event_list(hgobj gobj);
PUBLIC BOOL gobj_event_in_input_event_list(
    hgobj gobj,
    const char *event,
    event_flag_t flag
);
PUBLIC BOOL gobj_event_in_output_event_list(
    hgobj gobj,
    const char *event,
    event_flag_t flag
);

PUBLIC void * gobj_priv_data(hgobj gobj);
PUBLIC hgobj gobj_parent(hgobj gobj);
PUBLIC BOOL gobj_is_running(hgobj gobj);
PUBLIC BOOL gobj_is_playing(hgobj gobj);
PUBLIC BOOL gobj_is_service(hgobj gobj);
PUBLIC BOOL gobj_is_disabled(hgobj gobj);
PUBLIC BOOL gobj_is_unique(hgobj gobj);

/*
 *  build_webix()
 *  Must return a json dict with "result", "schema" and "data" keys.
 *  On error "result" will be a negative value, and "data" a string with the error description
 *  On successful "data" must return an any type of json data.
 *  "schema" will describe the format of data in "data".

    {
        "result": Integer,
        "comment": String,
        "schema": Array or Object,
        "data": Array or Object
    }

 *
 */
#define WEBIX_RESULT(webix)     (kw_get_int((webix), "result", 0, 0))
#define WEBIX_COMMENT(webix)    (kw_get_str((webix), "comment", "", 0))
#define WEBIX_SCHEMA(webix)     (kw_get_dict_value((webix), "comment", 0, 0))
#define WEBIX_DATA(webix)       (kw_get_dict_value((webix), "data", 0, 0))

PUBLIC json_t *build_webix(
    json_int_t result,
    json_t *jn_comment, // owned
    json_t *jn_schema,  // owned
    json_t *jn_data     // owned
);

/*
 *  Add to kw the metadata of yuno
 *  key: __md_yuno__,
 *  value: a dict with:
 *      __t__, current_time
 *      __origin__, source
 *      hostname,
 *      realm_name,
 *      yuno_role,
 *      yuno_name,
 *      gobj_name,
 *      pid
 */
PUBLIC int append_yuno_metadata(hgobj gobj, json_t *kw, const char *source);

/*
 *  Return a dict with attrs marked with SDF_STATS and stats_metadata
 */
PUBLIC json_t * gobj_stats( // Call mt_stats() or build_stats()
    hgobj gobj,
    const char* stats,
    json_t* kw,
    hgobj src
);


/*
 *  Set stats, path relative to gobj, including the attribute
 */
PUBLIC int gobj_set_stats(
    hgobj gobj,
    const char* path,
    uint32_t set
);

/*
 *  Check stats, path relative to gobj, including the attribute
 */
PUBLIC BOOL gobj_has_stats(
    hgobj gobj,
    const char* path
);

/*
 *  mt_command must return a webix json or 0 on asynchronous responses.
 */
PUBLIC json_t *gobj_command(
    hgobj gobj,
    const char *command,
    json_t *kw,
    hgobj src
);

/*
 *  gobj_list_childs(): return a list with dicc of child's attrs of typeof inherited gclass
 *  Simulate table with childs
 */
PUBLIC json_t *gobj_list_childs(
    hgobj gobj,
    const char *child_gclass,
    const char **attributes
);
PUBLIC int gobj_audit_commands( // Only one can audit. New calls will overwrite the callback audit_command_cb.
    int (*audit_command_cb)(
        const char *command,
        json_t *kw, // NOT owned
        void *user_data
    ),
    void *user_data
);

PUBLIC json_t *gclass_public_attrs(GCLASS *gclass);// Return a dict with gclass's public attrs (all if null).
PUBLIC json_t *gclass2json(GCLASS *gclass); // Return a dict with gclass's description.
PUBLIC json_t *gcflag2json(GCLASS *gclass); // Return a list with gcflag's strings.
PUBLIC json_t *gobj2json(hgobj gobj);       // Return a dict with gobj's description.
PUBLIC json_t *attr2json(hgobj gobj);       // Return a list with gobj's public attributes.

PUBLIC json_t *webix_gobj_tree(hgobj gobj); // Return webix style tree with gobj's tree.
PUBLIC json_t *view_gobj_tree(hgobj gobj);  // Return tree with gobj's tree.

PUBLIC json_t *gobj_gobjs_treedb_schema(const char *topic_name); // Return is NOT YOURS
PUBLIC json_t *gobj_gobjs_treedb_data(hgobj gobj);          // Return must be decref

PUBLIC int gobj_set_message_error(hgobj gobj, const char *msg);
PUBLIC int gobj_set_message_errorf(hgobj gobj, const char *msg, ...);
PUBLIC const char * gobj_get_message_error(hgobj gobj);


/*--------------------------------------------*
 *  Trace functions
 *--------------------------------------------*/
/*
 *  Global trace levels
 *  16 higher bits for global use.
 *  16 lower bits for user use.
 */
enum {
    TRACE_MACHINE           = 0x00010000,
    TRACE_CREATE_DELETE     = 0x00020000,
    TRACE_CREATE_DELETE2    = 0x00040000,
    TRACE_SUBSCRIPTIONS     = 0x00080000,
    TRACE_START_STOP        = 0x00100000,
    TRACE_MONITOR           = 0x00200000,
    TRACE_EVENT_MONITOR     = 0x00400000,
    TRACE_UV                = 0x00800000,
    TRACE_EV_KW             = 0x01000000,
};
#define TRACE_USER_LEVEL    0x0000FFFF
#define TRACE_GLOBAL_LEVEL  0xFFFF0000

/*
 *  Global trace level names:
 *
 *      "machine"
 *      "create_delete"
 *      "create_delete2"
 *      "subscriptions"
 *      "start_stop"
 *      "monitor"
 *      "event_monitor"
 *      "libuv"
 *      "ev_kw"
 */

/*
 *  gobj_repr_gclass_trace_levels():
 *      Return [{gclass:null, trace_levels:[s]}]
 */

PUBLIC json_t * gobj_repr_global_trace_levels(void);
/*
 *  gobj_repr_gclass_trace_levels():
 *      Return [{gclass:s, trace_levels:[s]}]
 */
PUBLIC json_t * gobj_repr_gclass_trace_levels(const char *gclass_name);

/*
 *  Return trace level list (internal and user defined)
 */
PUBLIC json_t *gobj_trace_level_list(GCLASS *gclass, BOOL not_internals);

/*
 *  Return list of trace levels
 *  Remember decref return
 */
PUBLIC json_t *gobj_trace_level_list2(
    GCLASS *gclass,
    BOOL with_global_levels,
    BOOL with_gclass_levels
);

/*
 *  Set trace levels and no-set trace levels, in gclass and gobj
 *      - if gobj is null then the trace level is global.
 *      - if level is empty, all levels are set/reset.
 *      - if gobj is not null then call mt_trace_on/mt_trace_off
 */
PUBLIC int gobj_set_gobj_trace(hgobj gobj, const char* level, BOOL set, json_t* kw);
PUBLIC int gobj_set_gclass_trace(GCLASS *gclass, const char *level, BOOL set);
PUBLIC int gobj_set_panic_trace(BOOL panic_trace);
PUBLIC int gobj_set_global_trace(const char* level, BOOL set); // If level is empty, set all global traces

/*
 *  Set no-trace-level
 */
PUBLIC int gobj_set_gclass_no_trace(GCLASS *gclass, const char *level, BOOL set);
PUBLIC int gobj_set_gobj_no_trace(hgobj gobj, const char *level, BOOL set);

/*
 *  Use this functions to see if you must trace in your gclasses
 */
PUBLIC uint32_t gobj_trace_level(hgobj gobj);
PUBLIC uint32_t gobj_no_trace_level(hgobj gobj);

/*
 *  Get traces set in tree of gclass or gobj
 */
PUBLIC json_t *gobj_get_gclass_trace_level_list(GCLASS *gclass);
PUBLIC json_t *gobj_get_gclass_no_trace_level_list(GCLASS *gclass);
PUBLIC json_t *gobj_get_gobj_trace_level_tree(hgobj gobj);
PUBLIC json_t *gobj_get_gobj_no_trace_level_tree(hgobj gobj);

/*
 *  Get traces set in gclass and gobj (return list of strings)
 */
PUBLIC json_t *gobj_get_global_trace_level(void);
PUBLIC json_t *gobj_get_gclass_trace_level(GCLASS *gclass);
PUBLIC json_t *gobj_get_gclass_no_trace_level(GCLASS *gclass);
PUBLIC json_t *gobj_get_gobj_trace_level(hgobj gobj);
PUBLIC json_t *gobj_get_gobj_no_trace_level(hgobj gobj);

PUBLIC int gobj_print_subscriptions(hgobj gobj);
PUBLIC int gobj_print_childs(dl_list_t *dl_childs, int verbose_level);

/*
 *  Stats functions
 */
PUBLIC json_int_t gobj_set_stat(hgobj gobj, const char *path, json_int_t value); // return old value
PUBLIC json_int_t gobj_incr_stat(hgobj gobj, const char *path, json_int_t value); // return new value
PUBLIC json_int_t gobj_decr_stat(hgobj gobj, const char *path, json_int_t value); // return new value
PUBLIC json_int_t gobj_get_stat(hgobj gobj, const char *path);
PUBLIC json_t *gobj_jn_stats(hgobj gobj);  // WARNING the json return is NOT YOURS!

/*
 *  2key: in-memory double-key para registrar json con acceso global
 *  Eschema 2key: {k1:{k2:value,},}
 *
 *  Registro:
 *      gobj_2key_register(k1, k2, value); // value incref, ref must be >= 2 else someone not unregister
 *
 *  Se puede recuperar `value` con:
 *      json_t *gobj_get_2key_value(k1, k2);
 *      Ex:
 *      // retorna `value` of {k1:{k2:value} (priv->tranger)
 *      gobj_2key_get_value("tranger", "fichador");
 *
 *  Se puede recuperar schema con:
 *      json_t *gobj_get_2key_schema();
 *      Ex:
 *      // retorna {k1:{k2:type,},}, sin `value`s!, en type: "",[],{}
 *      gobj_get_2key_schema();
 */
PUBLIC int gobj_2key_register(const char *key1, const char *key2, json_t *value);
PUBLIC int gobj_2key_deregister(const char *key1, const char *key2);
PUBLIC json_t *gobj_2key_get_schema(void);  // return is yours
PUBLIC json_t *gobj_2key_get_value(const char *key1, const char *key2); // WARNING return is NOT YOURS!


#ifdef __cplusplus
}
#endif


#endif

