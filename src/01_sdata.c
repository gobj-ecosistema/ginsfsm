/***********************************************************************
 *              SDATA.C
 *              Structured Data Table
 *              Copyright (c) 2013-2016 Niyamaka.
 *              All Rights Reserved.
 ***********************************************************************/
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <strings.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include "01_sdata.h"

/****************************************************************
 *         Constants
 ****************************************************************/
#define _FLAG_DESTROYED     0x0001
#define IS_DESTROYING(s)    ((s)->_flag & _FLAG_DESTROYED)

/****************************************************************
 *         Structures
 ****************************************************************/
typedef struct _SData_t {
    /*
     *  There may be millions of this structure. Please keep straw free.
     */

    /*
     *  RC_RESOURCE_HEADER
     */
    struct _SData_t *__parent__;
    dl_list_t dl_instances;
    dl_list_t dl_childs;
    size_t refcount;

    const sdata_desc_t *items;
    void *user_data;        /* user data for callbacks */

    post_write_it_cb post_write_cb;
    post_read_it_cb post_read_cb;
    post_write2_it_cb post_write_stats_cb;

    char resource[32];      /* Resource of the record. User utility. */

    /*
     *  Internal use
     */
    uint32_t _total_size;
    uint64_t _numb;         // number of items
    uint64_t _max_suboid;   // maximum oid in this sdata (last oid + 1)
    uint32_t _flag;         // flag
    char *_bf;              // internal buffer
} SData_t;


/****************************************************************
 *         Data
 ****************************************************************/
PRIVATE int sizeof_types[][2] = {
{ASN_INTEGER,       sizeof(TYPE_ASN_INTEGER)},
{ASN_COUNTER,       sizeof(TYPE_ASN_COUNTER)},
{ASN_UNSIGNED,      sizeof(TYPE_ASN_UNSIGNED)},
{ASN_TIMETICKS,     sizeof(TYPE_ASN_TIMETICKS)},
{ASN_OCTET_STR,     sizeof(TYPE_ASN_OCTET_STR)+1},  // +1 space for final null. What???
{ASN_IPADDRESS,     sizeof(TYPE_ASN_IPADDRESS)+1},
{ASN_BOOLEAN,       sizeof(TYPE_ASN_BOOLEAN)},
{ASN_COUNTER64,     sizeof(TYPE_ASN_COUNTER64)},
{ASN_FLOAT,         sizeof(TYPE_ASN_FLOAT)},
{ASN_DOUBLE,        sizeof(TYPE_ASN_DOUBLE)},
{ASN_INTEGER64,     sizeof(TYPE_ASN_INTEGER64)},
{ASN_UNSIGNED64,    sizeof(TYPE_ASN_UNSIGNED64)},
{ASN_JSON,          sizeof(TYPE_ASN_JSON)},
{ASN_DL_LIST,       sizeof(TYPE_DL_LIST)},
{ASN_POINTER,       sizeof(TYPE_POINTER)},
{ASN_ITER,          sizeof(TYPE_ITER)},
{0,0}
};

#define SDATA_METADATA_TYPE uint32_t

PRIVATE const char *sdata_flag_names[] = {
    "SDF_NOTACCESS",
    "SDF_RD",
    "SDF_WR",
    "SDF_REQUIRED",
    "SDF_PERSIST",
    "SDF_VOLATIL",
    "SDF_RESOURCE",
    "SDF_PKEY",
    "SDF_PURECHILD",
    "SDF_PARENTID",
    "SDF_WILD_CMD",
    "SDF_STATS",
    "SDF_FKEY",
    "SDF_RSTATS",
    "SDF_PSTATS",
    "SDF_AUTHZ_R",
    "SDF_AUTHZ_W",
    "SDF_AUTHZ_X",
    "SDF_AUTHZ_P",
    0
};


/****************************************************************
 *         Prototypes
 ****************************************************************/
PRIVATE int items_count(register const sdata_desc_t *items);
PRIVATE int calculate_size(register SData_t *sdata, int acc);
PRIVATE void build_default_values(SData_t *sdata);
PRIVATE void clear_values(SData_t *sdata);
PRIVATE void clear_value(SData_t *sdata, const sdata_desc_t *it);
PRIVATE void set_default(SData_t *sdata, const sdata_desc_t *it, void *value);
PRIVATE void *item_pointer(hsdata hs, const sdata_desc_t *it);
PRIVATE json_t *itdesc2json0(const sdata_desc_t *it);
PRIVATE json_t *itdesc2json(const sdata_desc_t *it);




                /*----------------------------*
                 *      Creation functions
                 *---------------------------*/




/***************************************************************************
 *  Create a structured data
 ***************************************************************************/
PUBLIC hsdata sdata_create(
    const sdata_desc_t* schema,
    void* user_data,
    post_write_it_cb post_write_cb,
    post_read_it_cb post_read_cb,
    post_write2_it_cb post_write_stats_cb,
    const char* resource)
{
    if(!schema) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "items NULL",
            NULL
        );
        return 0;
    }

    SData_t *sdata = gbmem_malloc(sizeof(SData_t));
    if(!sdata) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_MEMORY_ERROR,
            "msg",          "%s", "no memory for sdata",
            "size",         "%d", sizeof(SData_t),
            NULL
        );
        return 0;
    }
    sdata->refcount = 1;
    sdata->user_data = user_data;
    if(resource) {
        snprintf(sdata->resource, sizeof(sdata->resource), "%s", resource);
    }

    /*
     *  Calculate size of buffer and alloc
     */
    sdata->items = schema;
    sdata->_total_size = calculate_size(sdata, 0); // idempotent.

    if(sdata->_total_size) {
        sdata->_bf = gbmem_malloc(sdata->_total_size);
        if(!sdata->_bf) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_MEMORY_ERROR,
                "msg",          "%s", "no memory for bf",
                "size",         "%d", sdata->_total_size,
                NULL
            );
            return 0;
        }
    }

    /*
     *  Build default values
     */
    build_default_values(sdata);

    /*
     *  Set this after build_default_values()
     *  for avoid early call to write_ib
     */
    sdata->post_write_cb = post_write_cb;
    sdata->post_read_cb = post_read_cb;
    sdata->post_write_stats_cb = post_write_stats_cb;
    return sdata;
}

/***************************************************************************
 *  Delete structured data
 ***************************************************************************/
PUBLIC void sdata_destroy(hsdata hs)
{
    SData_t *sdata = hs;
    if(!sdata) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "hs NULL",
            NULL
        );
        return;
    }
    sdata->_flag |=_FLAG_DESTROYED;
    clear_values(sdata);
    GBMEM_FREE(sdata->_bf);
    GBMEM_FREE(sdata);
}

/***************************************************************************
 *  Calculate the offset of all items and the total size.
 *  acc = size accumulated
 ***************************************************************************/
PRIVATE int get_sizeof_type(uint8_t type)
{
    int i;

    for(i=0; sizeof_types[i][0]!=0; i++) {
        if(sizeof_types[i][0] == type) {
            return sizeof_types[i][1];
        }
    }
    log_error(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", __FILE__,
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "type NOT FOUND",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  Calculate the offset of all items and the total size.
 *  acc = size accumulated
 ***************************************************************************/
PRIVATE int items_count(register const sdata_desc_t *items)
{
    int n = 0;
    if(!items) {
        return 0;
    }

    while(items->name != 0) {
        n++;
        items++;
    }
    return n;
}

/***************************************************************************
 *  Get the idx of item in schema.
 *  Relative to 1
 ***************************************************************************/
// PRIVATE int item_idx(
//     const sdata_desc_t *schema,
//     const char *name)
// {
//     int n = 0;
//
//     while(schema->name != 0) {
//         if(strcasecmp(schema->name, name)==0) {
//             return n+1;
//         }
//         n++;
//         schema++;
//     }
//     return 0;
// }

/***************************************************************************
 *  Calculate the offset of all items and the total size.
 *  acc = size accumulated
 ***************************************************************************/
PRIVATE int calculate_size(register SData_t *sdata, int acc)
{
    const sdata_desc_t *it = sdata->items;

    if(!it) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "items is NULL",
            NULL
        );
        return acc;
    }

    while(it->name != 0) {
        int type = it->type;
        int size;

        ((sdata_desc_t *)it)->_offset = acc + sizeof(SDATA_METADATA_TYPE);
        acc += sizeof(SDATA_METADATA_TYPE);    // HACK space to metadata of variable

        size = get_sizeof_type(type);
        acc += size;

        ((sdata_desc_t *)it)->_ln = acc - it->_offset;
        it++;
    }
    return acc;
}

/***************************************************************************
 *  Build default values
 ***************************************************************************/
PRIVATE void build_default_values(SData_t *sdata)
{
    const sdata_desc_t *it = sdata->items;
    int suboid;

    suboid = 1;
    sdata->_numb = 0;
    while(it->name != 0) {
        set_default(sdata, it, it->default_value);
        ((sdata_desc_t *)it)->_suboid = suboid;
        suboid++;
        sdata->_max_suboid = suboid;
        it++;
        sdata->_numb++;
    }
}

/***************************************************************************
 *  Clear values
 ***************************************************************************/
PRIVATE void clear_values(SData_t *sdata)
{
    const sdata_desc_t *it = sdata->items;

    while(it->name != 0) {
        clear_value(sdata, it);
        it++;
    }
}

/***************************************************************************
 *  Set array values to sdata, from json or binary
 ***************************************************************************/
PRIVATE void clear_value(SData_t *sdata, const sdata_desc_t *it)
{
    void *ptr = item_pointer(sdata, it);
    if(!ptr) {
        // Error already logged
        return;
    }

    if(ASN_IS_ITER(it->type)) {
        if(rc_iter_size(ptr)>0) {
            rc_free_iter(ptr, FALSE, it->free_fn);
        }
    } else if(ASN_IS_DL_LIST(it->type)) {
        if(dl_size(ptr)) {
            dl_flush(ptr, it->free_fn);
        }
    } else {
        SData_Value_t v = {0};

        if(ASN_IS_STRING(it->type)) {
            v.s = 0;
        } else if(ASN_IS_JSON(it->type)) {
            v.j = 0;
        } else if(ASN_IS_POINTER(it->type)) {
            v.p = 0;
        } else if(ASN_IS_UNSIGNED32(it->type)) {
            v.u32 = 0;
        } else if(ASN_IS_UNSIGNED64(it->type)) {
            v.u64 = 0;
        } else if(ASN_IS_SIGNED32(it->type)) {
            v.i32 = 0;
        } else if(ASN_IS_SIGNED64(it->type)) {
            v.i64 = 0;
        } else if(it->type == ASN_FLOAT) {
            v.f = 0;
        } else if(it->type == ASN_DOUBLE) {
            v.f = 0;
        } else { /* Let RESOURCE and DL_LIST arrive here */
            log_error(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "ASN it->type, not case FOUND",
                "it->type",     "%d", it->type,
                NULL
            );
        }

        if(sdata_write_by_type(sdata, it, ptr, v)<0) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "sdata_write_by_type() FAILED",
                NULL
            );
        }
    }
}

