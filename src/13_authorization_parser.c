/***********************************************************************
 *          AUTHORIZATION_PARSER.C
 *
 *          Authorization parser
 *
 *          Copyright (c) 2017 Niyamaka.
 *          All Rights Reserved.
***********************************************************************/
#include "13_authorization_parser.h"

/***************************************************************
 *              Constants
 ***************************************************************/

/***************************************************************
 *              Structures
 ***************************************************************/

/***************************************************************
 *              Prototypes
 ***************************************************************/
PRIVATE BOOL authorization_in_gobj(
    hgobj gobj,
    const char *authorization
);
PRIVATE json_t * expand_authorization(
    hgobj gobj,
    const char *authorization,
    json_t *kw,     // NOT owned
    const sdata_desc_t **authz_desc
);
PRIVATE json_t *build_authz_kw(
    hgobj gobj,
    const char *authorization,
    const sdata_desc_t *cnf_authz,
    char *parameters,
    json_t *kw, // not owned
    int *result
);

/***************************************************************
 *              Data
 ***************************************************************/

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t * authorization_parser(hgobj gobj,
    const char *authorization,
    json_t *kw,
    hgobj src
)
{
    const sdata_desc_t *cnf_authz = 0;
    if(!authorization_in_gobj(gobj, authorization)) {
        return msg_iev_build_webix(
            gobj,
            -15,
            json_local_sprintf(
                "%s: authorization '%s' not available. Try 'help' authorization.\n",
                gobj_short_name(gobj),
                authorization
            ),
            0,
            0,
            kw
        );
    }

    json_t *kw_authz = expand_authorization(gobj, authorization, kw, &cnf_authz);
    if(gobj_trace_level(gobj) & (TRACE_EV_KW)) {
        log_debug_json(0, kw_authz, "expanded_authorization: kw_authz");
    }
    if(!cnf_authz) {
        return msg_iev_build_webix(
            gobj,
            -14,
            kw_authz,
            0,
            0,
            kw
        );
    }
    json_t *webix = 0;
    if(cnf_authz->json_fn) {
        webix = (cnf_authz->json_fn)(gobj, cnf_authz->name, kw_authz, src);
    } else {
        /*
         *  Redirect authorization to event
         */
        const char *event;
        if(*cnf_authz->alias)
            event = *cnf_authz->alias;
        else
            event = cnf_authz->name;
        gobj_send_event(gobj, event, kw_authz, src);
        KW_DECREF(kw);
        return 0;   /* asynchronous response */
    }
    KW_DECREF(kw);
    return webix;  /* can be null if asynchronous response */
}

/***************************************************************************
 *  Find an input parameter
 ***************************************************************************/
PUBLIC const sdata_desc_t *authorization_get_authz_desc(const sdata_desc_t *authz_table, const char *authz)
{
    const sdata_desc_t *pauthz = authz_table;
    while(pauthz->name) {
        /*
         *  Alias have precedence if there is no json_fn authorization function.
         *  It's the combination to redirect the authorization as `name` event,
         */
        BOOL alias_checked = FALSE;
        if(!pauthz->json_fn && pauthz->alias) {
            alias_checked = TRUE;
            const char **alias = pauthz->alias;
            while(alias && *alias) {
                if(strcasecmp(*alias, authz)==0) {
                    return pauthz;
                }
                alias++;
            }
        }
        if(strcasecmp(pauthz->name, authz)==0) {
            return pauthz;
        }
        if(!alias_checked) {
            const char **alias = pauthz->alias;
            while(alias && *alias) {
                if(strcasecmp(*alias, authz)==0) {
                    return pauthz;
                }
                alias++;
            }
        }

        pauthz++;
    }
    return 0;
}

/***************************************************************************
 *  Is a authorization in the gobj?
 ***************************************************************************/
PRIVATE BOOL authorization_in_gobj(
    hgobj gobj,
    const char *authorization
)
{
    const sdata_desc_t *authz_table = gobj_gclass(gobj)->authz_table;

    char *str, *p;
    str = p = gbmem_strdup(authorization);
    char *authz = get_parameter(p, &p);  // dejalo como en expand
    if(empty_string(authz)) {
        gbmem_free(str);
        return FALSE;
    }
    const sdata_desc_t *cnf_authz = authorization_get_authz_desc(authz_table, authz);
    gbmem_free(str);
    return cnf_authz?TRUE:FALSE;
}

