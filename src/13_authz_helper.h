/****************************************************************************
 *          AUTHZ_HELPER.H
 *
 *          Authz helper
 *
 *          Copyright (c) 2020 Niyamaka.
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
    const char *authz
);

PUBLIC const sdata_desc_t *authz_get_level_desc(
    const sdata_desc_t *authz_table,
    const char *authz
);

PUBLIC json_t *gobj_build_authzs_doc(
    hgobj gobj,
    const char *cmd,
    json_t *kw,
    hgobj src
);

#ifdef __cplusplus
}
#endif