/***************************************************************************
 *  Set array values to sdata, from json or binary
 ***************************************************************************/
PRIVATE void set_default(SData_t *sdata, const sdata_desc_t *it, void *value)
{
    int is_json;
    is_json = ASN_IS_JSON(it->type)? 1:0;

    if(is_json && value)
        value = nonlegalstring2json(value, TRUE);

    /*-----------------------------------*
        *  Get the value and assign it.
        *-----------------------------------*/
    void *ptr = item_pointer(sdata, it);
    if(!ptr) {
        // Error already logged
        return;
    }

    if(ASN_IS_ITER(it->type)) {
        if(rc_iter_size(ptr)>0) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "Current resource iter has items!",
                "name",         "%s", it->name,
                NULL
            );
        }
        rc_init_iter(ptr);
    } else if(ASN_IS_DL_LIST(it->type)) {
        if(dl_size(ptr)) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "Current resource iter has items!",
                "name",         "%s", it->name,
                NULL
            );
        }
        dl_init(ptr);
    } else {
        SData_Value_t v = {0};

        if(ASN_IS_STRING(it->type)) {
            v.s = value;
        } else if(ASN_IS_JSON(it->type)) {
            v.j = value;
        } else if(ASN_IS_POINTER(it->type)) {
            v.p = value;
        } else if(ASN_IS_UNSIGNED32(it->type)) {
            v.u32 = (uint32_t)(size_t)value;
        } else if(ASN_IS_UNSIGNED64(it->type)) {
            v.u64 = (uint64_t)(size_t)value;
        } else if(ASN_IS_SIGNED32(it->type)) {
            v.i32 = (int32_t)(size_t)value;
        } else if(ASN_IS_SIGNED64(it->type)) {
            v.i64 = (int64_t)(size_t)value;
        } else if(it->type == ASN_FLOAT) {
            v.f = (double)(size_t)value;
        } else if(it->type == ASN_DOUBLE) {
            v.f = (double)(size_t)value;
        } else { /* Let RESOURCE and DL_LIST arrive here */
            log_error(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "ASN it->type, not case FOUND",
                "it->type",     "%d", it->type,
                NULL
            );
        }

        if(sdata_write_by_type(sdata, it, ptr, v)<0) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "sdata_write_by_type() FAILED",
                NULL
            );
        }
    }
    if(is_json && value) {
        json_t *v = value;
        JSON_DECREF(v);
    }
}




                /*----------------------------*
                 *      Schema functions
                 *---------------------------*/




/***************************************************************************
 *  Get resource name
 ***************************************************************************/
PUBLIC const char * sdata_resource(hsdata hs)
{
    if(!hs) {
        return 0;
    }
    SData_t *sdata = hs;
    return sdata->resource;
}

/***************************************************************************
 * Return sdata table describing the resource
 ***************************************************************************/
PUBLIC const sdata_desc_t * resource_schema(
    const sdata_desc_t *tb_resources,
    const char *resource,
    sdata_flag_t *flag)
{
    const sdata_desc_t *it = tb_resources;
    while(resource && it && it->name) {
        if(it->flag & SDF_RESOURCE) {
            if(strcmp(it->name, resource)==0) {
                if(flag) {
                    *flag = it->flag;
                }
                return it->schema;
            }
        }
        it++;
    }
    if(flag) {
        *flag = 0;
    }
    log_error(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", __FILE__,
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_PARAMETER_ERROR,
        "msg",          "%s", "resource schema not found",
        "resource",     "%s", resource?resource:"",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  Schema type to simple json type
 ***************************************************************************/
PRIVATE json_t *sdatatype2simplejsontype(const sdata_desc_t *it)
{
    int type = it->type;

    if(ASN_IS_STRING(type)) {
        return json_string("");
    } else if(ASN_IS_BOOLEAN(type)) {
        return json_false();
    } else if(ASN_IS_NATURAL_NUMBER(type)) {
        return json_integer(0);
    } else if(ASN_IS_REAL_NUMBER(type)) {
        return json_real(0.0);
    } else if(ASN_IS_JSON(type)) {
        return json_null();
    } else {
        return json_null();
    }
}

/***************************************************************************
 * Schema to json
 ***************************************************************************/
PUBLIC json_t *schema2json(
    const sdata_desc_t *schema,
    const char **key
)
{
    *key = 0;
    json_t *jn_fields = json_object();
    const sdata_desc_t *it = schema;
    while(it && it->name) {
        if(!(it->flag & SDF_PERSIST)) {
            it++;
            continue;
        }
        if(it->flag & SDF_NOTACCESS) {
            it++;
            continue;
        }
        json_object_set_new(jn_fields, it->name, sdatatype2simplejsontype(it));
        if(it->flag & SDF_PKEY) {
            *key = it->name;
        }
        it++;
    }
    // TODO set system_latch_t (from flag?) system keys: __latch__
    return jn_fields;
}

/***************************************************************************
 *  Get items desc
 ***************************************************************************/
PUBLIC const sdata_desc_t * sdata_schema(hsdata hs)
{
    SData_t *sdata = hs;
    if(!sdata) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hsdata is NULL",
            NULL
        );
        return 0;
    }
    return sdata->items;
}

/***************************************************************************
 *  Get sdata desc of item
 ***************************************************************************/
PUBLIC const sdata_desc_t * sdata_it_desc(const sdata_desc_t *schema, const char *name)
{
    if(!schema) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "it is NULL",
            "name",         "%s", name?name:"",
            NULL
        );
        return 0;
    }
    if(empty_string(name)) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "No name",
            NULL
        );
        return 0;
    }

    const sdata_desc_t *it = schema;
    while(it->name) {
        if(strcasecmp(it->name, name)==0) {
            return it;
        }
        it++;
    }

    return 0;
}

/***************************************************************************
 *  Get name of sdata type
 ***************************************************************************/
PRIVATE const char *sdata_type_name(uint8_t type)
{
    if(ASN_IS_STRING(type)) {
        return "ASN_OCTET_STR";
    } else if(ASN_IS_BOOLEAN(type)) {
        return "ASN_BOOLEAN";
    } else if(ASN_IS_UNSIGNED32(type)) {
        return "ASN_UNSIGNED";
    } else if(ASN_IS_SIGNED32(type)) {
        return "ASN_INTEGER";
    } else if(ASN_IS_UNSIGNED64(type)) {
        return "ASN_UNSIGNED64";
    } else if(ASN_IS_SIGNED64(type)) {
        return "ASN_INTEGER64";
    } else if(ASN_IS_DOUBLE(type)) {
        return "ASN_DOUBLE";
    } else if(ASN_IS_JSON(type)) {
        return "ASN_JSON";
    } else if(ASN_IS_ITER(type)) {
        return "ASN_ITER";
    } else if(ASN_IS_POINTER(type)) {
        return "ASN_POINTER";
    } else if(ASN_IS_DL_LIST(type)) {
        return "ASN_DL_LIST";
    } else {
        return "UNKNOWN";
    }
}




                /*----------------------------*
                 *      Cell  functions
                 *---------------------------*/




/***************************************************************************
 *  Get the address pointer of a sdata item metadata
 ***************************************************************************/
PRIVATE SDATA_METADATA_TYPE *item_metadata_pointer(hsdata hs, const sdata_desc_t *it)
{
    SData_t *sdata = hs;
    void *pointer;

    if(!sdata) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "hsdata NULL",
            "name",         "%s", it->name,
            NULL
        );
        return 0;
    }
    if(!sdata->_bf) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "Internal buffer NULL",
            "name",         "%s", it->name,
            NULL
        );
        return 0;
    }
    pointer = sdata->_bf + it->_offset - sizeof(SDATA_METADATA_TYPE);
    return pointer;
}

/***************************************************************************
 *  Get the address pointer of a sdata item
 ***************************************************************************/
PRIVATE void *item_pointer(hsdata hs, const sdata_desc_t *it)
{
    SData_t *sdata = hs;
    void *pointer;

    if(!sdata) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "hsdata NULL",
            "name",         "%s", it->name,
            NULL
        );
        return 0;
    }
    if(!sdata->_bf) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "Internal buffer NULL",
            "name",         "%s", it->name,
            NULL
        );
        return 0;
    }
    pointer = sdata->_bf + it->_offset;
    return pointer;
}

/***************************************************************************
 *  ATTR: get the attr pointer
 *  DANGER if you don't cast well: OVERFLOW variables!
 ***************************************************************************/
PUBLIC void *sdata_it_pointer(hsdata hs, const char *name, const sdata_desc_t **pit)
{
    SData_t *sdata = hs;
    if(!sdata) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata NULL",
            "name",         "%s", name,
            NULL
        );
        if(pit)
            *pit = 0;
        return 0;
    }

    const sdata_desc_t *it = sdata_it_desc(sdata->items, name);

    if(!it) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata item NOT FOUND",
            "name",         "%s", name,
            NULL
        );
        if(pit)
            *pit = 0;
        return 0;
    }
    if(pit) {
        *pit = it;
    }
    return item_pointer(sdata, it);
}

/***************************************************************************
 *  Write value to sdata, from binary.
 ***************************************************************************/
