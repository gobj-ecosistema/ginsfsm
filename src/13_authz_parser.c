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
 *  Return a webix json
 ***************************************************************************/
PUBLIC json_t *authzs_list(
    hgobj gobj,
    const char *auth,
    json_t *kw,
    hgobj src
)
{
    /*--------------------------------------*
     *  Build standard stats
     *--------------------------------------*/
    KW_INCREF(kw);
    json_t *jn_data = build_authzs(
        gobj,
        auth,
        kw,     // owned
        src
    );
    append_yuno_metadata(gobj, jn_data, auth);

    return msg_iev_build_webix(
        gobj,
        0,
        0,
        0,
        jn_data,  // owned
        kw // owned
    );
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t *build_authzs(
    hgobj gobj,
    const char *authz,
    json_t *kw,
    hgobj src
)
{
    if(!gobj) {
        KW_DECREF(kw);
        return sdataauth2json(gobj_get_global_authz_table());
    }
    json_t *jn_list = sdataauth2json(gobj_get_global_authz_table());
    const char *service = kw_get_str(kw, "service", "", 0);
    hgobj gobj_service = gobj_find_service(service, FALSE);
    if(gobj_service) {
        json_array_extend(
            jn_list,
            sdataauth2json(gobj_gclass(gobj_service)->authz_table)
        );
    }

    KW_DECREF(kw);
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