/***************************************************************************
 *  Return a new kw for authorization, poping the parameters inside of `authorization`
 *  If authz_desc is 0 then there is a error
 *  and the return json is a json string message with the error.
 ***************************************************************************/
PRIVATE json_t *expand_authorization(
    hgobj gobj,
    const char *authorization,
    json_t *kw,     // NOT owned
    const sdata_desc_t **authz_desc
)
{
    const sdata_desc_t *authz_table = gobj_gclass(gobj)->authz_table;

    if(authz_desc) {
        *authz_desc = 0; // It's error
    }

    char *str, *p;
    str = p = gbmem_strdup(authorization);
    char *authz = get_parameter(p, &p);
    if(empty_string(authz)) {
        gbmem_free(str);
        return json_local_sprintf("No authorization");
    }
    const sdata_desc_t *cnf_authz = authorization_get_authz_desc(authz_table, authz);
    if(!cnf_authz) {
        gbmem_free(str);
        return json_local_sprintf("No '%s' authorization found in '%s'", authz, gobj_short_name(gobj));
    }
    if(authz_desc) {
        *authz_desc = cnf_authz;
    }

    int ok = 0;
    json_t *kw_authz = build_authz_kw(gobj, cnf_authz->name, cnf_authz, p, kw, &ok);
    gbmem_free(str);
    if(ok < 0) {
        if(authz_desc) {
            *authz_desc = 0;
        }
        return kw_authz;
    }
    return kw_authz;
}


/***************************************************************************
 *  Parameters of authorization are described as sdata_desc_t
 ***************************************************************************/
PRIVATE json_t *parameter2json(hgobj gobj, int type, const char *name, const char *s, int *result)
{
    if(ASN_IS_STRING(type)) {
        if(!s) {
            s = "";
        }
        json_t *jn_param = json_string(s);
        if(!jn_param) {
            *result = -1;
            json_t *jn_data = json_local_sprintf(
                "%s: STRING type of parameter '%s' has failed",
                gobj_short_name(gobj),
                name
            );
            return jn_data;
        }
        return jn_param;
    } else if(ASN_IS_BOOLEAN(type)) {
        BOOL value;
        if(strcasecmp(s, "true")==0) {
            value = 1;
        } else if(strcasecmp(s, "false")==0) {
            value = 0;
        } else {
            value = atoi(s);
        }
        if(value) {
            return json_true();
        } else {
            return json_false();
        }
    } else if(ASN_IS_NATURAL_NUMBER(type)) {
        json_t *jn_param = json_integer(atoll(s));
        if(!jn_param) {
            *result = -1;
            json_t *jn_data = json_local_sprintf(
                "%s: NATURAL type of parameter '%s' has failed",
                gobj_short_name(gobj),
                name
            );
            return jn_data;
        }
        return jn_param;
    } else if(ASN_IS_REAL_NUMBER(type)) {
        json_t *jn_param = json_real(atof(s));
        if(!jn_param) {
            *result = -1;
            json_t *jn_data = json_local_sprintf(
                "%s: REAL typeof parameter '%s' has failed",
                gobj_short_name(gobj),
                name
            );
            return jn_data;
        }
        return jn_param;
    } else if(ASN_IS_JSON(type)) {
        json_t *jn_param = nonlegalstring2json(s, TRUE);
        if(!jn_param) {
            *result = -1;
            json_t *jn_data = json_local_sprintf(
                "%s: JSON typeof parameter '%s' has failed",
                gobj_short_name(gobj),
                name
            );
            return jn_data;
        }
        return jn_param;
    } else {
        *result = -1;
        json_t *jn_data = json_local_sprintf(
            "s: type %d of parameter '%s' is unknown",
            gobj_short_name(gobj),
            type,
            name
        );
        return jn_data;
    }
}

/***************************************************************************
 *  Find an input parameter
 ***************************************************************************/
