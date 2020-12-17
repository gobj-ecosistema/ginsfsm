/****************************************************************************
 *          AUTHZ_PARSER.H
 *
 *          Authz parser
 *
 *          Copyright (c) 2017 Niyamaka.
 *          All Rights Reserved.
 ****************************************************************************/
#pragma once

#include "ginsfsm.h"
#include "12_msg_ievent.h"

#ifdef __cplusplus
extern "C"{
#endif

/***************************************************
 *              Prototypes
 **************************************************/
PUBLIC json_t *authz_parser(
    hgobj gobj,
    const char *level,
    json_t *kw,
    hgobj src
);

PUBLIC json_t *gobj_build_authzs_doc(
    hgobj gobj,
    json_t *kw
);

PUBLIC const sdata_desc_t *authz_get_level_desc(
    const sdata_desc_t *authz_table,
    const char *level
);

#ifdef __cplusplus
}
#endif
