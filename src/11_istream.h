/****************************************************************************
 *          ISTREAM.H
 *          Input stream
 *          Mixin: process-data & emit events
 *          Copyright (c) 2013 Niyamaka.
 *          All Rights Reserved.
 ****************************************************************************/

#ifndef _istream_H
#define _istream_H 1

#include <ghelpers.h>
#include "10_gobj.h"

#ifdef __cplusplus
extern "C"{
#endif

/*********************************************************************
 *      Constants
 *********************************************************************/
typedef struct _ISTREAM {
    hgobj gobj;
    GBUFFER *gbuf;
    size_t data_size;
    size_t max_size;
    const char *event_regex;
    const char *regex;
    const char *event_delimiter;
    const char *delimiter;
    const char *event_num_bytes;
    size_t num_bytes;
    char completed;
} ISTREAM;

typedef ISTREAM * istream;

#define ISTREAM_CREATE(var, gobj, data_size, max_size, max_disk_size, encoding)         \
    if(var) {                                                                           \
        log_error(LOG_OPT_TRACE_STACK,                                                  \
            "gobj",         "%s", gobj_full_name(gobj),                                 \
            "function",     "%s", __FUNCTION__,                                         \
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,                                \
            "msg",          "%s", "istream ALREADY exists!",                            \
            NULL                                                                        \
        );                                                                              \
        istream_destroy(var);                                                           \
    }                                                                                   \
    (var) = istream_create(gobj, data_size, max_size, max_disk_size, encoding);

#define ISTREAM_DESTROY(ptr)    \
    if(ptr) {                   \
        istream_destroy(ptr);   \
    }                           \
    (ptr) = 0;

/*********************************************************************
 *      Prototypes
 *********************************************************************/
istream istream_create(
    hgobj gobj,
    size_t data_size,
    size_t max_size,
    size_t max_disk_size,
    gbuf_encoding encoding
);
void istream_destroy(istream istream);
int istream_read_until_num_bytes(istream istream, size_t num_bytes, const char *event);
size_t istream_consume(istream istream, char *bf, size_t len);
char *istream_cur_rd_pointer(istream istream);
size_t istream_length(istream istream);
GBUFFER *istream_get_gbuffer(istream istream);
GBUFFER *istream_pop_gbuffer(istream istream);
int istream_new_gbuffer(istream istream, size_t data_size, size_t max_size);
char *istream_extract_matched_data(istream istream, size_t *len);
int istream_reset_wr(istream istream);
int istream_reset_rd(istream istream);
void istream_clear(istream istream); // reset wr/rd

#ifdef __cplusplus
}
#endif


#endif

