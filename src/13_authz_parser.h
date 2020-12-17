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
PUBLIC json_t *authzs_list(
    hgobj gobj,
    const char *authz,
    json_t *kw,
    hgobj src
);
PUBLIC json_t *build_authzs(
    hgobj gobj,
    const char *authz,
    json_t *kw,
    hgobj src
);

PUBLIC const sdata_desc_t *authz_get_level_desc(
    const sdata_desc_t *authz_table,
    const char *authz
);

#ifdef __cplusplus
}
#endif
