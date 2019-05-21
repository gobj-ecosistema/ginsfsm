/****************************************************************************
 *              GINSFSM.C
 *              Copyright (c) 1996-2015 Niyamaka.
 *              All Rights Reserved.
 ****************************************************************************/
#include "ginsfsm.h"

PRIVATE BOOL initialized = FALSE;

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int init_ginsfsm_library(void)
{
    if(initialized) {
        return -1;
    }

    // nothing to do by the moment

    initialized = TRUE;

    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC void end_ginsfsm_library(void)
{
    if(!initialized) {
        return;
    }
    gobj_end();

    initialized = FALSE;
}
