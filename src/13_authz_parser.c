/***********************************************************************
 *          AUTHZ_PARSER.C
 *
 *          Authz parser
 *
 *          Copyright (c) 2017 Niyamaka.
 *          All Rights Reserved.
***********************************************************************/
#include "13_authz_parser.h"

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
 *  Return list authzs of gobj
 ***************************************************************************/
PUBLIC json_t *authzs_list(
    hgobj gobj,
    const char *auth
)
{
    if(!gobj) {
        return sdataauth2json(gobj_get_global_authz_table());
    }
    json_t *jn_list = sdataauth2json(gobj_gclass(gobj)->authz_table);

    return jn_list;
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

    hgobj service_gobj = 0;

    if(!empty_string(service)) {
        service_gobj = gobj_find_service(service, FALSE);
        if(!service_gobj) {
            return msg_iev_build_webix(
                gobj,
                -1,
                json_sprintf("Service not found: '%s'", service),
                0,
                0,
                kw // owned
            );
        }
    }

    json_t *jn_authzs = authzs_list(service_gobj, authz);
    if(!jn_authzs) {
        return msg_iev_build_webix(
            gobj,
            -1,
            json_sprintf("Authz not found: '%s'", authz),
            0,
            0,
            kw // owned
        );
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

