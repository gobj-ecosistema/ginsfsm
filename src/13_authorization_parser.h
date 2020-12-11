/****************************************************************************
 *          AUTHORIZATION_PARSER.H
 *
 *          Authorization parser
 *
 *          Copyright (c) 2017 Niyamaka.
 *          All Rights Reserved.
 ****************************************************************************/

#ifndef _C_AUTHORIZATION_PARSER_H
#define _C_AUTHORIZATION_PARSER_H 1

#include "ginsfsm.h"

#ifdef __cplusplus
extern "C"{
#endif

/***************************************************
 *              Prototypes
 **************************************************/
PUBLIC json_t * authorization_parser(
    hgobj gobj,
    const char *authorization,
    json_t *kw,
    hgobj src
);

PUBLIC json_t *gobj_build_authzs_doc(
    hgobj gobj,
    json_t *kw
);

PUBLIC const sdata_desc_t *authorization_get_authz_desc(
    const sdata_desc_t *authorization_table,
    const char *authz
);

#ifdef __cplusplus
}
#endif

#endif