PUBLIC int sdata_write_by_type(
    hsdata hs,
    const sdata_desc_t *it,
    void *ptr,
    SData_Value_t value)
{
    SData_t *sdata = hs;
    SData_Value_t old_value = {0};

    if(ASN_IS_STRING(it->type)) {
        char **s = ptr;
        char *new_s = 0;
        if(value.s) {
            new_s = gbmem_strdup(value.s);
        }
        if(*s) {
            gbmem_free(*s);
            *s = 0;
        }
        *s = new_s;
    } else if(ASN_IS_JSON(it->type)) {
        json_t **jn = ptr;
// WARNING new 7-7-2016. Efecto colateral?
// Comportamiento demasiado implicito, lo quito
//         if(json_is_array(*jn) && json_is_array(value.j)) {
//             json_array_extend(*jn, value.j);
//         } else if(json_is_object(*jn) && json_is_object(value.j)) {
//             json_object_update(*jn, value.j);
//         } else {
        {
            if(*jn) {
                // remove the previous value
                json_t *_jn = *jn;
                if(_jn && _jn->refcount <= 0) {
                    log_error(LOG_OPT_TRACE_STACK,
                        "gobj",         "%s", __FILE__,
                        "function",     "%s", __FUNCTION__,
                        "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                        "msg",          "%s", "BAD JSON_DECREF()",
                        "name",         "%s", it->name,
                        "desc",         "%s", it->description,
                        NULL
                    );
                } else {
                    KW_DECREF(*jn); // WARNING cambiado JSON_ por KW_ 2.0.17
                }
                *jn = 0;
            }
            if(value.j) {
                *jn = value.j;
                KW_INCREF(*jn); // WARNING collateral damage: new in V2, y cambiado JSON_ por KW_ 2.0.17
            }
        }
    } else if(ASN_IS_POINTER(it->type)) {
        void  **p = ptr;
        *p = value.p;

    } else if(ASN_IS_UNSIGNED32(it->type)) {
        uint32_t *pi = ptr;
        old_value.u32 = *pi;
        *pi = value.u32;

    } else if(ASN_IS_UNSIGNED64(it->type)) {
        uint64_t *pl = ptr;
        old_value.u64 = *pl;
        *pl = value.u64;

    } else if(ASN_IS_SIGNED32(it->type)) {
        int32_t *pi = ptr;
        old_value.i32 = *pi;
        *pi = value.i32;

    } else if(ASN_IS_SIGNED64(it->type)) {
        int64_t *pl = ptr;
        old_value.i64 = *pl;
        *pl = value.i64;

    } else if(it->type == ASN_FLOAT) {
        float *pf = ptr;
        old_value.f = *pf;
        *pf = value.f;

    } else if(it->type == ASN_DOUBLE) {
        double *pd = ptr;
        old_value.f = *pd;
        *pd = value.f;

    } else {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "ASN it->type, not case FOUND",
            "it->name",     "%s", it->name,
            "it->type",     "%d", it->type,
            NULL
        );
        return -1;
    }

    if(!IS_DESTROYING(sdata)) {
        if(sdata->post_write_cb) {
            sdata->post_write_cb(sdata->user_data, it->name);
        }

        SDATA_METADATA_TYPE *p = item_metadata_pointer(sdata, it);
        SDATA_METADATA_TYPE m = *p;
        if((it->flag & (SDF_STATS|SDF_RSTATS|SDF_PSTATS))) {
            m |= 0x8000;
        }
        if(m & 0x8000) {
            if(sdata->post_write_stats_cb) {
                sdata->post_write_stats_cb(sdata->user_data, it->name, it->type, old_value, value);
            }
        }
    }

    return 0;
}

/***************************************************************************
 *  Read value from sdata
 ***************************************************************************/
PUBLIC SData_Value_t sdata_read_by_type(hsdata hs, const sdata_desc_t *it, void *ptr)
{
    SData_t *sdata = hs;
    SData_Value_t value = {0};
    int type = it->type;

    if(ASN_IS_STRING(type)) {
        char **s = ptr;
        value.s = *s;
    } else if(ASN_IS_JSON(type)) {
        json_t **jn = ptr;
        value.j = *jn;
    } else if(ASN_IS_DL_LIST(type)) {
        // return reference
        dl_list_t *dl_list = ptr;
        value.n = dl_list;
    } else if(ASN_IS_ITER(type)) {
        // return reference
        dl_list_t *dl_list = ptr;
        value.r = dl_list;
    } else if(ASN_IS_POINTER(type)) {
        void **p = ptr;
        value.p = *p;
    } else if(ASN_IS_SIGNED32(type)) {
        int32_t *pi = ptr;
        value.i32 = *pi;
    } else if(ASN_IS_UNSIGNED32(type)) {
        uint32_t *pi = ptr;
        value.u32 = *pi;
    } else if(ASN_IS_SIGNED64(type)) {
        int64_t *pl = ptr;
        value.i64 = *pl;
    } else if(ASN_IS_UNSIGNED64(type)) {
        uint64_t *pl = ptr;
        value.u64 = *pl;
    } else if(type == ASN_FLOAT) {
        float *pf = ptr;
        value.f = *pf;
    } else if(type == ASN_DOUBLE) {
        double *pd = ptr;
        value.f = *pd;
    } else {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "ASN type, not case FOUND",
            "name",         "%s", it->name,
            "type",         "%d", type,
            NULL
        );
    }
    if(sdata && sdata->post_read_cb) {
        value = sdata->post_read_cb(sdata->user_data, it->name, it->type, value);
    }
    return value;
}




                /*----------------------------*
                 *      Utils
                 *---------------------------*/




/***************************************************************************
 *
 ***************************************************************************/
PUBLIC uint32_t sdata_set_stats_metadata(hsdata hs, const char *name, uint32_t mask, BOOL set)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    mask |= 0x8000;

    SDATA_METADATA_TYPE *p = item_metadata_pointer(sdata, it);
    SDATA_METADATA_TYPE m = *p;
    SDATA_METADATA_TYPE old = m;
    if(set) {
        m |= mask;
        *p = m;
    } else {
        m &= ~mask;
        *p = m;
    }
    return old;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC uint32_t sdata_get_stats_metadata(hsdata hs, const char *name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }

    SDATA_METADATA_TYPE *p = item_metadata_pointer(sdata, it);
    SDATA_METADATA_TYPE m = *p;
    if((it->flag & (SDF_STATS|SDF_RSTATS|SDF_PSTATS))) {
        m |= 0x8000;
    }
    return m;
}

/***************************************************************************
 *    Return refcont (lives)
 ***************************************************************************/
PUBLIC int sdata_lives(hsdata hs)
{
    SData_t *sdata = hs;

    if(!sdata)
        return 0;
    return sdata->refcount;
}

/***************************************************************************
 *  Get a gbuffer with type strings
 ***************************************************************************/
PUBLIC GBUFFER *get_sdata_flag_desc(sdata_flag_t flag)
{
    GBUFFER *gbuf = gbuf_create(1024, 1024, 0, 0);
    if(!gbuf) {
        return 0;
    }
    BOOL add_sep = FALSE;

    char **name = (char **)sdata_flag_names;
    while(*name) {
        if(flag & 0x01) {
            if(add_sep) {
                gbuf_append(gbuf, "|", 1);
            }
            gbuf_append(gbuf, *name, strlen(*name));
            add_sep = TRUE;
        }
        flag = flag >> 1;
        name++;
    }
    return gbuf;
}

/***************************************************************************
 *  CHECK - return TRUE if the required attribute has a value
 ***************************************************************************/
PRIVATE BOOL attr_with_value(const sdata_desc_t *it, SData_Value_t value)
{
    if(ASN_IS_STRING(it->type)) {
        if(!value.s) {
            return FALSE;
        }
    } else if(ASN_IS_JSON(it->type)) {
        if(!value.j) {
            return FALSE;
        }
    } else if(ASN_IS_POINTER(it->type)) {
        if(!value.p) {
            return FALSE;
        }
    } else if(ASN_IS_SIGNED32(it->type)) {
        if(!value.i32) {
            return FALSE;
        }
    } else if(ASN_IS_UNSIGNED32(it->type)) {
        if(!value.u32) {
            return FALSE;
        }
    } else if(ASN_IS_SIGNED64(it->type)) {
        if(!value.i64) {
            return FALSE;
        }
    } else if(ASN_IS_UNSIGNED64(it->type)) {
        if(!value.u64) {
            return FALSE;
        }
    } else if(ASN_IS_DOUBLE(it->type)) {
        if(!value.f) {
            return FALSE;
        }
    }
    return TRUE;
}

/***************************************************************************
 *  CHECK - return TRUE if all required attributes are present (not null)
 ***************************************************************************/
PUBLIC BOOL sdata_check_required_attrs(
    hsdata hs,
    not_found_cb_t not_found_cb, // Called when the key not exist in hsdata
    void *user_data
)
{
    SData_t *sdata = hs;
    if(!sdata) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "sdata is NULL",
            NULL
        );
        return FALSE;
    }
    const sdata_desc_t *it = sdata->items;
    if(!it) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "sdata items is NULL",
            NULL
        );
        return FALSE;
    }

    while(it->name) {
        if(it->flag & SDF_REQUIRED) {
            void *ptr = item_pointer(hs, it);
            SData_Value_t value = sdata_read_by_type(hs, it, ptr);
            if(!attr_with_value(it, value)) {
                if(not_found_cb) {
                    not_found_cb(user_data, it->name);
                }
                return FALSE;
            }
        }
        it++;
    }
    return TRUE;
}

/***************************************************************************
 *  Return a the selected keys
 ***************************************************************************/
PUBLIC const char **sdata_keys( // WARNING remember free with gbmem_free()
    const sdata_desc_t *schema,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag
)
{
    int count = items_count(schema);
    count++;
    const char **keys = gbmem_malloc(count *sizeof(char *));
    if(!keys) {
        return 0;
    }

    const sdata_desc_t *it = schema;
    int idx = 0;
    while(it->name) {
        if(exclude_flag && (it->flag & exclude_flag)) {
            it++;
            continue;
        }
        if(include_flag == -1 || (it->flag & include_flag)) {
            *(keys + idx) = it->name;
            idx++;
        }
        it++;
    }

    return keys;
}

/***************************************************************************
 *  Return a json object describing the hsdata for commands
 ***************************************************************************/