PRIVATE const sdata_desc_t *find_ip_parameter(const sdata_desc_t *input_parameters, const char *key)
{
    const sdata_desc_t *ip = input_parameters;
    while(ip->name) {
        if(strcasecmp(ip->name, key)==0) {
            return ip;
        }
        /* check alias */
        const char **alias = ip->alias;
        while(alias && *alias) {
            if(strcasecmp(*alias, key)==0) {
                return ip;
            }
            alias++;
        }

        ip++;
    }
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PRIVATE const char *sdata_authorization_type(uint8_t type)
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
PRIVATE void add_authorization_help(GBUFFER *gbuf, const sdata_desc_t *pauthzs, BOOL extended)
{
    if(pauthzs->alias) {
        gbuf_printf(gbuf, "- %-28s (", pauthzs->name);
        const char **alias = pauthzs->alias;
        if(*alias) {
            gbuf_printf(gbuf, "%s ", *alias);
        }
        alias++;
        while(*alias) {
            gbuf_printf(gbuf, ", %s", *alias);
            alias++;
        }
        gbuf_printf(gbuf, ")", pauthzs->name);
    } else {
        gbuf_printf(gbuf, "- %-28s", pauthzs->name);
    }
    BOOL add_point = FALSE;
    const sdata_desc_t *pparam = pauthzs->schema;
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
        gbuf_printf(gbuf, ". %s\n", (pauthzs->description)?pauthzs->description:"");
    } else {
        gbuf_printf(gbuf, " %s\n", (pauthzs->description)?pauthzs->description:"");
    }

    if(extended) {
        gbuf_printf(gbuf, "\n");
        pparam = pauthzs->schema;
        while(pparam && pparam->name) {
            GBUFFER *gbuf_flag = get_sdata_flag_desc(pparam->flag);
            char *p = gbuf_cur_rd_pointer(gbuf_flag);
            gbuf_printf(gbuf, "    - %-16s Type:%-8s, Desc:%-35s, Flag:%s\n",
                pparam->name,
                sdata_authorization_type(pparam->type),
                (pparam->description)?pparam->description:"",
                p?p:""
            );
            gbuf_decref(gbuf_flag);
            pparam++;
        }
    }
}

/***************************************************************************
 *  string parameters to json dict
 *  If error (result < 0) return a json string message
 ***************************************************************************/
