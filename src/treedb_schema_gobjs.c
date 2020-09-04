#pragma once

/*
 *
                        ┌───────────────┐
                        │     gclass    │
                        └───────────────┘
                                ▲ n (hook 'gobjs')
                                ┃
                                ┃
                                ▼ 1 (fkey 'gclass_name')
            ┌───────────────────────────────────────┐
            │               gobj                    │
            └───────────────────────────────────────┘
                    ▲ n (hook 'childs')     1 (fkey 'parent_id')
                    │                       │
                    │                       │
                    └───────────────────────┘


                where n -> dl or []

*/

static char treedb_schema_gobjs[]= "\
{                                                                   \n\
    'id': 'gobjs',                                                  \n\
    'schema_version': '1',                                          \n\
    'topics': [                                                     \n\
        {                                                           \n\
            'topic_name': 'gclass',                                 \n\
            'pkey': 'id',                                           \n\
            'system_flag': 'sf_string_key',                         \n\
            'topic_version': '1',                                   \n\
            'cols': {                                               \n\
                'id': {                                             \n\
                    'header': 'id',                                 \n\
                    'fillspace': 10,                                \n\
                    'type': 'string',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'base': {                                           \n\
                    'header': 'base',                               \n\
                    'fillspace': 10,                                \n\
                    'type': 'string',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'priv_size': {                                      \n\
                    'header': 'priv_size',                          \n\
                    'fillspace': 6,                                 \n\
                    'type': 'integer',                              \n\
                    'flag': [                                       \n\
                        'persistent'                                \n\
                    ]                                               \n\
                },                                                  \n\
                'attrs': {                                          \n\
                    'header': 'attributes',                         \n\
                    'fillspace': 20,                                \n\
                    'type': 'object',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'commands': {                                       \n\
                    'header': 'commands',                           \n\
                    'fillspace': 20,                                \n\
                    'type': 'object',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'global_methods': {                                 \n\
                    'header': 'global_methods',                     \n\
                    'fillspace': 20,                                \n\
                    'type': 'object',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'local_methods': {                                  \n\
                    'header': 'local_methods',                      \n\
                    'fillspace': 20,                                \n\
                    'type': 'object',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'FSM': {                                            \n\
                    'header': 'FSM',                                \n\
                    'fillspace': 20,                                \n\
                    'type': 'object',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'ACL': {                                            \n\
                    'header': 'ACL',                                \n\
                    'fillspace': 20,                                \n\
                    'type': 'list',                                 \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'info_global_trace': {                              \n\
                    'header': 'info_global_trace',                  \n\
                    'fillspace': 20,                                \n\
                    'type': 'object',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'info_gclass_trace': {                              \n\
                    'header': 'info_gclass_trace',                  \n\
                    'fillspace': 20,                                \n\
                    'type': 'object',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'gclass_trace_level': {                             \n\
                    'header': 'gclass_trace_level',                 \n\
                    'fillspace': 6,                                 \n\
                    'type': 'integer',                              \n\
                    'flag': [                                       \n\
                        'persistent'                                \n\
                    ]                                               \n\
                },                                                  \n\
                'gclass_no_trace_level': {                          \n\
                    'header': 'gclass_no_trace_level',              \n\
                    'fillspace': 6,                                 \n\
                    'type': 'integer',                              \n\
                    'flag': [                                       \n\
                        'persistent'                                \n\
                    ]                                               \n\
                },                                                  \n\
                'instances': {                                      \n\
                    'header': 'instances',                          \n\
                    'fillspace': 6,                                 \n\
                    'type': 'integer',                              \n\
                    'flag': [                                       \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'gobjs': {                                          \n\
                    'header': 'gobjs',                              \n\
                    'fillspace': 20,                                \n\
                    'type': 'array',                                \n\
                    'flag': ['hook'],                               \n\
                    'hook': {                                       \n\
                        'gobj': 'gclass_name'                       \n\
                    }                                               \n\
                }                                                   \n\
            }                                                       \n\
        },                                                          \n\
        {                                                           \n\
            'topic_name': 'gobj' ,                                  \n\
            'pkey': 'id',                                           \n\
            'system_flag': 'sf_string_key',                         \n\
            'topic_version': '1',                                   \n\
            'cols': {                                               \n\
                'id': {                                             \n\
                    'header': 'id',                                 \n\
                    'fillspace': 10,                                \n\
                    'type': 'string',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'name': {                                           \n\
                    'header': 'name',                               \n\
                    'fillspace': 20,                                \n\
                    'type': 'string',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'shortname': {                                      \n\
                    'header': 'shortname',                          \n\
                    'fillspace': 20,                                \n\
                    'type': 'string',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'fullname': {                                       \n\
                    'header': 'fullname',                           \n\
                    'fillspace': 20,                                \n\
                    'type': 'string',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'gclass_name': {                                    \n\
                    'header': 'gclass_name',                        \n\
                    'fillspace': 20,                                \n\
                    'type': 'string',                               \n\
                    'flag': [                                       \n\
                        'fkey'                                      \n\
                    ]                                               \n\
                },                                                  \n\
                'running': {                                        \n\
                    'header': 'running',                            \n\
                    'fillspace': 7,                                 \n\
                    'type': 'boolean',                              \n\
                    'flag': [                                       \n\
                    ]                                               \n\
                },                                                  \n\
                'playing': {                                        \n\
                    'header': 'playing',                            \n\
                    'fillspace': 7,                                 \n\
                    'type': 'boolean',                              \n\
                    'flag': [                                       \n\
                    ]                                               \n\
                },                                                  \n\
                'service': {                                        \n\
                    'header': 'service',                            \n\
                    'fillspace': 7,                                 \n\
                    'type': 'boolean',                              \n\
                    'flag': [                                       \n\
                        'persistent'                                \n\
                    ]                                               \n\
                },                                                  \n\
                'unique': {                                         \n\
                    'header': 'unique',                             \n\
                    'fillspace': 7,                                 \n\
                    'type': 'boolean',                              \n\
                    'flag': [                                       \n\
                        'persistent'                                \n\
                    ]                                               \n\
                },                                                  \n\
                'disabled': {                                       \n\
                    'header': 'disabled',                           \n\
                    'fillspace': 7,                                 \n\
                    'type': 'boolean',                              \n\
                    'flag': [                                       \n\
                        'persistent'                                \n\
                    ]                                               \n\
                },                                                  \n\
                'state': {                                          \n\
                    'header': 'state',                              \n\
                    'fillspace': 20,                                \n\
                    'type': 'string',                               \n\
                    'flag': [                                       \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'gobj_trace_level': {                               \n\
                    'header': 'gobj_trace_level',                   \n\
                    'fillspace': 6,                                 \n\
                    'type': 'integer',                              \n\
                    'flag': [                                       \n\
                        'persistent'                                \n\
                    ]                                               \n\
                },                                                  \n\
                'gobj_no_trace_level': {                            \n\
                    'header': 'gobj_no_trace_level',                \n\
                    'fillspace': 6,                                 \n\
                    'type': 'integer',                              \n\
                    'flag': [                                       \n\
                        'persistent'                                \n\
                    ]                                               \n\
                },                                                  \n\
                'attrs': {                                          \n\
                    'header': 'attrs',                              \n\
                    'fillspace': 20,                                \n\
                    'type': 'object',                               \n\
                    'flag': [                                       \n\
                        'persistent',                               \n\
                        'required'                                  \n\
                    ]                                               \n\
                },                                                  \n\
                'parent_id': {                                      \n\
                    'header': 'parent_id',                          \n\
                    'fillspace': 10,                                \n\
                    'type': 'string',                               \n\
                    'flag': [                                       \n\
                        'fkey'                                      \n\
                    ]                                               \n\
                },                                                  \n\
                'childs': {                                         \n\
                    'header': 'childs',                             \n\
                    'fillspace': 20,                                \n\
                    'type': 'array',                                \n\
                    'flag': ['hook'],                               \n\
                    'hook': {                                       \n\
                        'gobj': 'parent_id'                         \n\
                    }                                               \n\
                }                                                   \n\
            }                                                       \n\
        }                                                           \n\
    ]                                                               \n\
}                                                                   \n\
";