PRIVATE json_t *cmddesc2json(const sdata_desc_t *it)
{
    int type = it->type;

    if(!ASN_IS_SCHEMA(type)) {
        return 0; // Only schemas please
    }

    json_t *jn_it = json_object();

    json_object_set_new(jn_it, "id", json_string(it->name));

    if(it->alias) {
        json_t *jn_alias = json_array();
        json_object_set_new(jn_it, "alias", jn_alias);
        const char **alias = it->alias;
        while(*alias) {
            json_array_append_new(jn_alias, json_string(*alias));
            alias++;
        }
    }

    json_object_set_new(jn_it, "description", json_string(it->description));
    GBUFFER *gbuf = get_sdata_flag_desc(it->flag);
    if(gbuf) {
        int l = gbuf_leftbytes(gbuf);
        if(l) {
            char *pflag = gbuf_get(gbuf, l);
            json_object_set_new(jn_it, "flag", json_string(pflag));
        } else {
            json_object_set_new(jn_it, "flag", json_string(""));
        }
        gbuf_decref(gbuf);
    }

    gbuf = gbuf_create(256, 16*1024, 0, 0);
    gbuf_printf(gbuf, "%s ", it->name);
    const sdata_desc_t *pparam = it->schema;
    while(pparam && pparam->name) {
        if((pparam->flag & SDF_REQUIRED)) {
            gbuf_printf(gbuf, " <%s>", pparam->name);
        } else {
            gbuf_printf(gbuf, " [%s='%s']", pparam->name, pparam->default_value?pparam->default_value:"?");
        }
        pparam++;
    }
    json_t *jn_usage = json_string(gbuf_cur_rd_pointer(gbuf));
    json_object_set_new(jn_it, "usage", jn_usage);
    GBUF_DECREF(gbuf);

    json_t *jn_parameters = json_array();
    json_object_set_new(jn_it, "parameters", jn_parameters);

    pparam = it->schema;
    while(pparam && pparam->name) {
        json_t *jn_param = json_object();
        json_object_set_new(jn_param, "id", json_string(pparam->name));
        json_object_update_missing_new(jn_param, itdesc2json(pparam));
        json_array_append_new(jn_parameters, jn_param);
        pparam++;
    }

    return jn_it;
}

/***************************************************************************
 *  Return a json object describing the hsdata for commands
 ***************************************************************************/
PUBLIC json_t *sdatacmd2json(
    const sdata_desc_t *items
)
{
    json_t *jn_items = json_array();
    const sdata_desc_t *it = items;
    if(!it) {
        return jn_items;
    }
    while(it->name) {
        json_t *jn_it = cmddesc2json(it);
        if(jn_it) {
            json_array_append_new(jn_items, jn_it);
        }
        it++;
    }
    return jn_items;
}

/***************************************************************************
 *  Return a json object describing the hsdata for commands
 ***************************************************************************/
PRIVATE json_t *authdesc2json(const sdata_desc_t *it)
{
    int type = it->type;

    if(!ASN_IS_SCHEMA(type)) {
        return 0; // Only schemas please
    }

    json_t *jn_it = json_object();

    json_object_set_new(jn_it, "id", json_string(it->name));

    if(it->alias) {
        json_t *jn_alias = json_array();
        json_object_set_new(jn_it, "alias", jn_alias);
        const char **alias = it->alias;
        while(*alias) {
            json_array_append_new(jn_alias, json_string(*alias));
            alias++;
        }
    }

    json_object_set_new(jn_it, "description", json_string(it->description));

    GBUFFER *gbuf = get_sdata_flag_desc(it->flag);
    if(gbuf) {
        int l = gbuf_leftbytes(gbuf);
        if(l) {
            char *pflag = gbuf_get(gbuf, l);
            json_object_set_new(jn_it, "flag", json_string(pflag));
        } else {
            json_object_set_new(jn_it, "flag", json_string(""));
        }
        gbuf_decref(gbuf);
    }

    json_t *jn_parameters = json_array();
    json_object_set_new(jn_it, "parameters", jn_parameters);

    const sdata_desc_t *pparam = it->schema;
    while(pparam && pparam->name) {
        json_t *jn_param = json_object();
        json_object_set_new(jn_param, "id", json_string(pparam->name));
        json_object_update_missing_new(jn_param, itdesc2json0(pparam));
        json_array_append_new(jn_parameters, jn_param);
        pparam++;
    }

    return jn_it;
}

/***************************************************************************
 *  Return a json object describing the hsdata for auths
 ***************************************************************************/
PUBLIC json_t *sdataauth2json(
    const sdata_desc_t *items
)
{
    json_t *jn_items = json_array();
    const sdata_desc_t *it = items;
    if(!it) {
        return jn_items;
    }
    while(it->name) {
        json_t *jn_it = authdesc2json(it);
        if(jn_it) {
            json_array_append_new(jn_items, jn_it);
        }
        it++;
    }
    return jn_items;
}

/***************************************************************************
 *  Return a json object describing the hsdata for attrs
 ***************************************************************************/
PUBLIC json_t *sdatadesc2json(
    const sdata_desc_t *items,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag
)
{
    json_t *jn_items = json_object();
    const sdata_desc_t *it = items;
    if(!it) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "sdata items is NULL",
            NULL
        );
        return jn_items;
    }
    while(it->name) {
        if(exclude_flag && (it->flag & exclude_flag)) {
            it++;
            continue;
        }
        if(include_flag == -1 || (it->flag & include_flag)) {
            json_t *jn_it = itdesc2json(it);
            json_object_set_new(jn_items, it->name, jn_it);
        }
        it++;
    }
    return jn_items;
}

/***************************************************************************
 *  Return a json array describing the hsdata for attrs
 ***************************************************************************/
PUBLIC json_t *sdatadesc2json2(
    const sdata_desc_t *items,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag
)
{
    json_t *jn_items = json_array();

    const sdata_desc_t *it = items;
    if(!it) {
        return jn_items;
    }
    while(it->name) {
        if(exclude_flag && (it->flag & exclude_flag)) {
            it++;
            continue;
        }
        if(include_flag == -1 || (it->flag & include_flag)) {
            json_t *jn_it = json_object();
            json_array_append_new(jn_items, jn_it);
            json_object_set_new(jn_it, "id", json_string(it->name));
            json_object_update_missing_new(jn_it, itdesc2json(it));
        }
        it++;
    }
    return jn_items;
}

/***************************************************************************
 *  Return a json object describing the parameter
 ***************************************************************************/
PRIVATE json_t *itdesc2json(const sdata_desc_t *it)
{
    json_t *jn_it = json_object();

    int type = it->type;

    if(ASN_IS_STRING(type)) {
        json_object_set_new(jn_it, "type", json_string("string"));
        json_object_set_new(
            jn_it,
            "default_value",
            json_string(it->default_value?it->default_value:"")
        );
    } else if(ASN_IS_JSON(type)) {
        json_object_set_new(jn_it, "type", json_string("json"));
        json_object_set_new(
            jn_it,
            "default_value",
            json_string(it->default_value?it->default_value:"")
        );
    } else if(ASN_IS_DL_LIST(type)) {
        json_object_set_new(jn_it, "type", json_string("dl_list"));
    } else if(ASN_IS_ITER(type)) {
        json_object_set_new(jn_it, "type", json_string("iter"));
    } else if(ASN_IS_POINTER(type)) {
        json_object_set_new(jn_it, "type", json_string("pointer"));
        json_object_set_new(jn_it, "default_value", json_integer((json_int_t)(size_t)it->default_value));
    } else if(ASN_IS_SIGNED32(type)) {
        json_object_set_new(jn_it, "type", json_string("signed32"));
        json_object_set_new(jn_it, "default_value", json_integer((json_int_t)(size_t)it->default_value));
    } else if(ASN_IS_UNSIGNED32(type)) {
        json_object_set_new(jn_it, "type", json_string("unsigned32"));
        json_object_set_new(jn_it, "default_value", json_integer((json_int_t)(size_t)it->default_value));
    } else if(ASN_IS_SIGNED64(type)) {
        json_object_set_new(jn_it, "type", json_string("signed64"));
        json_object_set_new(jn_it, "default_value", json_integer((json_int_t)(size_t)it->default_value));
    } else if(ASN_IS_UNSIGNED64(type)) {
        json_object_set_new(jn_it, "type", json_string("unsigned64"));
        json_object_set_new(jn_it, "default_value", json_integer((json_int_t)(size_t)it->default_value));
    } else if(ASN_IS_DOUBLE(type)) {
        json_object_set_new(jn_it, "type", json_string("double"));
        json_object_set_new(jn_it, "default_value", json_real((double)(long)(size_t)it->default_value));
    } else {
        json_object_set_new(jn_it, "type", json_string("???"));
        json_object_set_new(jn_it, "default_value", json_integer((json_int_t)(size_t)it->default_value));
    }

    json_object_set_new(jn_it, "description", json_string(it->description));
    GBUFFER *gbuf = get_sdata_flag_desc(it->flag);
    if(gbuf) {
        int l = gbuf_leftbytes(gbuf);
        if(l) {
            char *pflag = gbuf_get(gbuf, l);
            json_object_set_new(jn_it, "flag", json_string(pflag));
        } else {
            json_object_set_new(jn_it, "flag", json_string(""));
        }
        gbuf_decref(gbuf);
    }
    return jn_it;
}

/***************************************************************************
 *  Return a json object describing the parameter without default_value
 ***************************************************************************/
PRIVATE json_t *itdesc2json0(const sdata_desc_t *it)
{
    json_t *jn_it = json_object();

    int type = it->type;

    if(ASN_IS_STRING(type)) {
        json_object_set_new(jn_it, "type", json_string("string"));
    } else if(ASN_IS_JSON(type)) {
        json_object_set_new(jn_it, "type", json_string("json"));
    } else if(ASN_IS_DL_LIST(type)) {
        json_object_set_new(jn_it, "type", json_string("dl_list"));
    } else if(ASN_IS_ITER(type)) {
        json_object_set_new(jn_it, "type", json_string("iter"));
    } else if(ASN_IS_POINTER(type)) {
        json_object_set_new(jn_it, "type", json_string("pointer"));
    } else if(ASN_IS_SIGNED32(type)) {
        json_object_set_new(jn_it, "type", json_string("signed32"));
    } else if(ASN_IS_UNSIGNED32(type)) {
        json_object_set_new(jn_it, "type", json_string("unsigned32"));
    } else if(ASN_IS_SIGNED64(type)) {
        json_object_set_new(jn_it, "type", json_string("signed64"));
    } else if(ASN_IS_UNSIGNED64(type)) {
        json_object_set_new(jn_it, "type", json_string("unsigned64"));
    } else if(ASN_IS_DOUBLE(type)) {
        json_object_set_new(jn_it, "type", json_string("double"));
    } else {
        json_object_set_new(jn_it, "type", json_string("???"));
    }

    json_object_set_new(jn_it, "description", json_string(it->description));
    GBUFFER *gbuf = get_sdata_flag_desc(it->flag);
    if(gbuf) {
        int l = gbuf_leftbytes(gbuf);
        if(l) {
            char *pflag = gbuf_get(gbuf, l);
            json_object_set_new(jn_it, "flag", json_string(pflag));
        } else {
            json_object_set_new(jn_it, "flag", json_string(""));
        }
        gbuf_decref(gbuf);
    }
    return jn_it;
}

