/***********************************************************************
 *          AUTHZ_HELPER.H
 *
 *          Authz helper
 *
 *          Copyright (c) 2020 Niyamaka.
 *          All Rights Reserved.
***********************************************************************/
#include "13_authz_helper.h"

/***************************************************************
 *              Constants
 ***************************************************************/

/***************************************************************
 *              Structures
 ***************************************************************/

/***************************************************************
 *              Prototypes
 ***************************************************************/

/***************************************************************
 *              Data
 ***************************************************************/

/***************************************************************************
 *  Return list authzs of gobj if authz is empty
 *  else return authz dict
 ***************************************************************************/
PUBLIC json_t *authzs_list(
    hgobj gobj,
    const char *authz
)
{
    json_t *jn_list = 0;
    if(!gobj) { // Can be null
        jn_list = sdataauth2json(gobj_get_global_authz_table());
    } else {
        if(!gobj_gclass(gobj)->authz_table) {
            return 0;
        }
        jn_list = sdataauth2json(gobj_gclass(gobj)->authz_table);
    }

    if(empty_string(authz)) {
        return jn_list;
    }

    int idx; json_t *jn_authz;
    json_array_foreach(jn_list, idx, jn_authz) {
        const char *id = kw_get_str(jn_authz, "id", "", KW_REQUIRED);
        if(strcmp(authz, id)==0) {
            json_incref(jn_authz);
            json_decref(jn_list);
            return jn_authz;
        }
    }

    log_error(0,
        "gobj",         "%s", gobj_full_name(gobj),
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "authz not found",
        "authz",        "%s", authz,
        NULL
    );

    json_decref(jn_list);
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const sdata_desc_t *authz_get_level_desc(
    const sdata_desc_t *authz_table,
    const char *auth
)
{
    const sdata_desc_t *pcmd = authz_table;
    while(pcmd->name) {
        /*
         *  Alias have precedence if there is no json_fn authz function.
         *  It's the combination to redirect the authz as `name` event,
         */
        BOOL alias_checked = FALSE;
        if(!pcmd->json_fn && pcmd->alias) {
            alias_checked = TRUE;
            const char **alias = pcmd->alias;
            while(alias && *alias) {
                if(strcasecmp(*alias, auth)==0) {
                    return pcmd;
                }
                alias++;
            }
        }
        if(strcasecmp(pcmd->name, auth)==0) {
            return pcmd;
        }
        if(!alias_checked) {
            const char **alias = pcmd->alias;
            while(alias && *alias) {
                if(strcasecmp(*alias, auth)==0) {
                    return pcmd;
                }
                alias++;
            }
        }

        pcmd++;
    }
    return 0;
}

/***************************************************************************
 *  Return a webix json
 ***************************************************************************/
PUBLIC json_t *gobj_build_authzs_doc(
    hgobj gobj,
    const char *cmd,
    json_t *kw,
    hgobj src
)
{
    const char *authz = kw_get_str(kw, "authz", "", 0);
    const char *service = kw_get_str(kw, "service", "", 0);

    if(!empty_string(service)) {
        hgobj service_gobj = gobj_find_service(service, FALSE);
        if(!service_gobj) {
            return msg_iev_build_webix(gobj,
                -1,
                json_sprintf("Service not found: '%s'", service),
                0,
                0,
                kw  // owned
            );
        }

        json_t *jn_authzs = authzs_list(service_gobj, authz);
        if(!jn_authzs) {
            if(empty_string(authz)) {
                return msg_iev_build_webix(
                    gobj,
                    -1,
                    json_sprintf("Service without authzs table: '%s'", service),
                    0,
                    0,
                    kw // owned
                );
            } else {
                return msg_iev_build_webix(
                    gobj,
                    -1,
                    json_sprintf("Authz not found: '%s' in service: '%s'", authz, service),
                    0,
                    0,
                    kw // owned
                );
            }
        }

        return msg_iev_build_webix(
            gobj,
            0,
            0,
            0,
            jn_authzs,
            kw // owned
        );
    }

    json_t *jn_authzs = json_object();
    json_t *jn_global_list = sdataauth2json(gobj_get_global_authz_table());
    json_object_set_new(jn_authzs, "global authzs", jn_global_list);

    json_t *jn_services = gobj_services();
    int idx; json_t *jn_service;
    json_array_foreach(jn_services, idx, jn_service) {
        const char *service = json_string_value(jn_service);
        hgobj gobj_service = gobj_find_service(service, TRUE);
        if(gobj_service) {
            if(gobj_gclass(gobj_service)->authz_table) {
                json_t *l = authzs_list(gobj_service, authz);
                json_object_set_new(jn_authzs, service, l);
            }
        }
    }

    json_decref(jn_services);

    return msg_iev_build_webix(
        gobj,
        0,
        0,
        0,
        jn_authzs,
        kw // owned
    );
}

