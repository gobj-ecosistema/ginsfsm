/***********************************************************************
 *          COMMAND_PARSER.C
 *
 *          Command parser
 *
 *          Copyright (c) 2017 Niyamaka.
 *          All Rights Reserved.
***********************************************************************/
#include "13_command_parser.h"

/***************************************************************
 *              Constants
 ***************************************************************/

/***************************************************************
 *              Structures
 ***************************************************************/

/***************************************************************
 *              Prototypes
 ***************************************************************/
PRIVATE BOOL command_in_gobj(
    hgobj gobj,
    const char *command
);

/***************************************************************
 *              Data
 ***************************************************************/

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC json_t * command_parser(hgobj gobj,
    const char *command,
    json_t *kw,
    hgobj src
)
{
    const sdata_desc_t *cnf_cmd = 0;
    if(!command_in_gobj(gobj, command)) {
        return msg_iev_build_webix(
            gobj,
            -15,
            json_sprintf(
                "%s: command '%s' not available. Try 'help' command.",
                gobj_short_name(gobj),
                command
            ),
            0,
            0,
            kw
        );
    }

    const sdata_desc_t *command_table = gobj_gclass(gobj)->command_table;
    json_t *kw_cmd = expand_command(gobj_short_name(gobj), command_table, command, kw, &cnf_cmd);
    if(gobj_trace_level(gobj) & (TRACE_EV_KW)) {
        log_debug_json(0, kw_cmd, "expanded_command: kw_cmd");
    }
    if(!cnf_cmd) {
        return msg_iev_build_webix(
            gobj,
            -14,
            kw_cmd,
            0,
            0,
            kw
        );
    }

    /*-----------------------------------------------*
     *  Check AUTHZ
     *-----------------------------------------------*/
//     if(cnf_cmd->flag & SDF_AUTHZ_X) {
//         /*
//          *  AUTHZ Required
//          */
//         json_t *kw_authz = json_pack("{s:s}",
//             "command", command
//         );
//         if(kw) {
//             json_object_set(kw_authz, "kw", kw);
//         } else {
//             json_object_set_new(kw_authz, "kw", json_object());
//         }
//         if(!gobj_user_has_authz(
//                 gobj,
//                 "__execute_command__",
//                 kw_authz,
//                 src
//           )) {
//             log_error(0,
//                 "gobj",         "%s", gobj_full_name(gobj),
//                 "function",     "%s", __FUNCTION__,
//                 "msgset",       "%s", MSGSET_OAUTH_ERROR,
//                 "msg",          "%s", "No permission to execute command",
//                 //"user",         "%s", gobj_get_user(src),
//                 "gclass",       "%s", gobj_gclass_name(gobj),
//                 "command",      "%s", command?command:"",
//                 NULL
//             );
//             return msg_iev_build_webix(
//                 gobj,
//                 -403,
//                 json_sprintf(
//                     "No permission to execute command"
//                 ),
//                 0,
//                 0,
//                 kw
//             );
//         }
//     }

    json_t *webix = 0;
    if(cnf_cmd->json_fn) {
        webix = (cnf_cmd->json_fn)(gobj, cnf_cmd->name, kw_cmd, src);
    } else {
        /*
         *  Redirect command to event
         */
        const char *event;
        if(*cnf_cmd->alias)
            event = *cnf_cmd->alias;
        else
            event = cnf_cmd->name;
        gobj_send_event(gobj, event, kw_cmd, src);
        KW_DECREF(kw);
        return 0;   /* asynchronous response */
    }
    KW_DECREF(kw);
    return webix;  /* can be null if asynchronous response */
}

/***************************************************************************
 *  Find the command descriptor
 ***************************************************************************/
