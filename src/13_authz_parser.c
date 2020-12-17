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
 *
 ***************************************************************************/
PRIVATE const char *sdata_authz_type(uint8_t type)
{
    if(ASN_IS_STRING(type)) {
        return "string";
    } else if(ASN_IS_BOOLEAN(type)) {
        return "boolean";
    } else if(ASN_IS_NATURAL_NUMBER(type)) {
        return "integer";
    } else if(ASN_IS_REAL_NUMBER(type)) {
        return "real";
    } else if(ASN_IS_JSON(type)) {
        return "json";
    } else {
        return "unknown";
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE void add_authz_help(GBUFFER *gbuf, const sdata_desc_t *pcmds, BOOL extended)
{
    if(pcmds->alias) {
        gbuf_printf(gbuf, "- %-28s (", pcmds->name);
        const char **alias = pcmds->alias;
        if(*alias) {
            gbuf_printf(gbuf, "%s ", *alias);
        }
        alias++;
        while(*alias) {
            gbuf_printf(gbuf, ", %s", *alias);
            alias++;
        }
        gbuf_printf(gbuf, ")", pcmds->name);
    } else {
        gbuf_printf(gbuf, "- %-28s", pcmds->name);
    }
    BOOL add_point = FALSE;
    const sdata_desc_t *pparam = pcmds->schema;
    while(pparam && pparam->name) {
        if((pparam->flag & SDF_REQUIRED) && !(pparam->flag & SDF_PERSIST)) { // TODO PERSITS? why?
            gbuf_printf(gbuf, " <%s>", pparam->name);
        } else {
            gbuf_printf(gbuf, " [%s='%s']", pparam->name, pparam->default_value?pparam->default_value:"?");
        }
        add_point = TRUE;
        pparam++;
    }
    if(add_point) {
        gbuf_printf(gbuf, ". %s\n", (pcmds->description)?pcmds->description:"");
    } else {
        gbuf_printf(gbuf, " %s\n", (pcmds->description)?pcmds->description:"");
    }

    if(extended) {
        gbuf_printf(gbuf, "\n");
        pparam = pcmds->schema;
        while(pparam && pparam->name) {
            GBUFFER *gbuf_flag = get_sdata_flag_desc(pparam->flag);
            char *p = gbuf_cur_rd_pointer(gbuf_flag);
            gbuf_printf(gbuf, "    - %-16s Type:%-8s, Desc:%-35s, Flag:%s\n",
                pparam->name,
                sdata_authz_type(pparam->type),
                (pparam->description)?pparam->description:"",
                p?p:""
            );
            gbuf_decref(gbuf_flag);
            pparam++;
        }
    }
}

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
    if(!empty_string(auth)) {
        const sdata_desc_t *cnf_auth;
        if(gobj_gclass(gobj)->authz_table) {
            cnf_auth = authz_get_level_desc(gobj_gclass(gobj)->authz_table, auth);
            if(cnf_auth) {
                GBUFFER *gbuf = gbuf_create(256, 16*1024, 0, 0);
                gbuf_printf(gbuf, "%s\n", auth);
                int len = strlen(auth);
                while(len > 0) {
                    gbuf_printf(gbuf, "%c", '=');
                    len--;
                }
                gbuf_printf(gbuf, "\n");
                if(!empty_string(cnf_auth->description)) {
                    gbuf_printf(gbuf, "%s\n", cnf_auth->description);
                }
                add_authz_help(gbuf, cnf_auth, TRUE);
                gbuf_printf(gbuf, "\n");
                json_t *jn_resp = json_string(gbuf_cur_rd_pointer(gbuf));
                gbuf_decref(gbuf);
                KW_DECREF(kw);
                return jn_resp;
            }
        }

        KW_DECREF(kw);
        return json_local_sprintf(
            "%s: authz '%s' not available.\n",
            gobj_short_name(gobj),
            auth
        );
    }

    GBUFFER *gbuf = gbuf_create(256, 64*1024, 0, 0);
    gbuf_printf(gbuf, "Available permissions\n");
    gbuf_printf(gbuf, "=====================\n");

    /*
     *  GObj authzs
     */
    if(gobj_gclass(gobj)->authz_table) {
        gbuf_printf(gbuf, "\n> %s\n", gobj_short_name(gobj));
        const sdata_desc_t *pauths = gobj_gclass(gobj)->authz_table;
        while(pauths->name) {
            if(!empty_string(pauths->name)) {
                add_authz_help(gbuf, pauths, FALSE);
            } else {
                /*
                *  Empty authz (not null) is for print a blank line or a title is desc is not empty
                */
                if(!empty_string(pauths->description)) {
                    gbuf_printf(gbuf, "%s\n", pauths->description);
                } else {
                    gbuf_printf(gbuf, "\n");
                }
            }
            pauths++;
        }
    }

    json_t *jn_resp = json_string(gbuf_cur_rd_pointer(gbuf));
    gbuf_decref(gbuf);
    KW_DECREF(kw);
    return jn_resp;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC const sdata_desc_t *authz_get_level_desc(
    const sdata_desc_t *authz_table,
    const char *level
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
                if(strcasecmp(*alias, level)==0) {
                    return pcmd;
                }
                alias++;
            }
        }
        if(strcasecmp(pcmd->name, level)==0) {
            return pcmd;
        }
        if(!alias_checked) {
            const char **alias = pcmd->alias;
            while(alias && *alias) {
                if(strcasecmp(*alias, level)==0) {
                    return pcmd;
                }
                alias++;
            }
        }

        pcmd++;
    }
    return 0;
}

