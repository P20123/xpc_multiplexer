project('xpc_multiplexer', 'c')
# project setup
if get_option('buildtype') == 'debug'
    # make a debug build target!
    add_project_arguments(['-ggdb3'], language: 'c')
endif

# ========= BUILDING CONTROL =========
# Build tests?
should_build_tests = false
if get_option('build_tests') == 'yes'
    should_build_tests = true
elif get_option('build_tests') == 'not_subproject'
    should_build_tests = not meson.is_subproject()
endif
# ========= END BUILDING CONTROL =========

# ========= EXTERNAL PROJECT DEPENDENCIES =========
dep_alc_dynabuf = dependency(
    'alc_dynabuf',
    fallback: ['alibc.containers', 'dep_dynabuf']
)

dep_alc_array = dependency(
    'alc_array',
    fallback: ['alibc.containers', 'dep_array']
)
dep_alc_array_iter = dependency(
    'alc_array_iter',
    fallback: ['alibc.containers', 'dep_array_iter']
)
dep_alc_iterator = dependency(
    'alc_iterator',
    fallback: ['alibc.containers', 'dep_iterator']
)

dep_alc_hashmap = dependency(
    'alc_hashmap',
    fallback: ['alibc.containers', 'dep_hashmap']
)

dep_alc_hashmap_iter = dependency(
    'alc_hashmap_iter',
    fallback: ['alibc.containers', 'dep_hashmap_iter']
)

dep_alc_hash_functions = dependency(
    'alc_hash_functions',
    fallback: ['alibc.containers', 'dep_hash_functions']
)

dep_alc_comparators = dependency(
    'alc_comparators',
    fallback: ['alibc.containers', 'dep_comparators']
)

dep_txpc = dependency(
    'tinyxpc',
    fallback: ['tinyxpc', 'dep_txpc']
)
# ========= END EXTERNAL PROJECT DEPENDENCIES =========

includes = include_directories('include')

# ========= EXECUTABLE TARGETS =========
exe_main = executable(
    'main',
    [
        'src/main.c',
        'src/epoll_app.c',
        'src/xpc_msg_queue.c',
        'src/xpc_utils.c',
        'src/xpc_relay.c'
    ],
    include_directories: includes,
    dependencies: [
        dep_alc_dynabuf,
        dep_alc_array,
        dep_alc_iterator,
        dep_alc_array_iter,
        dep_alc_hashmap,
        dep_alc_hashmap_iter,
        dep_alc_hash_functions,
        dep_alc_comparators,
        dep_txpc
    ]
)
# ========= END EXECUTABLE TARGETS =========

# ========= UNIT TEST BUILD TARGETS =========
ext_cmocka       = dependency('cmocka', required: false)
if ext_cmocka.found() and should_build_tests
    exe_msg_queue_test = executable(
        'test_msg_queue',
        [
            'tests/test_msg_queue.c',
            'src/xpc_msg_queue.c'
        ],
        include_directories: includes,
        link_with:
            [
            ],
        dependencies: [
            ext_cmocka,
            dep_txpc,
            dep_alc_hashmap,
            dep_alc_hashmap_iter,
            dep_alc_hash_functions,
            dep_alc_comparators,
            dep_alc_array,
            dep_alc_iterator,
            dep_alc_array_iter,
        ]
    )

    exe_relay_test = executable(
        'test_relay',
        [
            'tests/test_relay.c',
            'src/xpc_relay.c',
            'src/crc.c'
        ],
        include_directories: includes,
        link_with:
            [
            ],
        dependencies: [
            ext_cmocka,
            dep_txpc,
            dep_alc_hashmap,
            dep_alc_hashmap_iter,
            dep_alc_hash_functions,
            dep_alc_comparators,
            dep_alc_array,
            dep_alc_iterator,
            dep_alc_array_iter,
        ]
    )

    # test run targets
    test('test_msg_queue', exe_msg_queue_test)
    test('test_relay', exe_relay_test)
endif
# ========= END UNIT TEST BUILD TARGETS =========
