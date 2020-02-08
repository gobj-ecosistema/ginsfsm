/****************************************************************************
 *          STATS_PARSER.H
 *
 *          Stats parser
 *
 *          Copyright (c) 2017 Niyamaka.
 *          All Rights Reserved.
 ****************************************************************************/

#ifndef _C_STATS_PARSER_H
#define _C_STATS_PARSER_H 1

#include "ginsfsm.h"
#include "12_msg_ievent.h"

#ifdef __cplusplus
extern "C"{
#endif

/***************************************************
 *              Prototypes
 **************************************************/
PUBLIC json_t * stats_parser(
    hgobj gobj,
    const char *stats,
    json_t *kw,
    hgobj src
);

PUBLIC json_t *build_stats( // Build stats from gobj's attributes with SFD_STATS flag.
    hgobj gobj,
    const char *stats,
    json_t *kw,
    hgobj src
);


#ifdef __cplusplus
}
#endif

#endif