/***************************************************************************
 *  Debug
 ***************************************************************************/
PUBLIC int log_debug_sdata(const char *prefix, hsdata hs)
{
    json_t *jn = sdata2json(hs, -1, 0);
    if(jn) {
        log_debug_json(0, jn, prefix);
        JSON_DECREF(jn);
    }
    return 0;
}


/***************************************************************
 *  Debug
 ***************************************************************/
PRIVATE int cb_debug_walking(rc_instance_t *instance, rc_resource_t *resource_, void *user_data, void *user_data2, void *user_data3)
{

    log_debug_printf(0, "resource '%s', instance %d", sdata_resource(resource_), instance->__id__);
    log_debug_sdata(0, resource_);
    return 0;
}
PUBLIC int log_debug_sdata_iter(
    const char *info,
    dl_list_t *iter,
    walk_type_t walk_type,
    const char *fmt,
    ...
)
{
    va_list ap;

    va_start(ap, fmt);
    log_debug_vprintf(info, fmt, ap);
    va_end(ap);

    rc_walk_by_list(
        iter,
        walk_type,
        cb_debug_walking,
        0,
        0,
        0
    );
    return 0;
}




                /*------------------------------*
                 *      Conversion functions
                 *------------------------------*/




/***************************************************************************
 *  Convert an attribute in json
 ***************************************************************************/
PUBLIC json_t *item2json(
    hsdata hs,
    const char *name,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it = sdata_it_desc(sdata_schema(sdata), name);

    int type = it->type;
    if(ASN_IS_STRING(type)) {
        const char *s = sdata_read_str(sdata, it->name);
        s = s?s:"";
        return json_string(s);

    } else if(ASN_IS_JSON(type)) {
        json_t *jn = sdata_read_json(sdata, it->name);
        if(jn) {
            json_incref(jn);
        } else {
            return json_null();
        }
        return jn;
    } else if(ASN_IS_BOOLEAN(type)) {
        BOOL b = sdata_read_bool(sdata, it->name);
        if(b) {
            return json_true();
        } else {
            return json_false();
        }
    } else if(ASN_IS_POINTER(type)) {
        void *p = sdata_read_pointer(sdata, it->name);
        return json_integer((json_int_t)(size_t)p);
    } else if(ASN_IS_SIGNED32(type)) {
        int32_t i32 = sdata_read_int32(sdata, it->name);
        return json_integer(i32);
    } else if(ASN_IS_UNSIGNED32(type)) {
        uint32_t u32 = sdata_read_uint32(sdata, it->name);
        return json_integer(u32);
    } else if(ASN_IS_SIGNED64(type)) {
        int64_t i64 = sdata_read_int64(sdata, it->name);
        return json_integer(i64);
    } else if(ASN_IS_UNSIGNED64(type)) {
        uint64_t u64 = sdata_read_uint64(sdata, it->name);
        return json_integer(u64);
    } else if(ASN_IS_REAL_NUMBER(type)) {
        double f= sdata_read_real(sdata, it->name);
        return json_real(f);

    } else if(ASN_IS_ITER(type)) {
        json_t *jn_ids_list = json_array();
        dl_list_t *iter = sdata_read_iter(sdata, it->name);
        if(!iter) {
            return json_null();
        }
        hsdata hs_; rc_instance_t *i_hs;
        i_hs = rc_first_instance(iter, (rc_resource_t **)&hs_);
        while(i_hs) {
            if(0) { // recursive
                json_array_append_new(jn_ids_list, sdata2json(hs_, include_flag, exclude_flag));
            } else {
                json_int_t id = sdata_read_uint64(hs_, "id");
                json_array_append_new(jn_ids_list, json_integer(id));
            }
            i_hs = rc_next_instance(i_hs, (rc_resource_t **)&hs_);
        }
        return jn_ids_list;

    } else {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "ASN type CANNOT convert to json",
            "name",         "%s", it->name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return json_null();
    }
}


/***************************************************************************
 *  Return a json object with fields with flag. -1 fort all
 ***************************************************************************/
PUBLIC json_t *sdata2json(
    hsdata hsdata,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag)
{
    if(!hsdata) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "hsdata NULL",
            NULL
        );
        return 0;
    }
    SData_t *sdata = hsdata;
    const sdata_desc_t *it = sdata->items;
    if(!it) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "sdata items is NULL",
            NULL
        );
        return 0;
    }
    json_t *jn_items = json_object();
    while(it->name) {
        if(exclude_flag && (it->flag & exclude_flag)) {
            it++;
            continue;
        }
        if(include_flag == -1 || (it->flag & include_flag)) {
            json_t *jn = item2json(sdata, it->name, include_flag, exclude_flag);
            json_object_set_new(jn_items, it->name, jn);
        }
        it++;
    }
    return jn_items;
}

/***************************************************************************
 *  Convert a hsdata into a json object with items in filter.
 ***************************************************************************/
PUBLIC json_t *sdata2json3(
    hsdata hsdata,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag,
    const char **keys
)
{
    if(!hsdata) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "hsdata NULL",
            NULL
        );
        return 0;
    }

    SData_t *sdata = hsdata;

    json_t *jn_items = json_object();

    while(*keys) {
        const sdata_desc_t *it = sdata_it_desc(sdata->items, *keys);
        if(it) {
            json_t *jn = item2json(sdata, it->name, include_flag, exclude_flag);
            json_object_set_new(jn_items, it->name, jn);
        } else {
            log_error(LOG_OPT_TRACE_STACK,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_INTERNAL_ERROR,
                "msg",          "%s", "sdata item NOT FOUND",
                "item",         "%s", *keys,
                NULL
            );
            return 0;
        }
        keys++;
    }

    return jn_items;
}

/***************************************************************************
 *  Comparator
 ***************************************************************************/
PRIVATE int cmp_by_id(const void *hs1_, const void *hs2_)
{
    hsdata hs1 = * (hsdata * const *)hs1_;
    hsdata hs2 = * (hsdata * const *)hs2_;

    uint64_t id1 = sdata_read_uint64(hs1, "id");
    uint64_t id2 = sdata_read_uint64(hs2, "id");
    if(id1 == id2)
        return 0;
    return (id1 > id2)? 1:-1;
}

/***************************************************************************
 *  Sort a iterator of hsdata
 *  Return a new iter and delete the old
 ***************************************************************************/
PUBLIC dl_list_t * sdata_sort_iter_by_id(
    dl_list_t * iter  // owned
){
    hsdata *keys;
    size_t size;

    size = rc_iter_size(iter);
    if(size==0) {
        return iter;
    }
    keys = gbmem_malloc(size * sizeof(hsdata));
    if(!keys) {
        return iter;
    }

    size_t idx=0;
    hsdata hs; rc_instance_t *i_hs;
    i_hs = rc_first_instance(iter, (rc_resource_t **)&hs);
    while(i_hs) {
        *(keys + idx) = hs;
        idx++;
        i_hs = rc_next_instance(i_hs, (rc_resource_t **)&hs);
    }

    qsort(keys, size, sizeof(hsdata), cmp_by_id);

    dl_list_t * new_iter = rc_init_iter(0);
    for(int i=0; i<size; i++) {
        rc_add_instance(new_iter, *(keys + i), 0);
    }

    rc_free_iter(iter, TRUE, 0);
    gbmem_free(keys);
    return new_iter;
}

/***************************************************************************
 *  Convert a hsdata into a json object with items with flag, or all if flag is -1
 ***************************************************************************/
PUBLIC json_t * sdata_iter2json(
    dl_list_t * iter,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag)
{
    json_t *jn_items = json_array();

    if(!iter) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "iter NULL",
            NULL
        );
        return jn_items;
    }

    hsdata hs; rc_instance_t *i_hs;
    i_hs = rc_first_instance(iter, (rc_resource_t **)&hs);
    while(i_hs) {
        json_t *jn_r = sdata2json(hs, include_flag, exclude_flag);
        if(jn_r) {
            json_array_append_new(jn_items, jn_r);
        }
        i_hs = rc_next_instance(i_hs, (rc_resource_t **)&hs);
    }
    return jn_items;
}

/***************************************************************************
 *  Convert a iterator of hsdata into a json array of json objects with items in keys.
 ***************************************************************************/
PUBLIC json_t * sdata_iter2json3(
    dl_list_t * iter,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag,
    const char **keys
)
{
    json_t *jn_items = json_array();

    if(!iter) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "iter NULL",
            NULL
        );
        return jn_items;
    }

    hsdata hs; rc_instance_t *i_hs;
    i_hs = rc_first_instance(iter, (rc_resource_t **)&hs);
    while(i_hs) {
        json_t *jn_r = sdata2json3(hs, include_flag, exclude_flag, keys);
        if(jn_r) {
            json_array_append_new(jn_items, jn_r);
        }
        i_hs = rc_next_instance(i_hs, (rc_resource_t **)&hs);
    }
    return jn_items;
}

/***************************************************************************
 *  Convert json to value
 *  Return -1 if not strict conversion done.
 *  `bf` is a temporal buffer used to convert numbers in strings, 64 bytes enought.
 ***************************************************************************/