PUBLIC const sdata_desc_t *command_get_cmd_desc(const sdata_desc_t *command_table, const char *cmd)
{
    const sdata_desc_t *pcmd = command_table;
    while(pcmd->name) {
        /*
         *  Alias have precedence if there is no json_fn command function.
         *  It's the combination to redirect the command as `name` event,
         */
        BOOL alias_checked = FALSE;
        if(!pcmd->json_fn && pcmd->alias) {
            alias_checked = TRUE;
            const char **alias = pcmd->alias;
            while(alias && *alias) {
                if(strcasecmp(*alias, cmd)==0) {
                    return pcmd;
                }
                alias++;
            }
        }
        if(strcasecmp(pcmd->name, cmd)==0) {
            return pcmd;
        }
        if(!alias_checked) {
            const char **alias = pcmd->alias;
            while(alias && *alias) {
                if(strcasecmp(*alias, cmd)==0) {
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
 *  Is a command in the gobj?
 ***************************************************************************/
PRIVATE BOOL command_in_gobj(
    hgobj gobj,
    const char *command
)
{
    const sdata_desc_t *command_table = gobj_gclass(gobj)->command_table;

    char *str, *p;
    str = p = gbmem_strdup(command);
    char *cmd = get_parameter(p, &p);  // dejalo como en expand
    if(empty_string(cmd)) {
        gbmem_free(str);
        return FALSE;
    }
    const sdata_desc_t *cnf_cmd = command_get_cmd_desc(command_table, cmd);
    gbmem_free(str);
    return cnf_cmd?TRUE:FALSE;
}

/***************************************************************************
 *  Return a new kw for command, poping the parameters inside of `command`
 *  If cmd_desc is 0 then there is a error
 *  and the return json is a json string message with the error.
 ***************************************************************************/
PUBLIC json_t *expand_command(
    const char *gobj_name,
    const sdata_desc_t *command_table,
    const char *command,
    json_t *kw,     // NOT owned
    const sdata_desc_t **cmd_desc
)
{
    if(cmd_desc) {
        *cmd_desc = 0; // It's error
    }

    char *str, *p;
    str = p = gbmem_strdup(command);
    char *cmd = get_parameter(p, &p);
    if(empty_string(cmd)) {
        gbmem_free(str);
        return json_sprintf("No command");
    }
    const sdata_desc_t *cnf_cmd = command_get_cmd_desc(command_table, cmd);
    if(!cnf_cmd) {
        gbmem_free(str);
        return json_sprintf("No command found: '%s'", cmd);
    }
    if(cmd_desc) {
        *cmd_desc = cnf_cmd;
    }

    int ok = 0;
    json_t *kw_cmd = build_cmd_kw(gobj_name, cnf_cmd->name, cnf_cmd, p, kw, &ok);
    gbmem_free(str);
    if(ok < 0) {
        if(cmd_desc) {
            *cmd_desc = 0;
        }
        return kw_cmd;
    }
    return kw_cmd;
}


/***************************************************************************
 *  Parameters of command are described as sdata_desc_t
 ***************************************************************************/
PRIVATE json_t *parameter2json(
    const char *gobj_name,
    int type,
    const char *name,
    const char *s,
    int *result
)
{
    *result = 0;

    if(ASN_IS_STRING(type)) {
        if(!s) {
            s = "";
        }
        return json_string(s);

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
        return json_integer(atoll(s));

    } else if(ASN_IS_REAL_NUMBER(type)) {
        return json_real(atof(s));

    } else if(ASN_IS_JSON(type)) {
        return nonlegalstring2json(s, TRUE);
    } else {
        *result = -1;
        json_t *jn_data = json_sprintf(
            "%s: type %d of parameter '%s' is unknown",
            gobj_name,
            (int)type,
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
PRIVATE const char *sdata_command_type(uint8_t type)
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
PRIVATE void add_command_help(GBUFFER *gbuf, const sdata_desc_t *pcmds, BOOL extended)
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
        gbuf_printf(gbuf, ")");
    } else {
        gbuf_printf(gbuf, "- %-28s", pcmds->name);
    }
    BOOL add_point = FALSE;
    const sdata_desc_t *pparam = pcmds->schema;
    while(pparam && pparam->name) {
        if((pparam->flag & SDF_REQUIRED) && !(pparam->flag & SDF_PERSIST)) { // TODO PERSITS? why?
            gbuf_printf(gbuf, " <%s>", pparam->name);
        } else {
            gbuf_printf(gbuf,
                " [%s='%s']",
                pparam->name, pparam->default_value?(char *)pparam->default_value:"?"
            );
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
                sdata_command_type(pparam->type),
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
PUBLIC json_t *build_cmd_kw(
    const char *gobj_name,
    const char *command,
    const sdata_desc_t *cnf_cmd,
    char *parameters,   // input line
    json_t *kw, // not owned
    int *result
)
{
    const sdata_desc_t *input_parameters = cnf_cmd->schema;
    BOOL wild_command = (cnf_cmd->flag & SDF_WILD_CMD)?1:0;
    json_t *kw_cmd = json_object();
    char *pxxx = parameters;
    char bftemp[1] = {0};
    if(!pxxx) {
        pxxx = bftemp;
    }

    *result = 0;

    if(!input_parameters) {
        json_object_update_missing(kw_cmd, kw);
        return kw_cmd;
    }
    /*
     *  Check required paramters of pure command.
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
            /*
             *  Required: si no está en pxxx buscalo en kw
             */
            json_t *jn_param = kw_get_dict_value(kw, ip->name, 0, 0);
            if(jn_param) {
                json_object_set(kw_cmd, ip->name, jn_param);
                ip++;
                continue;
            } else {
                *result = -1;
                JSON_DECREF(kw_cmd);
                return json_sprintf(
                    "%s: command '%s', parameter '%s' is required",
                    gobj_name,
                    command,
                    ip->name
                );
            }
        }
        if(strchr(param, '=')) {
            // es ya un key=value, falta el required
            *result = -1;
            JSON_DECREF(kw_cmd);
            return json_sprintf(
                "%s: required parameter '%s' not found",
                gobj_name,
                ip->name
            );
        }
        json_t *jn_param = parameter2json(gobj_name, ip->type, ip->name, param, result);
        if(*result < 0) {
            JSON_DECREF(kw_cmd);
            return jn_param;
        }
        if(!jn_param) {
            *result = -1;
            JSON_DECREF(kw_cmd);
            return json_sprintf(
                "%s: internal error, command '%s', parameter '%s'",
                gobj_name,
                command,
                ip->name
            );
        }
        json_object_set_new(kw_cmd, ip->name, jn_param);

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

        if(kw_has_key(kw, ip->name)) {
            json_t *value = kw_get_dict_value(kw, ip->name, 0, 0);
            if(!ASN_IS_STRING(ip->type) && json_is_string(value)) {
                const char *s = json_string_value(value);
                json_t *new_value = nonlegalstring2json(s, TRUE);
                json_object_set_new(kw_cmd, ip->name, new_value);
            } else {
                json_object_set(kw_cmd, ip->name, value);
            }
            ip++;
            continue;
        }

        if(ip->default_value) {
            json_t *jn_param = parameter2json(
                gobj_name,
                ip->type,
                ip->name,
                (char *)ip->default_value,
                result
            );
            if(*result < 0) {
                JSON_DECREF(kw_cmd);
                return jn_param;
            }
            json_object_set_new(kw_cmd, ip->name, jn_param);

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
            JSON_DECREF(kw_cmd);
            return json_sprintf(
                "%s: command '%s', optional parameters must be with key=value format ('%s=?')",
                gobj_name,
                command,
                key
            );
        }
        const sdata_desc_t *ip = find_ip_parameter(input_parameters, key);
        json_t *jn_param = 0;
        if(ip) {
            jn_param = parameter2json(gobj_name, ip->type, ip->name, value, result);
        } else {
            if(wild_command) {
                jn_param = parameter2json(gobj_name, ASN_OCTET_STR, "wild-option", value, result);
            } else {
                *result = -1;
                JSON_DECREF(kw_cmd);
                return json_sprintf(
                    "%s: '%s' command has no option '%s'",
                    gobj_name,
                    command,
                    key?key:"?"
                );
            }
        }
        if(*result < 0) {
            JSON_DECREF(kw_cmd);
            return jn_param;
        }
        if(!jn_param) {
            *result = -1;
            JSON_DECREF(kw_cmd);
            jn_param = json_sprintf(
                "%s: internal error, command '%s', parameter '%s', value '%s'",
                gobj_name,
                command,
                key,
                value
            );
            return jn_param;
        }
        json_object_set_new(kw_cmd, key, jn_param);
    }

    if(!empty_string(pxxx)) {
        *result = -1;
        JSON_DECREF(kw_cmd);
        return json_sprintf(
            "%s: command '%s' with extra parameters: '%s'",
            gobj_name,
            command,
            pxxx
        );
    }

    json_object_update_missing(kw_cmd, kw); // HACK lo quité y dejó de funcionar el GUI

    return kw_cmd;
}

/***************************************************************************
 *  Return a string json
 ***************************************************************************/
PUBLIC json_t *gobj_build_cmds_doc(hgobj gobj, json_t *kw)
{
    int level = kw_get_int(kw, "level", 0, KW_WILD_NUMBER);
    const char *cmd = kw_get_str(kw, "cmd", 0, 0);
    if(!empty_string(cmd)) {
        const sdata_desc_t *cnf_cmd;
        if(gobj_gclass(gobj)->command_table) {
            cnf_cmd = command_get_cmd_desc(gobj_gclass(gobj)->command_table, cmd);
            if(cnf_cmd) {
                GBUFFER *gbuf = gbuf_create(256, 16*1024, 0, 0);
                gbuf_printf(gbuf, "%s\n", cmd);
                int len = strlen(cmd);
                while(len > 0) {
                    gbuf_printf(gbuf, "%c", '=');
                    len--;
                }
                gbuf_printf(gbuf, "\n");
                if(!empty_string(cnf_cmd->description)) {
                    gbuf_printf(gbuf, "%s\n", cnf_cmd->description);
                }
                add_command_help(gbuf, cnf_cmd, TRUE);
                gbuf_printf(gbuf, "\n");
                json_t *jn_resp = json_string(gbuf_cur_rd_pointer(gbuf));
                gbuf_decref(gbuf);
                KW_DECREF(kw);
                return jn_resp;
            }
        }

        /*
         *  Search in Child commands
         */
        if(level) {
            hgobj child_;
            rc_instance_t *i_child = gobj_first_child(gobj, &child_);
            while(i_child) {
                hgobj child = child_;
                if(gobj_gclass(child)->command_table) {
                    cnf_cmd = command_get_cmd_desc(gobj_gclass(child)->command_table, cmd);
                    if(cnf_cmd) {
                        GBUFFER *gbuf = gbuf_create(256, 16*1024, 0, 0);
                        gbuf_printf(gbuf, "%s\n", cmd);
                        int len = strlen(cmd);
                        while(len > 0) {
                            gbuf_printf(gbuf, "%c", '=');
                            len--;
                        }
                        gbuf_printf(gbuf, "\n");
                        if(!empty_string(cnf_cmd->description)) {
                            gbuf_printf(gbuf, "%s\n", cnf_cmd->description);
                        }
                        add_command_help(gbuf, cnf_cmd, TRUE);
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
        return json_sprintf(
            "%s: command '%s' not available.\n",
            gobj_short_name(gobj),
            cmd
        );
    }

    GBUFFER *gbuf = gbuf_create(256, 64*1024, 0, 0);
    gbuf_printf(gbuf, "Available commands\n");
    gbuf_printf(gbuf, "==================\n");

    /*
     *  GObj commands
     */
    if(gobj_gclass(gobj)->command_table) {
        gbuf_printf(gbuf, "\n> %s\n", gobj_short_name(gobj));
        const sdata_desc_t *pcmds = gobj_gclass(gobj)->command_table;
        while(pcmds->name) {
            if(!empty_string(pcmds->name)) {
                add_command_help(gbuf, pcmds, FALSE);
            } else {
                /*
                *  Empty command (not null) is for print a blank line or a title is desc is not empty
                */
                if(!empty_string(pcmds->description)) {
                    gbuf_printf(gbuf, "%s\n", pcmds->description);
                } else {
                    gbuf_printf(gbuf, "\n");
                }
            }
            pcmds++;
        }
    }

    /*
     *  Child commands
     */
    if(level) {
        hgobj child_;
        rc_instance_t *i_child = gobj_first_child(gobj, &child_);
        while(i_child) {
            hgobj child = child_;
            if(gobj_gclass(child)->command_table) {
                gbuf_printf(gbuf, "\n> %s\n", gobj_short_name(child));
                const sdata_desc_t *pcmds = gobj_gclass(child)->command_table;
                while(pcmds->name) {
                    if(!empty_string(pcmds->name)) {
                        add_command_help(gbuf, pcmds, FALSE);
                    } else {
                        /*
                        *  Empty command (not null) is for print a blank line or a title is desc is not empty
                        */
                        if(!empty_string(pcmds->description)) {
                            gbuf_printf(gbuf, "%s\n", pcmds->description);
                        } else {
                            gbuf_printf(gbuf, "\n");
                        }
                    }
                    pcmds++;
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

