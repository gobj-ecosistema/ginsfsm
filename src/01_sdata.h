/****************************************************************************
 *              SDATA.H
 *              Structured Data
 *
 *  Features:
 *      - Data can be located by:
 *          * `name`
 *          * `index`
 *      - Data can be retrieved by:
 *          * `reference`
 *
 *  The internal buffer has inside all variables with his size
 *  BUT, in case of the :
 *      - STRINGS types: the pointer to an allocated memory is saved.
 *          When writing a string value, the previous string gbmem is freed,
 *          and the new value is a gbmem_strdup() of the value.
 *
 *      - JSON variables: the pointer to a json structure.
 *          When writing a json value, the previous json are json_decr(),
 *          and the new value are json_incr()
 *
 *      - numbers: save as it. Distinguish between u/int32, u/int64, float and double.
 *
 *  The basic types are managed by Yuneta.
 *  You don't warry about free them (strings and numbers).
 *  In private types it's different.
 *  In some private cases you are the unique responsable for free them.
 *  My private types:
 *  ASN_JSON        Json, you know. Yuneta will free it.
 *  ASN_SCHEMA      sdata_desc_t for command parameters. Used only static data.
 *  ASN_POINTER     It's your responsability manage this.
 *  ASN_DL_LIST     Supply a free function and Yuneta will free it.
 *  ASN_ITER        Supply a free function and Yuneta will free it.
 *
 *              Copyright (c) 2013-2016 Niyamaka.
 *              All Rights Reserved.
 ****************************************************************************/

#ifndef _SDATA_H
#define _SDATA_H 1

#include <stdint.h>
#include <ghelpers.h>
#include "00_msglog_ginsfsm.h"