PRIVATE int json2value(char *bf, int bfsize, int type, SData_Value_t *v, json_t *jn_value)
{

    if(ASN_IS_STRING(type)) {
        if(json_is_string(jn_value)) {
            v->s = (char *)json_string_value(jn_value);
            return 0;
        } else if(json_is_integer(jn_value)) {
            snprintf(bf, bfsize, "%"JSON_INTEGER_FORMAT, json_integer_value(jn_value));
            v->s = bf;
        } else if(json_is_real(jn_value)) {
            snprintf(bf, bfsize, "%.f", json_real_value(jn_value));
            v->s = bf;
        } else if(json_is_boolean(jn_value)) {
            snprintf(bf, bfsize, "%s", json_is_true(jn_value)?"1":"0");
            v->s = bf;
        } else {
            v->s = 0;
        }
    } else if(ASN_IS_BOOLEAN(type)) {
        if(json_is_string(jn_value)) {
            const char *s = json_string_value(jn_value);
            if(strcasecmp(s, "true")==0) {
                v->b = 1;
            } else if(strcasecmp(s, "false")==0) {
                v->b = 0;
            } else {
                v->b = atoi(json_string_value(jn_value))?1:0;
            }
        } else if(json_is_integer(jn_value)) {
            v->b = json_integer_value(jn_value)?1:0;
        } else if(json_is_real(jn_value)) {
            v->b = json_real_value(jn_value)?1:0;
        } else if(json_is_boolean(jn_value)) {
            v->b = json_is_true(jn_value)?1:0;
            return 0;
        } else {
            v->b = 0;
        }
    } else if(ASN_IS_SIGNED32(type)) {
        if(json_is_string(jn_value)) {
            v->i32 = atoi(json_string_value(jn_value));
        } else if(json_is_integer(jn_value)) {
            v->i32 = (int32_t)json_integer_value(jn_value);
            return 0;
        } else if(json_is_real(jn_value)) {
            v->i32 = (int32_t)json_real_value(jn_value);
        } else if(json_is_boolean(jn_value)) {
            v->i32 = json_is_true(jn_value)?1:0;
        } else {
            v->i32 = 0;
        }
    } else if(ASN_IS_UNSIGNED32(type)) {
        if(json_is_string(jn_value)) {
            v->u32 = atoi(json_string_value(jn_value));
        } else if(json_is_integer(jn_value)) {
            v->u32 = (uint32_t)json_integer_value(jn_value);
            return 0;
        } else if(json_is_real(jn_value)) {
            v->u32 = (uint32_t)json_real_value(jn_value);
        } else if(json_is_boolean(jn_value)) {
            v->u32 = json_is_true(jn_value)?1:0;
        } else {
            v->u32 = 0;
        }
    } else if(ASN_IS_SIGNED64(type)) {
        v->i64 = (int64_t)json_integer_value(jn_value);
        if(json_is_string(jn_value)) {
            v->i64 = atoll(json_string_value(jn_value));
        } else if(json_is_integer(jn_value)) {
            v->i64 = (int64_t)json_integer_value(jn_value);
            return 0;
        } else if(json_is_real(jn_value)) {
            v->i64 = (int64_t)json_real_value(jn_value);
        } else if(json_is_boolean(jn_value)) {
            v->i64 = json_is_true(jn_value)?1:0;
        } else {
            v->i64 = 0;
        }
    } else if(ASN_IS_UNSIGNED64(type)) {
        if(json_is_string(jn_value)) {
            v->u64 = atoll(json_string_value(jn_value));
        } else if(json_is_integer(jn_value)) {
            v->u64 = (uint64_t)json_integer_value(jn_value);
            return 0;
        } else if(json_is_real(jn_value)) {
            v->u64 = (uint64_t)json_real_value(jn_value);
        } else if(json_is_boolean(jn_value)) {
            v->u64 = json_is_true(jn_value)?1:0;
        } else {
            v->u64 = 0;
        }
    } else if(ASN_IS_DOUBLE(type)) {
        if(json_is_string(jn_value)) {
            v->f = atof(json_string_value(jn_value));
        } else if(json_is_integer(jn_value)) {
            v->f = (double)json_integer_value(jn_value);
        } else if(json_is_real(jn_value)) {
            v->f = json_real_value(jn_value);
            return 0;
        } else if(json_is_boolean(jn_value)) {
            v->f = json_is_true(jn_value)?1.0:0.0;
        } else {
            v->f = 0.0;
        }
    } else if(ASN_IS_JSON(type)) {
        v->j = jn_value;
    } else if(ASN_IS_POINTER(type)) {
        v->p = (void *)(size_t)json_integer_value(jn_value);
    } else {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "ASN type NOT ALLOWED",
            "type",         "%d", type,
            NULL
        );
        return -1;
    }
    return 0;
}

/***************************************************************************
 *  Convert json to item
 ***************************************************************************/
PUBLIC int json2item(
    hsdata hsdata,
    const char *name,
    json_t *jn_value) // not owned
{
    char temp[64];
    SData_t *sdata = hsdata;
    const sdata_desc_t *it = sdata_it_desc(sdata->items, name);
    if(!it) {
        return -1;
    }
    void *ptr = item_pointer(sdata, it);
    if(!ptr) {
        // Error already logged
        return -1;
    }

    /*
     *  Complex types
     */
    if(ASN_IS_ITER(it->type)) {
        if(sdata_iter_load_from_dict_list(ptr, it->schema, jn_value, 0, 0, 0)<0) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_JSON_ERROR,
                "msg",          "%s", "sdata_iter_load_from_dict_list() FAILED",
                "name",         "%s", name,
                NULL);
            return -1;
        }
    } else {
        /*
         *  Simple types
         */
        SData_Value_t v;
        memset(&v, 0, sizeof(SData_Value_t));
        if(json2value(temp, sizeof(temp), it->type, &v, jn_value)<0) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_PARAMETER_ERROR,
                "msg",          "%s", "json2value() FAILED",
                "name",         "%s", name,
                NULL
            );
        }
        sdata_write_by_type(sdata, it, ptr, v);
    }

    return 0;
}

/***************************************************************************
 *  Update sdata fields from a json dict.
 *  Constraint: if flag is -1
 *                  => all fields!
 *              else
 *                  => matched flag
 ***************************************************************************/
PUBLIC int json2sdata(
    hsdata hsdata,
    json_t *kw,  // not owned
    sdata_flag_t flag,
    not_found_cb_t not_found_cb, // Called when the key not exist in hsdata
    void *user_data)
{
    int ret = 0;
    SData_t *sdata = hsdata;
    if(!sdata || !sdata->items || !kw) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "hsdata or schema or kw NULL",
            NULL
        );
        return -1;
    }
    const char *key;
    json_t *jn_value;
    json_object_foreach(kw, key, jn_value) {
        const sdata_desc_t *it = sdata_it_desc(sdata->items, key);
        if(!it) {
            if(not_found_cb) {
                not_found_cb(user_data, key);
                ret--;
            }
            continue;
        }
        if(!(flag == -1 || (it->flag & flag))) {
            continue;
        }
        ret += json2item(hsdata, it->name, jn_value);
    }

    return ret;
}

/***************************************************************************
 *  Like json2sdata but in all sdata of iter
 ***************************************************************************/
PUBLIC int json2sdata_iter(
    dl_list_t *iter,
    json_t *kw,  // not owned
    sdata_flag_t flag,
    not_found_cb_t not_found_cb, // Called when the key not exist in hsdata
    void *user_data)
{
    if(!iter || !kw) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "iter or kw NULL",
            NULL
        );
        return -1;
    }
    hsdata hs; rc_instance_t *i_hs;
    i_hs = rc_first_instance(iter, (rc_resource_t **)&hs);
    while(i_hs) {
        json2sdata(hs, kw, flag, not_found_cb, user_data);
        i_hs = rc_next_instance(i_hs, (rc_resource_t **)&hs);
    }
    return 0;
}

/***************************************************************************
 *  Convert a SData_Value_t value into a string
 *  to let certain values be string "keys"
 ***************************************************************************/
PUBLIC int svalue2str(const sdata_desc_t *it, char *bf, int bflen, SData_Value_t v) // HIDE by the moment
{
    int type = it->type;

    if(ASN_IS_STRING(type)) {
        snprintf(bf, bflen, "%s", v.s);
    } else if(ASN_IS_SIGNED32(type)) {
        snprintf(bf, bflen, "%d", v.i32);
    } else if(ASN_IS_UNSIGNED32(type)) {
        snprintf(bf, bflen, "%u", v.u32);
    } else if(ASN_IS_SIGNED64(type)) {
        snprintf(bf, bflen, "%" PRId64, v.i64);
    } else if(ASN_IS_UNSIGNED64(type)) {
        snprintf(bf, bflen, "%"PRIu64, v.u64);
    } else if(type == ASN_FLOAT) {
        snprintf(bf, bflen, "%f", v.f);
    } else if(type == ASN_DOUBLE) {
        snprintf(bf, bflen, "%f", v.f);
    } else {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "ASN type NOT ALLOWED",
            "name",         "%s", it->name,
            "type",         "%d", type,
            NULL
        );
        return -1;
    }
    return 0;
}




                /*---------------------------------*
                 *      Search/Match functions
                 *---------------------------------*/




/***************************************************************************
 *  Get sdata item marked as SDF_PKEY, traversing sdata items
 ***************************************************************************/
PRIVATE const sdata_desc_t *_pkey_item(const sdata_desc_t *schema)
{
    if(!schema) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "schema is NULL",
            NULL
        );
        return 0;
    }
    const sdata_desc_t *it = schema;
    while(it->name) {
        if(it->flag & SDF_PKEY) {
            return it;
        }
        it++;
    }
    log_error(LOG_OPT_TRACE_STACK,
        "gobj",         "%s", __FILE__,
        "function",     "%s", __FUNCTION__,
        "msgset",       "%s", MSGSET_INTERNAL_ERROR,
        "msg",          "%s", "sdata SDF_PKEY item NOT FOUND",
        NULL
    );
    return 0;
}

/***************************************************************************
 *  Get the value and item of the primary key field of the row
 ***************************************************************************/
PRIVATE const sdata_desc_t * _row_pkey(
    hsdata hs,
    SData_Value_t *v)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it = _pkey_item(sdata->items);
    if(!it) {
        // Error already logged
        return 0;
    }
    void *ptr = sdata_it_pointer(hs, it->name, 0);
    if(v) {
        *v = sdata_read_by_type(hs, it, ptr);
    }
    return it;
}

/***************************************************************************
 *  Compare two sdata values
 ***************************************************************************/
PRIVATE int _sdata_compare_value(
    const sdata_desc_t *it,
    SData_Value_t *v,
    const char *pkey)
{
    int type = it->type;

    if(ASN_IS_STRING(type)) {
        return strcasecmp(v->s, pkey);
    } else if(ASN_IS_SIGNED32(type)) {
        return (v->i32==atoi(pkey))?0:-1; // TODO return like strcmp
    } else if(ASN_IS_UNSIGNED32(type)) {
        return (v->u32==atoi(pkey))?0:-1; // TODO return like strcmp
    } else if(ASN_IS_SIGNED64(type)) {
        return (v->i64==atoi(pkey))?0:-1; // TODO return like strcmp
    } else if(ASN_IS_UNSIGNED64(type)) {
        return (v->u64==atoi(pkey))?0:-1; // TODO return like strcmp
    } else if(type == ASN_FLOAT) {
        return (v->f==atof(pkey))?0:-1; // TODO return like strcmp
    } else if(type == ASN_DOUBLE) {
        return (v->f==atof(pkey))?0:-1; // TODO return like strcmp
    } else {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "ASN type NOT ALLOWED",
            "name",         "%s", it->name,
            "type",         "%d", type,
            NULL
        );
        return -1;
    }
}