PRIVATE json_t *build_authz_kw(
    hgobj gobj,
    const char *authorization,
    const sdata_desc_t *cnf_authz,
    char *parameters,   // input line
    json_t *kw, // not owned
    int *result)
{
    const sdata_desc_t *input_parameters = cnf_authz->schema;
    BOOL wild_authorization = (cnf_authz->flag & SDF_WILD_CMD)?1:0;
    json_t *kw_authz = json_object();
    char *pxxx = parameters;
    char bftemp[1] = {0};
    if(!pxxx) {
        pxxx = bftemp;
    }

    if(!input_parameters) {
        return kw_authz;
    }
    /*
     *  Check required paramters of pure authorization.
     *  Else, it's a redirect2event, let action check parameters.
     */
    /*
     *  Firstly get required parameters
     */
    const sdata_desc_t *ip = input_parameters;
    while(ip->name) {
        if(ip->flag & SDF_NOTACCESS) {
            ip++;
            continue;
        }
        if(!(ip->flag & SDF_REQUIRED)) {
            break;
        }

        char *param = get_parameter(pxxx, &pxxx);
        if(!param) {
            // Si no está en pxxx buscalo en kw
            json_t *jn_param = kw_get_dict_value(kw, ip->name, 0, 0);
            if(jn_param) {
                json_object_set(kw_authz, ip->name, jn_param);
                ip++;
                continue;
            } else {
                *result = -1;
                JSON_DECREF(kw_authz);
                return json_local_sprintf(
                    "%s: authorization '%s', parameter '%s' is required",
                    gobj_short_name(gobj),
                    authorization,
                    ip->name
                );
            }
        }
        if(strchr(param, '=')) {
            // es ya un key=value, falta el required
            *result = -1;
            JSON_DECREF(kw_authz);
            return json_local_sprintf(
                "%s: required parameter '%s' not found",
                gobj_short_name(gobj),
                ip->name
            );
        }
        json_t *jn_param = parameter2json(gobj, ip->type, ip->name, param, result);
        if(*result < 0) {
            JSON_DECREF(kw_authz);
            return jn_param;
        }
        if(!jn_param) {
            *result = -1;
            JSON_DECREF(kw_authz);
            return json_local_sprintf(
                "%s: internal error, authorization '%s', parameter '%s'",
                gobj_short_name(gobj),
                authorization,
                ip->name
            );
        }
        json_object_set_new(kw_authz, ip->name, jn_param);

        ip++;
    }

    /*
     *  Next: get value from kw or default values
     */
    while(ip->name) {
        if(ip->flag & SDF_NOTACCESS) {
            ip++;
            continue;
        }

        json_t *jn_param = kw_get_dict_value(kw, ip->name, 0, 0);
        if(jn_param) {
            const char *param = json_string_value(jn_param);
            jn_param = parameter2json(gobj, ip->type, ip->name, param, result);
            if(*result < 0) {
                JSON_DECREF(kw_authz);
                return jn_param;
            }
            json_object_set_new(kw_authz, ip->name, jn_param);
            ip++;
            continue;
        }

        if(ip->default_value) {
            char *param = (char *)ip->default_value;
            jn_param = parameter2json(gobj, ip->type, ip->name, param, result);
            if(*result < 0) {
                JSON_DECREF(kw_authz);
                return jn_param;
            }
            json_object_set_new(kw_authz, ip->name, jn_param);

            ip++;
            continue;
        }

        ip++;
    }

    /*
     *  Get key=value parameters from input line
     */
    char *key;
    char *value;
    while((value=get_key_value_parameter(pxxx, &key, &pxxx))) {
        if(!key) {
            // No parameter then stop
            break;
        }
        if(!value) {
            // Non-required parameter must be key=value format
            *result = -1;
            JSON_DECREF(kw_authz);
            return json_local_sprintf(
                "%s: authorization '%s', optional parameters must be with key=value format ('%s=?')",
                gobj_short_name(gobj),
                authorization,
                key
            );
        }
        const sdata_desc_t *ip = find_ip_parameter(input_parameters, key);
        json_t *jn_param = 0;
        if(ip) {
            jn_param = parameter2json(gobj, ip->type, ip->name, value, result);
        } else {
            if(wild_authorization) {
                jn_param = parameter2json(gobj, ASN_OCTET_STR, "wild-option", value, result);
            } else {
                *result = -1;
                JSON_DECREF(kw_authz);
                return json_local_sprintf(
                    "%s: '%s' authorization has no option '%s'",
                    gobj_short_name(gobj),
                    authorization,
                    key?key:"?"
                );
            }
        }
        if(*result < 0) {
            JSON_DECREF(kw_authz);
            return jn_param;
        }
        if(!jn_param) {
            *result = -1;
            JSON_DECREF(kw_authz);
            jn_param = json_local_sprintf(
                "%s: internal error, authorization '%s', parameter '%s', value '%s'",
                gobj_short_name(gobj),
                authorization,
                key,
                value
            );
            return jn_param;
        }
        json_object_set_new(kw_authz, key, jn_param);
    }

    if(!empty_string(pxxx)) {
        *result = -1;
        JSON_DECREF(kw_authz);
        return json_local_sprintf(
            "%s: authorization '%s' with extra parameters: '%s'",
            gobj_short_name(gobj),
            authorization,
            pxxx
        );
    }

    json_object_update_missing(kw_authz, kw); // HACK lo quité y dejó de funcionar el GUI

    return kw_authz;
}

/***************************************************************************
 *  Return a webix json
 ***************************************************************************/
