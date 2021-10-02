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
typedef struct _ISTREAM {
    hgobj gobj;
    GBUFFER *gbuf;
    size_t data_size;
    size_t max_size;
    const char *event_name;
    const char *delimiter;
    size_t delimiter_size;
    size_t num_bytes;
    char completed;
} ISTREAM;

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
        log_error(LOG_OPT_TRACE_STACK,
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
        log_error(LOG_OPT_TRACE_STACK,
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
    if(ist) {
        GBMEM_FREE(ist->delimiter);
        GBMEM_FREE(ist->event_name);
        GBUF_DECREF(ist->gbuf);
        gbmem_free(ist);
    }
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int istream_read_until_delimiter(
    istream istream,
    const char *delimiter,
    int delimiter_size,
    const char *event
)
{
    ISTREAM *ist = istream;

    if(delimiter_size <= 0) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(ist->gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "delimiter_size is <= 0",
            NULL
        );
        return -1;
    }
    ist->delimiter_size = delimiter_size;

    GBMEM_FREE(ist->delimiter);

    ist->delimiter = gbmem_malloc(delimiter_size);
    if(!ist->delimiter) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", gobj_full_name(ist->gobj),
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "No memory",
            NULL
        );
        return -1;
    }
    memcpy((void *)ist->delimiter, delimiter, delimiter_size);

    GBMEM_FREE(ist->event_name);
    ist->event_name = gbmem_strdup(event);
    ist->completed = FALSE;

    ist->num_bytes = 0;

    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int istream_read_until_num_bytes(istream istream, size_t num_bytes, const char *event)
{
    ISTREAM *ist = istream;

    ist->num_bytes = num_bytes;
    GBMEM_FREE(ist->event_name);
    ist->event_name = gbmem_strdup(event);
    ist->completed = FALSE;

    ist->delimiter = 0;

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
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "gbuf NULL",
            NULL
        );
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

    } else if(ist->delimiter) {
        for(int i=0; i<len; i++) {
            uint8_t c = bf[i];
            if(gbuf_append(ist->gbuf, &c, 1)!=1) {
                log_error(LOG_OPT_TRACE_STACK,
                    "gobj",         "%s", __FILE__,
                    "function",     "%s", __FUNCTION__,
                    "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                    "msg",          "%s", "gbuf FULL",
                    NULL
                );
                return consumed;
            }
            consumed++;
            if(gbuf_leftbytes(ist->gbuf) >= ist->delimiter_size) {
                char *p = gbuf_cur_wr_pointer(ist->gbuf);
                p -= ist->delimiter_size;
                if(memcmp(ist->delimiter, p, ist->delimiter_size) == 0) {
                    ist->completed = TRUE;
                    break;
                }
            }
        }
    }

    if(ist->completed) {
        if(!empty_string(ist->event_name)) {
            json_t *kw = json_pack("{s:I}",
                "gbuffer", (json_int_t)(size_t)ist->gbuf
            );
            /*
            *  gbuf is for client, create a new gbuf
            */
            ist->gbuf = gbuf_create(ist->data_size, ist->max_size, 0,0);
            if(!ist->gbuf) {
                log_error(LOG_OPT_TRACE_STACK,
                    "gobj",         "%s", gobj_full_name(ist->gobj),
                    "function",     "%s", __FUNCTION__,
                    "msgset",       "%s", MSGSET_MEMORY_ERROR,
                    "msg",          "%s", "gbuf_create() return NULL",
                    NULL
                );
            }
            gobj_send_event(ist->gobj, ist->event_name, kw, ist->gobj);
        }
    }

    return consumed;
}

/***************************************************************************
 *  Current reading pointer
 ***************************************************************************/
PUBLIC char *istream_cur_rd_pointer(istream istream)
{
    ISTREAM *ist = istream;
    if(!ist) {
        log_error(LOG_OPT_TRACE_STACK,
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
        log_error(LOG_OPT_TRACE_STACK,
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
        log_error(LOG_OPT_TRACE_STACK,
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
        log_error(LOG_OPT_TRACE_STACK,
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

    if(!ist) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "ist NULL",
            NULL
        );
        return -1;
    }
    if(ist->gbuf) {
        gbuf_decref(ist->gbuf);
        ist->gbuf = 0;
    }
    ist->data_size = data_size;
    ist->max_size = max_size;
    ist->gbuf = gbuf_create(ist->data_size, ist->max_size, 0,0);
    if(!ist->gbuf) {
        log_error(LOG_OPT_TRACE_STACK,
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

    if(!ist) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "ist NULL",
            NULL
        );
        return 0;
    }
    if(!ist->completed) {
        return 0;
    }
    if(!ist->gbuf) {
        log_error(LOG_OPT_TRACE_STACK,
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
    ist->completed = FALSE;
    return p;
}

/***************************************************************************
 *  Reset WRITING pointer
 ***************************************************************************/
PUBLIC int istream_reset_wr(istream istream)
{
    ISTREAM *ist = istream;

    if(!ist || !ist->gbuf) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "ist or gbuf NULL",
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

    if(!ist || !ist->gbuf) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "ist or gbuf NULL",
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

/***************************************************************************
 *  Reset READING and WRITING pointer
 ***************************************************************************/
PUBLIC BOOL istream_is_completed(istream istream)
{
    ISTREAM *ist = istream;

    if(!ist) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "ist NULL",
            NULL
        );
        return FALSE;
    }
    return ist->completed;
}