/***************************************************************************
 *  Search and return the row of the `ht` table with a `v` primary key
 ***************************************************************************/
PUBLIC hsdata sdata_iter_search_by_pkey(dl_list_t *iter, const char *pkey)
{
    hsdata hs; rc_instance_t *i_hs;
    i_hs = rc_first_instance(iter, (rc_resource_t **)&hs);
    while(i_hs) {
        SData_Value_t v;
        const sdata_desc_t *it;
        it = _row_pkey(hs, &v);
        if(it) {
            if(_sdata_compare_value(it, &v, pkey)==0) {
                return hs;
            }
        }

        i_hs = rc_next_instance(i_hs, (rc_resource_t **)&hs);
    }
    return 0;
}

/***************************************************************************
 *  Find the first resource matching the filter
 ***************************************************************************/
PUBLIC hsdata sdata_iter_find_one(
    dl_list_t * iter,
    json_t *jn_filter,  // owned
    rc_instance_t** i_hs_
)
{
    if(i_hs_) {
        *i_hs_ = 0;
    }
    hsdata hs; rc_instance_t *i_hs;
    i_hs = rc_first_instance(iter, (rc_resource_t **)&hs);
    while(i_hs) {
        json_t *jn_data = sdata2json(hs, -1, 0);
        JSON_INCREF(jn_filter);
        if(kw_match_simple(jn_data, jn_filter)) {
            if(i_hs_) {
                *i_hs_ = i_hs;
            }
            JSON_DECREF(jn_filter);
            JSON_DECREF(jn_data);
            return hs;
        }
        JSON_DECREF(jn_data);
        i_hs = rc_next_instance(i_hs, (rc_resource_t **)&hs);
    }
    JSON_DECREF(jn_filter);
    return 0;
}

/***************************************************************************
 *  Return TRUE if all keys in jn_filter dict match with sdata's key
 *  Only compare str/int/real/bool items
 *  If match_fn is null then kw_match_simple() is used
 ***************************************************************************/
PUBLIC BOOL sdata_match(
    hsdata hs,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag,
    json_t *jn_filter, // owned
    BOOL (*match_fn) (
        json_t *kw,         // not owned
        json_t *jn_filter   // owned
    )
)
{
    if(!match_fn) {
        match_fn = kw_match_simple;
    }

    json_t *kw = sdata2json(hs, include_flag, exclude_flag);
    BOOL matched = match_fn(
        kw,         // not owned
        jn_filter   // owned
    );
    JSON_DECREF(kw);
    return matched;
}

/***************************************************************************
 *  Return iter with matched resources
 * (free return with with rc_free_iter(iter, TRUE, 0);)
 *  If match_fn is null then kw_match_simple() is used
 ***************************************************************************/
PUBLIC dl_list_t * sdata_iter_match(
    dl_list_t * iter,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag,
    json_t *jn_filter,  // owned
    BOOL (*match_fn) (
        json_t *kw,         // not owned
        json_t *jn_filter   // owned
    )
)
{
    if(!match_fn) {
        match_fn = kw_match_simple;
    }
    // Return always an iter, although empty.
    dl_list_t *user_iter = rc_init_iter(0);

    hsdata hs; rc_instance_t *i_hs;
    i_hs = rc_first_instance(iter, (rc_resource_t **)&hs);
    while(i_hs) {
        JSON_INCREF(jn_filter);
        if(sdata_match(
            hs,
            include_flag,
            exclude_flag,
            jn_filter, // owned
            match_fn)
        ) {
            rc_add_instance(user_iter, hs, 0);
        }

        i_hs = rc_next_instance(i_hs, (rc_resource_t **)&hs);
    }
    JSON_DECREF(jn_filter);
    return user_iter;
}




                /*-------------------------------*
                 *      Load/save persistent
                 *-------------------------------*/




/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int sdata_load_persistent(hsdata hs, const char *path)
{
    if(access(path, 0)!=0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "writable-persistent attrs file NOT FOUND",
            "path",         "%s", path,
            NULL
        );
        return -1;
    }

    size_t flags = 0;
    json_error_t error;
    json_t *jn_dict = json_load_file(path, flags, &error);
    if(!jn_dict) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_JSON_ERROR,
            "msg",          "%s", "JSON data INVALID",
            "line",         "%d", error.line,
            "column",       "%d", error.column,
            "position",     "%d", error.position,
            "json",         "%s", error.text,
            "path",         "%s", path,
            NULL
        );
        return -1;
    }

    json2sdata(hs, jn_dict, SDF_PERSIST, 0, 0);

    JSON_DECREF(jn_dict);
    return 0;
}

/***************************************************************************
 *
 ***************************************************************************/
PUBLIC int sdata_save_persistent(hsdata hs, const char *path, int rpermission)
{
    SData_t *sdata = hs;
    json_t *jn_dict = sdata2json(sdata, SDF_PERSIST, 0);
    if(json_object_size(jn_dict) > 0) {
        size_t flags = JSON_INDENT(4);
        int ret = json_dump_file(jn_dict, path, flags);
        if(ret < 0) {
            log_error(0,
                "gobj",         "%s", __FILE__,
                "function",     "%s", __FUNCTION__,
                "msgset",       "%s", MSGSET_JSON_ERROR,
                "msg",          "%s", "json_dump_file() FAILED",
                "path",         "%s", path,
                NULL
            );
            JSON_DECREF(jn_dict);
            return -1;
        }
    }
    JSON_DECREF(jn_dict);
    return 0;
}

/***************************************************************************
 *  Load the SDF_PERSISTENT fields from json file
 ***************************************************************************/
PUBLIC int sdata_iter_load_persistent(
    dl_list_t *iter,
    const char *path,
    not_found_cb_t not_found_cb, // Called when the key not exist in hsdata
    void *user_data
)
{
    if(access(path, 0)!=0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "writable-persistent iter file NOT FOUND",
            "path",         "%s", path,
            NULL
        );
        return -1;
    }

    size_t flags = 0;
    json_error_t error;
    json_t *jn_dict = json_load_file(path, flags, &error);
    if(!jn_dict) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_JSON_ERROR,
            "msg",          "%s", "JSON data INVALID",
            "line",         "%d", error.line,
            "column",       "%d", error.column,
            "position",     "%d", error.position,
            "json",         "%s", error.text,
            "path",         "%s", path,
            NULL
        );
        return -1;
    }

    const char *pkey;
    json_t *value;
    json_object_foreach(jn_dict, pkey, value) {
        hsdata hsrow = sdata_iter_search_by_pkey(iter, pkey);
        if(!hsrow) {
            continue;
        }
        json2sdata(hsrow, value, SDF_PERSIST, not_found_cb, user_data);
    }

    JSON_DECREF(jn_dict);
    return 0;
}

/***************************************************************************
 *  Save the SDF_PERSISTENT fields to json file
 *  in a dictionary using the SDF_PKEY field as key.
 ***************************************************************************/
PUBLIC int sdata_iter_save_persistent(dl_list_t *iter, const char *path, int rpermission)
{
    json_t *jn_dict = json_object();

    hsdata hs; rc_instance_t *i_hs;
    i_hs = rc_first_instance(iter, (rc_resource_t **)&hs);
    while(i_hs) {
        SData_Value_t v;
        const sdata_desc_t *it = _row_pkey(hs, &v);
        if(it) {
            json_t *jn_persistent_items = sdata2json(hs, SDF_PERSIST, 0);
            if(jn_persistent_items) {
                char skey[128];
                if(svalue2str(it, skey, sizeof(skey), v)==0) {
                    json_object_set_new(jn_dict, skey, jn_persistent_items);
                } else {
                    JSON_DECREF(jn_persistent_items);
                }
            }
        }

        i_hs = rc_next_instance(i_hs, (rc_resource_t **)&hs);
    }

    size_t flags = JSON_INDENT(4); // JSON_SORT_KEYS |
    int ret = json_dump_file(jn_dict, path, flags);
    if(ret < 0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_JSON_ERROR,
            "msg",          "%s", "json_dump_file() FAILED",
            "path",         "%s", path,
            NULL
        );
    }
    JSON_DECREF(jn_dict);
    return ret;
}

/***************************************************************************
 *  Load a row from json dictionary
 *  This function is NOT to work with PERSISTENT fields, instead,
 *  it will fill the table with all found fields in the json list's dictionaries.
 *  OLD table_load_row_from_json()
 ***************************************************************************/
PRIVATE hsdata iter_load_from_dict(
    dl_list_t * iter,
    sdata_desc_t *schema,
    json_t *kw,  // not owned
    not_found_cb_t not_found_cb, // Called when the key not exist in hsdata
    void *user_data)
{
    hsdata hsrow = sdata_create(schema, 0,0,0,0,0);
    if(!hsrow) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_INTERNAL_ERROR,
            "msg",          "%s", "table_create_row() FAILED",
            NULL
        );
        return 0;
    }
    json2sdata(
        hsrow,
        kw,
        -1,
        0,
        0
    );
    rc_add_instance(iter, hsrow, 0);

    /*
     *  Check required attributes.
     */
    sdata_check_required_attrs(hsrow, not_found_cb, user_data);
    return hsrow;
}

/***************************************************************************
 *  Load a table from json list of dictionaries
 *  This function is NOT to work with PERSISTENT fields, instead,
 *  it will fill the table with all found fields in the json list's dictionaries.
 *  OLD table_load_rows_from_json()
 ***************************************************************************/
PUBLIC int sdata_iter_load_from_dict_list(
    dl_list_t * iter,
    sdata_desc_t *schema,
    json_t *jn_list,  // not owned
    not_found_cb_t not_found_cb, // Called when the key not exist in hsdata
    int (*cb)(void *user_data, hsdata hs),
    void *user_data)
{
    size_t index;
    json_t *jn_dict;
    json_array_foreach(jn_list, index, jn_dict) {
        hsdata hs = iter_load_from_dict(iter, schema, jn_dict, not_found_cb, user_data);
        if(cb) {
            (cb)(user_data, hs);
        }
    }
    return 0;
}




                /*----------------------------*
                 *      Read functions
                 *---------------------------*/




/***************************************************************************
 *  READ - high level - string
 ***************************************************************************/