#ifdef __cplusplus
extern "C"{
#endif

/*********************************************************************
 *      Macros
 *********************************************************************/

/*********************************************************************
 *      Constants
 *********************************************************************/
typedef int32_t         TYPE_ASN_INTEGER;
typedef uint32_t        TYPE_ASN_COUNTER;
typedef uint32_t        TYPE_ASN_UNSIGNED;
typedef uint32_t        TYPE_ASN_TIMETICKS;
typedef char *          TYPE_ASN_OCTET_STR;
typedef char *          TYPE_ASN_IPADDRESS;
typedef int32_t         TYPE_ASN_BOOLEAN;
typedef uint64_t        TYPE_ASN_COUNTER64;
typedef float           TYPE_ASN_FLOAT;
typedef double          TYPE_ASN_DOUBLE;
typedef int64_t         TYPE_ASN_INTEGER64;
typedef uint64_t        TYPE_ASN_UNSIGNED64;
typedef json_t *        TYPE_ASN_JSON;      // Not for snmp
typedef dl_list_t       TYPE_DL_LIST;       // Not for snmp
typedef void *          TYPE_POINTER;       // Not for snmp
typedef void *          TYPE_STRUCT;        // Not for snmp
typedef dl_list_t       TYPE_RESOURCE;      // Not for snmp
typedef dl_list_t       TYPE_ITER;          // Not for snmp

/*
 *  SData field flags.
 */
typedef enum {   // HACK strict ascendent value!, strings in sdata_flag_names[]
    SDF_NOTACCESS   = 0x00000001,   /* snmp/db exposed as not accessible */
    SDF_RD          = 0x00000002,   /* Field only readable by user */
    SDF_WR          = 0x00000004,   /* Field writable (an readable) by user */
    SDF_REQUIRED    = 0x00000008,   /* Required attribute. Must not be null */
    SDF_PERSIST     = 0x00000010,   /* (implicit SDF_WR) Field must be loaded/saved HACK 7-Feb-2020 */
    SDF_VOLATIL     = 0x00000020,   /* (implicit SDF_RD) Field must not be loaded/saved HACK 7-Feb-2020 */
    SDF_RESOURCE    = 0x00000040,   /* Mark as resource.  Use `schema` to specify the sdata schema */
    SDF_PKEY        = 0x00000080,   /* field used as primary key */
    SDF_PURECHILD   = 0x00000100,   /* Pure child, unique child (For n-1 relation) */
    SDF_PARENTID    = 0x00000200,   /* Field with parent_id in a pure child record */
    SDF_WILD_CMD    = 0x00000400,   /* Command with wild options (no checked) */
    SDF_STATS       = 0x00000800,   /* (implicit SDF_RD) Field with stats (METADATA)*/
    SDF_FKEY        = 0x00001000,   /* Foreign key (no pure child) */
    SDF_RSTATS      = 0x00002000,   /* Field with resettable stats, implicitly SDF_STATS */
    SDF_PSTATS      = 0x00004000,   /* Field with persistent stats, implicitly SDF_STATS */
    SDF_AUTHZ_R     = 0x00008000,   /* Need Attribute '__read_attribute__' authorization */
    SDF_AUTHZ_W     = 0x00010000,   /* Need Attribute '__write_attribute__' authorization */
    SDF_AUTHZ_X     = 0x00020000,   /* Need Command '__execute_command__' authorization */
    SDF_AUTHZ_P     = 0x00040000,   /* authorization constraint parameter */
} sdata_flag_t;


#define SDATA_END()                                     \
{                                                       \
    .type=0,                                            \
    .name=0,                                            \
    .alias=0,                                           \
    .json_fn=0,                                         \
    .flag=0,                                            \
    .default_value=0,                                   \
    .description=0,                                     \
    .resource=0,                                        \
    .header=0,                                          \
    .fillsp=0,                                          \
    .schema=0,                                          \
    .free_fn=0,                                         \
    .__acl__=0,                                         \
    ._offset=0, ._ln=0, ._suboid=0                      \
}

/*
 *  GObj Attribute
 */
/*-ATTR-type------------name----------------flag------------------------default---------description----------*/
#define SDATA(type_, name_, flag_, default_value_, description_) \
{                                                       \
    .type=type_,                                        \
    .name=name_,                                        \
    .alias=0,                                           \
    .json_fn=0,                                         \
    .flag=flag_,                                        \
    .default_value=(void *)(size_t)(default_value_),    \
    .description=description_,                          \
    .resource=0,                                        \
    .header=0,                                          \
    .fillsp=0,                                          \
    .schema=0,                                          \
    .free_fn=0,                                         \
    .__acl__=0,                                         \
    ._offset=0, ._ln=0, ._suboid=0                      \
}

/*
 *  Database
 */
/*-DB----type-----------name----------------flag------------------------schema----------free_fn---------header-----------*/
#define SDATADB(type_, name_, flag_, schema_, free_fn_, header_)  \
{                                                       \
    .type=type_,                                        \
    .name=name_,                                        \
    .alias=0,                                           \
    .json_fn=0,                                         \
    .flag=flag_,                                        \
    .default_value=0,                                   \
    .description=0,                                     \
    .resource=0,                                        \
    .header=header_,                                    \
    .fillsp=0,                                          \
    .schema=schema_,                                    \
    .free_fn=free_fn_,                                  \
    .__acl__=0,                                         \
    ._offset=0, ._ln=0, ._suboid=0                      \
}

/*
 *  Database Field
 */
/*-FIELD-type-----------name----------------flag------------------------resource--------header----------fillsp--description---------*/
#define SDATADF(type_, name_, flag_, resource_, header_, fillsp_,description_)  \
{                                                       \
    .type=type_,                                        \
    .name=name_,                                        \
    .alias=0,                                           \
    .json_fn=0,                                         \
    .flag=flag_,                                        \
    .default_value=0,                                   \
    .description=description_,                          \
    .resource=resource_,                                \
    .header=header_,                                    \
    .fillsp=fillsp_,                                    \
    .schema=0,                                          \
    .free_fn=0,                                         \
    .__acl__=0,                                         \
    ._offset=0, ._ln=0, ._suboid=0                      \
}

/*
 *  Database child
 */
/*-CHILD-type-----------name----------------flag------------------------resource------------free_fn---------header--------------fillsp---description--*/
#define SDATADC(type_, name_, flag_, resource_, free_fn_, header_, fillsp_, description_)  \
{                                                       \
    .type=type_,                                        \
    .name=name_,                                        \
    .alias=0,                                           \
    .json_fn=0,                                         \
    .flag=flag_,                                        \
    .default_value=0,                                   \
    .description=description_,                          \
    .resource=resource_,                                \
    .header=header_,                                    \
    .fillsp=fillsp_,                                    \
    .schema=0,                                          \
    .free_fn=free_fn_,                                  \
    .__acl__=0,                                         \
    ._offset=0, ._ln=0, ._suboid=0                      \
}

/*-CMD---type-----------name----------------alias---------------items-----------json_fn---------description---------- */
#define SDATACM(type_, name_, alias_, items_, json_fn_, description_) \
{                                                       \
    .type=type_,                                        \
    .name=name_,                                        \
    .alias=alias_,                                      \
    .json_fn=json_fn_,                                  \
    .flag=0,                                            \
    .default_value=0,                                   \
    .description=description_,                          \
    .resource=0,                                        \
    .header=0,                                          \
    .fillsp=0,                                          \
    .schema=items_,                                     \
    .free_fn=0,                                         \
    .__acl__=0,                                         \
    ._offset=0, ._ln=0, ._suboid=0                      \
}

/*-CMD2--type-----------name----------------flag----------------alias---------------items-----------json_fn---------description---------- */
#define SDATACM2(type_, name_, flag_, alias_, items_, json_fn_, description_) \
{                                                       \
    .type=type_,                                        \
    .name=name_,                                        \
    .alias=alias_,                                      \
    .json_fn=json_fn_,                                  \
    .flag=flag_,                                        \
    .default_value=0,                                   \
    .description=description_,                          \
    .resource=0,                                        \
    .header=0,                                          \
    .fillsp=0,                                          \
    .schema=items_,                                     \
    .free_fn=0,                                         \
    .__acl__=0,                                         \
    ._offset=0, ._ln=0, ._suboid=0                      \
}

/*-PM----type-----------name------------flag------------default-----description---------- */
#define SDATAPM(type_, name_, flag_, default_value_, description_) \
{                                                       \
    .type=type_,                                        \
    .name=name_,                                        \
    .alias=0,                                           \
    .json_fn=0,                                         \
    .flag=flag_,                                        \
    .default_value=(void *)(size_t)(default_value_),    \
    .description=description_,                          \
    .resource=0,                                        \
    .header=0,                                          \
    .fillsp=0,                                          \
    .schema=0,                                          \
    .free_fn=0,                                         \
    .__acl__=0,                                         \
    ._offset=0, ._ln=0, ._suboid=0                      \
}

/*-AUTHZ--type----------name------------flag----alias---items---------------description--*/
#define SDATAAUTHZ(type_, name_, flag_, alias_, items_, description_) \
{                                                       \
    .type=type_,                                        \
    .name=name_,                                        \
    .alias=alias_,                                      \
    .json_fn=0,                                         \
    .flag=flag_,                                        \
    .default_value=0,                                   \
    .description=description_,                          \
    .resource=0,                                        \
    .header=0,                                          \
    .fillsp=0,                                          \
    .schema=items_,                                     \
    .free_fn=0,                                         \
    .__acl__=0,                                         \
    ._offset=0, ._ln=0, ._suboid=0                      \
}

/*********************************************************************
 *      Structures
 *********************************************************************/
/*
 *  Cell functions
 */
typedef union _SData_Value_t {
    char *s;            // string
    int b;              // bool
    size_t i;           // integer, as long as possible
    int32_t i32;        // signed integer 32 bits
    int64_t i64;        // signed integer 64 bits
    uint32_t u32;       // unsigned integer 32 bits
    uint64_t u64;       // unsigned integer 64 bits
    double f;           // float
    json_t *j;          // json_t
    dl_list_t *n;       // dl_list_t
    dl_list_t *r;       // dl_list_t (iter)
    void *p;            // pointer
} SData_Value_t;


typedef int (*post_write_it_cb)(void *user_data, const char *name);
typedef int (*post_write2_it_cb)(
    void *user_data,
    const char *name,
    int type,   // my ASN1 types
    SData_Value_t old_v,
    SData_Value_t new_v
);
typedef SData_Value_t (*post_read_it_cb)(void *user_data, const char *name, int type, SData_Value_t v);

/*
 *  Not found callback
 */
typedef int (*not_found_cb_t)(void *user_data, const char *name);

/*
 *  Generic json function
 */
typedef json_t *(*json_function_t)(
    void *param1,
    const char *something,
    json_t *kw, // Owned
    void *param2
);

typedef struct sdata_desc_s {
    uint8_t type;
    const char *name;
    const char **alias;
    json_function_t json_fn;
    sdata_flag_t flag;
    void *default_value;
    const char *description;
    const char *header;
    int fillsp;
    struct sdata_desc_s *schema;
    const char *resource;
    void (*free_fn)(void *);
    void ** __acl__; // not used

    /*
     *  Internal use
     */
    int _offset;    /* variable position in internal buffer */
    int _ln;        /* variable size */
    int _suboid;    /* variable oid subindex */
} sdata_desc_t;

typedef void *hsdata;

/*********************************************************************
 *      Prototypes
 *********************************************************************/

/*----------------------------------*
 *      Creation functions
 *----------------------------------*/

PUBLIC hsdata sdata_create(
    const sdata_desc_t *schema,
    void *user_data,
    post_write_it_cb post_write_cb,
    post_read_it_cb post_rd_cb,
    post_write2_it_cb post_write_stats_cb,
    const char *resource    // maximum 31 bytes. User utility.
);

PUBLIC void sdata_destroy(hsdata hs);  // Compatible free(), no puede retornar int.

/*----------------------------------*
 *      Schema functions
 *----------------------------------*/

PUBLIC const char * sdata_resource(hsdata hs);

/*
 *  Return sdata desc describing resource.
 *  If flag is not null, get the flag of resource item.
 */
PUBLIC const sdata_desc_t * resource_schema(
    const sdata_desc_t *tb_resources,
    const char *resource,
    sdata_flag_t *flag
);
PUBLIC json_t *schema2json(
    const sdata_desc_t *schema,
    const char **key
);

PUBLIC const sdata_desc_t* sdata_schema(hsdata hs);
PUBLIC const sdata_desc_t * sdata_it_desc(const sdata_desc_t *schema, const char *name);


/*----------------------------------*
 *      Cell  functions
 *----------------------------------*/

/*
 *  Get the reference of a Sdata Item for READ/WRITE.
 *  Be aware of the variable type you are retrieving for properly casting it.
 *  DANGER if you don't cast it well: OVERFLOW variables!
 */
PUBLIC void *sdata_it_pointer(hsdata hs, const char *name, const sdata_desc_t **pit); // search it by name, return pointer and it.

/*
 *  Be aware of the variable type you are writing for passing it the right type.
 *  The pointer `ptr` must be returned by sdata_pointer() functions.
 *  WARNING: json data type is NOT owned! (new in V2)
 */
PUBLIC int sdata_write_by_type(hsdata hs, const sdata_desc_t *it, void *ptr, SData_Value_t value);
/*
 *  Be aware of the variable type you are writing for passing it the right type.
 *  The pointer `ptr` must be returned by sdata_it_pointer() function.
 */
PUBLIC SData_Value_t sdata_read_by_type(hsdata hs, const sdata_desc_t *it, void *ptr);


/*----------------------------------*
 *      Utils  functions
 *----------------------------------*/

// mask 0x8000 is internal, to mark an attribute as stats
PUBLIC uint32_t sdata_set_stats_metadata(hsdata hs, const char *name, uint32_t mask, BOOL set); // return previous value
PUBLIC uint32_t sdata_get_stats_metadata(hsdata hs, const char *name);

PUBLIC int sdata_lives(hsdata hs); // WARNING call before any decref()!!!

PUBLIC GBUFFER *get_sdata_flag_desc(sdata_flag_t flag);
PUBLIC BOOL sdata_check_required_attrs(
    hsdata hs,
    not_found_cb_t not_found_cb, // Called when the key not exist in hsdata
    void *user_data
);

PUBLIC const char **sdata_keys( // WARNING remember free with gbmem_free()
    const sdata_desc_t* schema,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag
);

/*
 *  Return a json object describing the hsdata for commands
 */
PUBLIC json_t *sdatacmd2json(
    const sdata_desc_t *items
);
PUBLIC json_t *cmddesc2json(const sdata_desc_t *it);

/*
 *  Return a json OBJECT describing the hsdata for attrs
 */
PUBLIC json_t *sdatadesc2json(
    const sdata_desc_t *items,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag
);
/*
 *  Return a json ARRAY describing the hsdata for attrs
 */
PUBLIC json_t *sdatadesc2json2(
    const sdata_desc_t *items,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag
);
PUBLIC json_t *itdesc2json(const sdata_desc_t *it);

PUBLIC int log_debug_sdata(const char *prefix, hsdata hs);
PUBLIC int log_debug_sdata_iter(
    const char *info,
    dl_list_t *iter,
    walk_type_t walk_type,
    const char *fmt,
    ...
);


/*----------------------------------*
 *      Conversion functions
 *----------------------------------*/

/*
 *  Convert an attribute in json
 */
PUBLIC json_t *item2json(
    hsdata hsdata,
    const char *name,
    sdata_flag_t include_flag,  // valid only S2J options.
    sdata_flag_t exclude_flag
);

/*
 *  Convert a hsdata into a json object with items with flag, or all if flag is -1
 */
PUBLIC json_t *sdata2json(
    hsdata hsdata,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag
);

/*
 *  Convert a hsdata into a json object with items in keys.
 */
PUBLIC json_t *sdata2json3(
    hsdata hsdata,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag,
    const char **keys
);

/*
 *  Sort a iterator of hsdata with a `id` field (must be uint64_t)
 *  Return a new iter and delete the old
 */
PUBLIC dl_list_t * sdata_sort_iter_by_id(
    dl_list_t * iter // owned
);

/*
 *  Convert a iterator of hsdata into a json array of json objects with items with flag, or all if flag is -1
 */
PUBLIC json_t * sdata_iter2json(
    dl_list_t * iter,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag
);

/*
 *  Convert a iterator of hsdata into a json array of json objects with items in keys.
 */
PUBLIC json_t * sdata_iter2json3(
    dl_list_t * iter,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag,
    const char **keys
);

/*
 *  Convert json to item
 */
PUBLIC int json2item(
    hsdata hsdata,
    const char *name,
    json_t *jn_value  // not owned
);

/*
 *  Update sdata fields from a json dict.
 *  Constraint: if flag is -1
 *                  => all fields!
 *              else
 *                  => matched flag
 */
PUBLIC int json2sdata(
    hsdata hsdata,
    json_t* kw,  // Not owned
    sdata_flag_t flag,
    not_found_cb_t not_found_cb, // Called when the key not exist in hsdata
    void *user_data
);

/*
 *  Like json2sdata but in all sdata of iter
 */
PUBLIC int json2sdata_iter(
    dl_list_t *iter,
    json_t *kw,  // not owned
    sdata_flag_t flag,
    not_found_cb_t not_found_cb, // Called when the key not exist in hsdata
    void *user_data
);


/*---------------------------------*
 *      Search/Match functions
 *---------------------------------*/
/*
 *  Find the first resource matching the filter
 */
PUBLIC hsdata sdata_iter_find_one(
    dl_list_t * iter,
    json_t *jn_filter,  // owned
    rc_instance_t** i_hs
);

/*
 *  Return iter with matched resources (free with with rc_free_iter(iter, TRUE, 0);)
 *  If match_fn is null then kw_match_simple() is used
 */
PUBLIC BOOL sdata_match(
    hsdata hs,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag,
    json_t *jn_filter, // owned
    BOOL (*match_fn) (
        json_t *kw,         // not owned
        json_t *jn_filter   // owned
    )
);

/*
 *  Return iter with matched resources (free with with rc_free_iter(iter, TRUE, 0);)
 *  If match_fn is null then kw_match_simple() is used
 */
PUBLIC dl_list_t * sdata_iter_match(
    dl_list_t * iter,
    sdata_flag_t include_flag,
    sdata_flag_t exclude_flag,
    json_t *jn_filter,  // owned
    BOOL (*match_fn) (
        json_t *kw,         // not owned
        json_t *jn_filter   // owned
    )
);


/*-------------------------------*
 *      Load/save persistent
 *-------------------------------*/

/*
 *  Load/save persistent and writable attrs
 *  HACK Attrs MUST have SDF_PERSIST (previous was SDF_PERSIST|SDF_WR)
 *  WARNING changed in 2.0.18.
 */
PUBLIC int sdata_load_persistent(hsdata hs, const char *path);
PUBLIC int sdata_save_persistent(hsdata hs, const char *path, int rpermission);

/*
 *  Load/save persistent attrs
 *  HACK Attrs MUST have SDF_PERSIST
 *  PKEY is used as key of dict
 */
PUBLIC int sdata_iter_load_persistent(
    dl_list_t *iter,
    const char *path,
    not_found_cb_t not_found_cb, // Called when the key not exist in hsdata
    void *user_data
);
PUBLIC int sdata_iter_save_persistent(
    dl_list_t *iter,
    const char *path,
    int rpermission
);

/*
 *  Load a table from json list of dictionaries
 *  This function is NOT to work with PERSISTENT fields, instead,
 *  it will fill the table with all found fields in the json list's dictionaries.
 *  OLD table_load_rows_from_json()
 */
PUBLIC int sdata_iter_load_from_dict_list(
    dl_list_t * iter,
    sdata_desc_t *schema,
    json_t *jn_list,  // not owned
    not_found_cb_t not_found_cb, // Called when the key not exist in hsdata
    int (*cb)(void *user_data, hsdata hs),
    void *user_data
);


/*----------------------------*
 *      Read functions
 *---------------------------*/

PUBLIC const char *sdata_read_str(hsdata hs, const char *name);
PUBLIC BOOL sdata_read_bool(hsdata hs, const char *name);
PUBLIC int32_t sdata_read_int32(hsdata hs, const char *name);
PUBLIC uint32_t sdata_read_uint32(hsdata hs, const char *name);
PUBLIC int64_t sdata_read_int64(hsdata hs, const char *name);
PUBLIC uint64_t sdata_read_uint64(hsdata hs, const char *name);
PUBLIC uint64_t sdata_read_integer(hsdata hs, const char *name);
PUBLIC double sdata_read_real(hsdata hs, const char *name);
PUBLIC json_t *sdata_read_json(hsdata hs, const char *name); // WARNING not incref, it's not your own.
PUBLIC void *sdata_read_pointer(hsdata hs, const char *name);
PUBLIC dl_list_t *sdata_read_dl_list(hsdata hs, const char* name);
PUBLIC dl_list_t *sdata_read_iter(hsdata hs, const char* name);
PUBLIC const sdata_desc_t *sdata_read_schema(hsdata hs, const char* name);
PUBLIC SData_Value_t sdata_get_default_value(hsdata hs, const char* name);  // Only for basic types

/*----------------------------*
 *      Write functions
 *---------------------------*/

PUBLIC int sdata_write_str(hsdata hs, const char *name, const char *value);
PUBLIC int sdata_write_bool(hsdata hs, const char *name, BOOL value);
PUBLIC int sdata_write_int32(hsdata hs, const char *name, int32_t value);
PUBLIC int sdata_write_uint32(hsdata hs, const char *name, uint32_t value);
PUBLIC int sdata_write_int64(hsdata hs, const char *name, int64_t value);
PUBLIC int sdata_write_uint64(hsdata hs, const char *name, uint64_t value);
PUBLIC int sdata_write_integer(hsdata hs, const char *name, uint64_t value);
PUBLIC int sdata_write_real(hsdata hs, const char *name, double value);
PUBLIC int sdata_write_json(hsdata hs, const char *name, json_t *value); // WARNING json is incref
PUBLIC int sdata_write_pointer(hsdata hs, const char *name, void *value);
PUBLIC int sdata_write_struct(hsdata hs, const char *name, void *value, int size);


#ifdef __cplusplus
}
#endif


#endif