PUBLIC json_t *gobj_build_authzs_doc(hgobj gobj, json_t *kw)
{
    int level = kw_get_int(kw, "level", 0, KW_WILD_NUMBER);
    const char *authz = kw_get_str(kw, "authz", 0, 0);
    if(!empty_string(authz)) {
        const sdata_desc_t *cnf_authz;
        if(gobj_gclass(gobj)->authz_table) {
            cnf_authz = authorization_get_authz_desc(gobj_gclass(gobj)->authz_table, authz);
            if(cnf_authz) {
                GBUFFER *gbuf = gbuf_create(256, 16*1024, 0, 0);
                gbuf_printf(gbuf, "%s\n", authz);
                int len = strlen(authz);
                while(len > 0) {
                    gbuf_printf(gbuf, "%c", '=');
                    len--;
                }
                gbuf_printf(gbuf, "\n");
                if(!empty_string(cnf_authz->description)) {
                    gbuf_printf(gbuf, "%s\n", cnf_authz->description);
                }
                add_authorization_help(gbuf, cnf_authz, TRUE);
                gbuf_printf(gbuf, "\n");
                json_t *jn_resp = json_string(gbuf_cur_rd_pointer(gbuf));
                gbuf_decref(gbuf);
                KW_DECREF(kw);
                return jn_resp;
            }
        }

        /*
         *  Search in Child authorizations
         */
        if(level) {
            hgobj child_;
            rc_instance_t *i_child = gobj_first_child(gobj, &child_);
            while(i_child) {
                hgobj child = child_;
                if(gobj_gclass(child)->authz_table) {
                    cnf_authz = authorization_get_authz_desc(gobj_gclass(child)->authz_table, authz);
                    if(cnf_authz) {
                        GBUFFER *gbuf = gbuf_create(256, 16*1024, 0, 0);
                        gbuf_printf(gbuf, "%s\n", authz);
                        int len = strlen(authz);
                        while(len > 0) {
                            gbuf_printf(gbuf, "%c", '=');
                            len--;
                        }
                        gbuf_printf(gbuf, "\n");
                        if(!empty_string(cnf_authz->description)) {
                            gbuf_printf(gbuf, "%s\n", cnf_authz->description);
                        }
                        add_authorization_help(gbuf, cnf_authz, TRUE);
                        gbuf_printf(gbuf, "\n");
                        json_t *jn_resp = json_string(gbuf_cur_rd_pointer(gbuf));
                        gbuf_decref(gbuf);
                        KW_DECREF(kw);
                        return jn_resp;
                    }
                }
                i_child = gobj_next_child(i_child, &child_);
            }
        }

        KW_DECREF(kw);
        return json_local_sprintf(
            "%s: authorization '%s' not available.\n",
            gobj_short_name(gobj),
            authz
        );
    }

    GBUFFER *gbuf = gbuf_create(256, 64*1024, 0, 0);
    gbuf_printf(gbuf, "Available authorizations\n");
    gbuf_printf(gbuf, "========================\n");

    /*
     *  GObj authorizations
     */
    if(gobj_gclass(gobj)->authz_table) {
        gbuf_printf(gbuf, "\n> %s\n", gobj_short_name(gobj));
        const sdata_desc_t *pauthzs = gobj_gclass(gobj)->authz_table;
        while(pauthzs->name) {
            if(!empty_string(pauthzs->name)) {
                add_authorization_help(gbuf, pauthzs, FALSE);
            } else {
                /*
                *  Empty authorization (not null) is for print a blank line or a title is desc is not empty
                */
                if(!empty_string(pauthzs->description)) {
                    gbuf_printf(gbuf, "%s\n", pauthzs->description);
                } else {
                    gbuf_printf(gbuf, "\n");
                }
            }
            pauthzs++;
        }
    }

    /*
     *  Child authorizations
     */
    if(level) {
        hgobj child_;
        rc_instance_t *i_child = gobj_first_child(gobj, &child_);
        while(i_child) {
            hgobj child = child_;
            if(gobj_gclass(child)->authz_table) {
                gbuf_printf(gbuf, "\n> %s\n", gobj_short_name(child));
                const sdata_desc_t *pauthzs = gobj_gclass(child)->authz_table;
                while(pauthzs->name) {
                    if(!empty_string(pauthzs->name)) {
                        add_authorization_help(gbuf, pauthzs, FALSE);
                    } else {
                        /*
                        *  Empty authorization (not null) is for print a blank line or a title is desc is not empty
                        */
                        if(!empty_string(pauthzs->description)) {
                            gbuf_printf(gbuf, "%s\n", pauthzs->description);
                        } else {
                            gbuf_printf(gbuf, "\n");
                        }
                    }
                    pauthzs++;
                }
            }
            i_child = gobj_next_child(i_child, &child_);
        }
    }

    json_t *jn_resp = json_string(gbuf_cur_rd_pointer(gbuf));
    gbuf_decref(gbuf);
    KW_DECREF(kw);
    return jn_resp;
}