PUBLIC const char *sdata_read_str(hsdata hs, const char *name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!ASN_IS_STRING(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT STRING",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return 0;
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.s;
}

/***************************************************************************
 *  READ - high level - bool
 ***************************************************************************/
PUBLIC BOOL sdata_read_bool(hsdata hs, const char *name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!ASN_IS_BOOLEAN(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT BOOLEAN",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        // If error is by the sign then continue
        if(!ASN_IS_NATURAL_NUMBER(it->type)) {
            return 0;
        }
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.b;
}

/***************************************************************************
 *  READ - high level - int
 ***************************************************************************/
PUBLIC int32_t sdata_read_int32(hsdata hs, const char *name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!ASN_IS_SIGNED32(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT SIGNED32",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        // If error is by the sign then continue
        if(!ASN_IS_NATURAL_NUMBER(it->type)) {
            return 0;
        }
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.i32;
}

/***************************************************************************
 *  READ - high level - int
 ***************************************************************************/
PUBLIC uint32_t sdata_read_uint32(hsdata hs, const char *name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!ASN_IS_UNSIGNED32(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT UNSIGNED32",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        // If error is by the sign then continue
        if(!ASN_IS_NATURAL_NUMBER(it->type)) {
            return 0;
        }
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.u32;
}

/***************************************************************************
 *  READ - high level - int
 ***************************************************************************/
PUBLIC int64_t sdata_read_int64(hsdata hs, const char *name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!ASN_IS_SIGNED64(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT SIGNED64",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        // If error is by the sign then continue
        if(!ASN_IS_NATURAL_NUMBER(it->type)) {
            return 0;
        }
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.i64;
}

/***************************************************************************
 *  READ - high level - int
 ***************************************************************************/
PUBLIC uint64_t sdata_read_uint64(hsdata hs, const char *name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!ASN_IS_UNSIGNED64(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT UNSIGNED64",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        // If error is by the sign then continue
        if(!ASN_IS_NATURAL_NUMBER(it->type)) {
            return 0;
        }
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.u64;
}

/***************************************************************************
 *  READ - high level - int
 ***************************************************************************/
PUBLIC uint64_t sdata_read_integer(hsdata hs, const char *name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!ASN_IS_NATURAL_NUMBER(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT NATURAL NUMBER",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return 0;
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.u64;
}

/***************************************************************************
 *  READ - high level - real
 ***************************************************************************/
PUBLIC double sdata_read_real(hsdata hs, const char *name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!ASN_IS_REAL_NUMBER(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT DOUBLE",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return 0;
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.f;
}

/***************************************************************************
 *  READ - high level - json
 ***************************************************************************/
PUBLIC json_t *sdata_read_json(hsdata hs, const char *name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!ASN_IS_JSON(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT JSON",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return 0;
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.j;
}

/***************************************************************************
 *  READ - high level - pointer
 ***************************************************************************/
PUBLIC void *sdata_read_pointer(hsdata hs, const char *name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!ASN_IS_POINTER(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT POINTER",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        // If error is by the sign then continue
        if(!ASN_IS_NUMBER64(it->type)) {
            return 0;
        }
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.p;
}

/***************************************************************************
 *  READ - high level - dl_list (external use)
 ***************************************************************************/
PUBLIC dl_list_t *sdata_read_dl_list(hsdata hs, const char* name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!ASN_IS_DL_LIST(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT DL_LIST",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return 0;
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.n;
}

/***************************************************************************
 *  READ - high level - resource (external use)
 ***************************************************************************/
PUBLIC dl_list_t *sdata_read_iter(hsdata hs, const char* name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return 0;
    }
    if(!(ASN_IS_ITER(it->type))) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT RESOURCE nor ITER",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return 0;
    }
    SData_Value_t value = sdata_read_by_type(hs, it, ptr);
    return value.r;
}

/***************************************************************************
 *  READ - high level - schema
 ***************************************************************************/
PUBLIC const sdata_desc_t *sdata_read_schema(hsdata hs, const char* name)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it = sdata_it_desc(sdata->items, name);
    if(!(ASN_IS_ITER(it->type) || ASN_IS_SCHEMA(it->type))) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT ITER or RESOURCE or SCHEMA",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return 0;
    }
    return it->schema;
}

/***************************************************************************
 *  READ - high level - default value
 ***************************************************************************/
SData_Value_t sdata_get_default_value(hsdata hs, const char* name)
{
    SData_t *sdata = hs;
    SData_Value_t v = {0};
    const sdata_desc_t *it = sdata_it_desc(sdata->items, name);
    if(!it) {
        return v;
    }
    void *value = it->default_value;

    if(ASN_IS_STRING(it->type)) {
        v.s = value;
    } else if(ASN_IS_JSON(it->type)) {
        v.j = value;
    } else if(ASN_IS_POINTER(it->type)) {
        v.p = value;
    } else if(ASN_IS_UNSIGNED32(it->type)) {
        v.u32 = (uint32_t)(size_t)value;
    } else if(ASN_IS_UNSIGNED64(it->type)) {
        v.u64 = (uint64_t)(size_t)value;
    } else if(ASN_IS_SIGNED32(it->type)) {
        v.i32 = (int32_t)(size_t)value;
    } else if(ASN_IS_SIGNED64(it->type)) {
        v.i64 = (int64_t)(size_t)value;
    } else if(it->type == ASN_FLOAT) {
        v.f = (double)(size_t)value;
    } else if(it->type == ASN_DOUBLE) {
        v.f = (double)(size_t)value;
    }
    return v;
}




                /*----------------------------*
                 *      Write functions
                 *---------------------------*/




/***************************************************************************
 *  WRITE - high level -
 ***************************************************************************/
PUBLIC int sdata_write_str(hsdata hs, const char *name, const char *value)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return -1;
    }
    if(!ASN_IS_STRING(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT STRING",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return -1;
    }
    SData_Value_t v;
    v.s = (char *)value;
    int ret = sdata_write_by_type(hs, it, ptr, v);
    if(ret<0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata_write_by_types() FAILED",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
    }
    return ret;
}

/***************************************************************************
 *  WRITE - high level -
 ***************************************************************************/
PUBLIC int sdata_write_bool(hsdata hs, const char *name, BOOL value)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return -1;
    }
    if(!ASN_IS_BOOLEAN(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT BOOLEAN",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return -1;
    }
    SData_Value_t v;
    v.b = value;
    int ret = sdata_write_by_type(hs, it, ptr, v);
    if(ret<0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata_write_by_types() FAILED",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
    }
    return ret;
}

/***************************************************************************
 *  WRITE - high level -
 ***************************************************************************/
PUBLIC int sdata_write_int32(hsdata hs, const char *name, int32_t value)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return -1;
    }
    if(!ASN_IS_SIGNED32(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT SIGNED32",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return -1;
    }
    SData_Value_t v;
    v.i32 = value;
    int ret = sdata_write_by_type(hs, it, ptr, v);
    if(ret<0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata_write_by_types() FAILED",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
    }
    return ret;
}

/***************************************************************************
 *  WRITE - high level -
 ***************************************************************************/
PUBLIC int sdata_write_uint32(hsdata hs, const char *name, uint32_t value)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return -1;
    }
    if(!ASN_IS_UNSIGNED32(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT UNSIGNED32",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return -1;
    }
    SData_Value_t v;
    v.u32 = value;
    int ret = sdata_write_by_type(hs, it, ptr, v);
    if(ret<0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata_write_by_types() FAILED",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
    }
    return ret;
}

/***************************************************************************
 *  WRITE - high level -
 ***************************************************************************/
PUBLIC int sdata_write_int64(hsdata hs, const char *name, int64_t value)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return -1;
    }
    if(!ASN_IS_SIGNED64(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT SIGNED64",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return -1;
    }
    SData_Value_t v;
    v.i64 = value;
    int ret = sdata_write_by_type(hs, it, ptr, v);
    if(ret<0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata_write_by_types() FAILED",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
    }
    return ret;
}

/***************************************************************************
 *  WRITE - high level -
 ***************************************************************************/
PUBLIC int sdata_write_uint64(hsdata hs, const char *name, uint64_t value)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return -1;
    }
    if(!ASN_IS_UNSIGNED64(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT UNSIGNED64",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return -1;
    }
    SData_Value_t v;
    v.u64 = value;
    int ret = sdata_write_by_type(hs, it, ptr, v);
    if(ret<0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata_write_by_types() FAILED",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
    }
    return ret;
}

/***************************************************************************
 *  WRITE - high level -
 ***************************************************************************/
PUBLIC int sdata_write_integer(hsdata hs, const char *name, uint64_t value)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return -1;
    }
    SData_Value_t v;
    v.u64 = value;
    int ret = sdata_write_by_type(hs, it, ptr, v);
    if(ret<0) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata_write_by_types() FAILED",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
    }
    return ret;
}

/***************************************************************************
 *  WRITE - high level -
 ***************************************************************************/
PUBLIC int sdata_write_real(hsdata hs, const char *name, double value)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return -1;
    }
    if(!ASN_IS_DOUBLE(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT DOUBLE",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return -1;
    }
    SData_Value_t v;
    v.f = value;
    int ret = sdata_write_by_type(hs, it, ptr, v);
    if(ret<0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata_write_by_types() FAILED",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
    }
    return ret;
}

/***************************************************************************
 *  WRITE - high level - WARNING json is incref
 ***************************************************************************/
PUBLIC int sdata_write_json(hsdata hs, const char *name, json_t *value)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return -1;
    }
    if(!ASN_IS_JSON(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT JSON",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return -1;
    }
    SData_Value_t v;
    v.j = value;
    int ret = sdata_write_by_type(hs, it, ptr, v);
    if(ret<0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata_write_by_types() FAILED",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
    }
    return ret;
}

/***************************************************************************
 *  WRITE - high level -
 ***************************************************************************/
PUBLIC int sdata_write_pointer(hsdata hs, const char *name, void *value)
{
    SData_t *sdata = hs;
    const sdata_desc_t *it;
    void *ptr = sdata_it_pointer(sdata, name, &it);
    if(!ptr) {
        // error already logged.
        return -1;
    }
    if(!ASN_IS_POINTER(it->type)) {
        log_error(LOG_OPT_TRACE_STACK,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata type IS NOT POINTER",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
        return -1;
    }
    SData_Value_t v;
    v.p = value;
    int ret = sdata_write_by_type(hs, it, ptr, v);
    if(ret<0) {
        log_error(0,
            "gobj",         "%s", __FILE__,
            "function",     "%s", __FUNCTION__,
            "msgset",       "%s", MSGSET_PARAMETER_ERROR,
            "msg",          "%s", "sdata_write_by_types() FAILED",
            "name",         "%s", name,
            "type",         "%s", sdata_type_name(it->type),
            NULL
        );
    }
    return ret;
}


