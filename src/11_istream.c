/***********************************************************************
 *          ISTREAM.C
 *          Input stream
 *          Mixin: process-data & emit events
 *          Copyright (c) 2013 Niyamaka.
 *          All Rights Reserved.
 ***********************************************************************/
#include "11_istream.h"

/***************************************************************************
 *              Constants
 ***************************************************************************/

/***************************************************************************
 *              Structures
 ***************************************************************************/

/***************************************************************************
 *              Prototypes
 ***************************************************************************/


/***************************************************************************
 *
 ***************************************************************************/
PUBLIC istream istream_create(
    hgobj gobj,
    size_t data_size,
    size_t max_size,
    size_t max_disk_size,
    gbuf_encoding encoding)
{
    ISTREAM *ist;

    /*---------------------------------*
     *   Alloc memory
     *---------------------------------*/
    ist = gbmem_malloc(sizeof(struct _ISTREAM));
    if(!ist) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "bmem_malloc() return NULL",
            "sizeof",       "%d", sizeof(struct _ISTREAM),
            NULL);
        return (istream )0;
    }

    /*---------------------------------*
     *   Inicializa atributos
     *---------------------------------*/
    ist->gobj = gobj;
    ist->data_size = data_size;
    ist->max_size = max_size;

    ist->gbuf = gbuf_create(data_size, max_size, max_disk_size, encoding);
    if(!ist->gbuf) {
        log_error(0,
            "gobj",         "%s", gobj_full_name(gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "gbuf_create() return NULL",
            "data_size",    "%d", data_size,
            NULL);
        istream_destroy(ist);
        return (istream )0;
    }

    /*----------------------------*
     *   Retorna pointer a ist
     *----------------------------*/
    return ist;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC void istream_destroy(istream istream)
{
    ISTREAM *ist = istream;

    /*-----------------------*
     *  Libera la memoria
     *-----------------------*/
    if(ist->gbuf) {
        gbuf_decref(ist->gbuf);
        ist->gbuf = 0;
    }
    gbmem_free(ist);
}

/***************************************************************************
 *  TODO
 ***************************************************************************/
// PUBLIC int istream_read_until_regex(istream istream, const char *regex, const char *event)
// {
//     ISTREAM *ist = istream;
//
//     ist->regex = regex;
//     ist->event_regex = event;
//     ist->completed = FALSE;
//     return 0;
// }

/***************************************************************************
 *  TODO
 ***************************************************************************/
// PUBLIC int istream_read_until_delimiter(istream istream, const char *delimiter, const char *event)
// {
//     ISTREAM *ist = istream;
//
//     ist->delimiter = delimiter;
//     ist->event_delimiter = event;
//     ist->completed = FALSE;
//
//     return 0;
// }

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int istream_read_until_num_bytes(istream istream, size_t num_bytes, const char *event)
{
    ISTREAM *ist = istream;

    ist->num_bytes = num_bytes;
    ist->event_num_bytes = event;
    ist->completed = FALSE;

    return 0;
}

/***************************************************************************
 *  Return number of bytes consumed
 ***************************************************************************/
PUBLIC size_t istream_consume(istream istream, char *bf, size_t len)
{
    ISTREAM *ist = istream;
    size_t consumed = 0;

    if(len == 0) {
        return 0;
    }
    if(!ist->gbuf) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "gbuf NULL",
            NULL);
        return 0;
    }
    if(ist->num_bytes) {
        size_t accumulated = gbuf_leftbytes(ist->gbuf);
        size_t needed = ist->num_bytes - accumulated;
        if(needed > len) {
            gbuf_append(ist->gbuf, bf, len);
            return len;
        }
        if(needed > 0) {
            gbuf_append(ist->gbuf, bf, needed);
            consumed = needed;
        }
        ist->completed = TRUE;
        if(!empty_string(ist->event_num_bytes)) {
            json_t *kw = json_pack("{s:I}",
                "gbuffer", (json_int_t)(size_t)ist->gbuf
            );
            /*
             *  gbuf is for client, create a new gbuf
             */
            ist->gbuf = gbuf_create(ist->data_size, ist->max_size, 0,0);
            if(!ist->gbuf) {
                log_error(0,
                    "gobj",         "%s", gobj_full_name(ist->gobj),
                    "function",     "%s", __FUNCTION__,
                    "msgset",       "%s", MSGSET_MEMORY_ERROR,
                    "msg",          "%s", "gbuf_create() return NULL",
                    NULL);
            }
            gobj_send_event(ist->gobj, ist->event_num_bytes, kw, ist->gobj);
        }
        return consumed;
    }

    return 0;
}

