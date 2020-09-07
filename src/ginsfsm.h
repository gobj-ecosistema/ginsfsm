/****************************************************************************
 *              GINSFSM.H
 *              Copyright (c) 1996-2015 Niyamaka.
 *              All Rights Reserved.
 ****************************************************************************/


#ifndef __GINSFSM_H
#define __GINSFSM_H 1

#include <ghelpers.h>

#include "00_msglog_ginsfsm.h"
#include "01_sdata.h"
#include "10_gobj.h"
#include "11_inter_event.h"
#include "11_istream.h"
#include "12_msg_ievent.h"
#include "13_command_parser.h"
#include "13_stats_parser.h"

#ifdef __cplusplus
extern "C"{
#endif

/*********************************************************************
 *      Prototypes
 *********************************************************************/
PUBLIC int init_ginsfsm_library(void);
PUBLIC void end_ginsfsm_library(void);

/*********************************************************************
 *      Version
 *********************************************************************/
#define __ginsfsm_version__  "4.2.10"


#ifdef __cplusplus
}
#endif

#endif