/***************************************************************************
 *  Current reading pointer
 ***************************************************************************/
PUBLIC char *istream_cur_rd_pointer(istream istream)
{
    ISTREAM *ist = istream;
    if(!ist) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "gbuf NULL",
            NULL
        );
        return 0;
    }
    return gbuf_cur_rd_pointer(ist->gbuf);
}

/***************************************************************************
 *  Current length of internal gbuffer
 ***************************************************************************/
PUBLIC size_t istream_length(istream istream)
{
    ISTREAM *ist = istream;
    if(!ist) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "gbuf NULL",
            NULL
        );
        return 0;
    }
    return gbuf_leftbytes(ist->gbuf);
}

/***************************************************************************
 *  Get current gbuffer
 ***************************************************************************/
PUBLIC GBUFFER *istream_get_gbuffer(istream istream)
{
    ISTREAM *ist = istream;
    if(!ist) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "gbuf NULL",
            NULL
        );
        return 0;
    }
    return ist->gbuf;
}

/***************************************************************************
 *  Pop current gbuffer
 ***************************************************************************/
PUBLIC GBUFFER *istream_pop_gbuffer(istream istream)
{
    ISTREAM *ist = istream;
    if(!ist) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "gbuf NULL",
            NULL
        );
        return 0;
    }
    GBUFFER *gbuf = ist->gbuf;
    ist->gbuf = 0;
    return gbuf;
}

/***************************************************************************
 *  Create new gbuffer
 ***************************************************************************/
PUBLIC int istream_new_gbuffer(istream istream, size_t data_size, size_t max_size)
{
    ISTREAM *ist = istream;

    if(ist->gbuf) {
        gbuf_decref(ist->gbuf);
        ist->gbuf = 0;
    }
    ist->data_size = data_size;
    ist->max_size = max_size;
    ist->gbuf = gbuf_create(ist->data_size, ist->max_size, 0,0);
    if(!ist->gbuf) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "gbuf_create() return NULL",
            NULL);
        return -1;
    }
    return 0;
}

/***************************************************************************
 *  Get the matched data
 ***************************************************************************/
PUBLIC char *istream_extract_matched_data(istream istream, size_t *len)
{
    ISTREAM *ist = istream;
    char *p;
    size_t ln;

    if(!istream->completed) {
        return 0;
    }
    if(!ist->gbuf) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "gbuf NULL",
            NULL);
        return 0;
    }
    ln = gbuf_leftbytes(ist->gbuf);
    p = gbuf_get(ist->gbuf, ln);
    if(len)
        *len = ln;
    istream->completed = FALSE;
    return p;
}

/***************************************************************************
 *  Reset WRITING pointer
 ***************************************************************************/
PUBLIC int istream_reset_wr(istream istream)
{
    ISTREAM *ist = istream;

    if(!ist->gbuf) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "gbuf NULL",
            NULL
        );
        return -1;
    }
    gbuf_reset_wr(ist->gbuf);
    return 0;
}

/***************************************************************************
 *  Reset READING pointer
 ***************************************************************************/
PUBLIC int istream_reset_rd(istream istream)
{
    ISTREAM *ist = istream;

    if(!ist->gbuf) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "gbuf NULL",
            NULL
        );
        return -1;
    }
    gbuf_reset_rd(ist->gbuf);
    return 0;
}

/***************************************************************************
 *  Reset READING and WRITING pointer
 ***************************************************************************/
PUBLIC void istream_clear(istream istream)
{
    istream_reset_rd(istream);
    istream_reset_wr(istream);
}
