{
    "targets": [
        {
            "target_name": "superstring",
            "dependencies": [
                "superstring_core",
                "superstring_tree_sitter",
            ],
            "sources": [
                "src/bindings/bindings.cc",
                "src/bindings/display-layer-wrapper.cc",
                "src/bindings/tree-sitter-wrapper.cc",
                "src/bindings/marker-index-wrapper.cc",
                "src/bindings/patch-wrapper.cc",
                "src/bindings/point-wrapper.cc",
                "src/bindings/range-wrapper.cc",
                "src/bindings/text-buffer-wrapper.cc",
                "src/bindings/text-buffer-snapshot-wrapper.cc",
                "src/bindings/text-reader.cc",
                "src/bindings/string-conversion.cc",
                "src/bindings/text-writer.cc",
            ],
            "include_dirs": [
                "src/core",
                "<!(node -p \"require('node-addon-api').include_dir\")",
            ],
            "defines": [
                "NAPI_VERSION=<(napi_build_version)",
                "NAPI_DISABLE_CPP_EXCEPTIONS",
            ],
            "conditions": [
                ['OS=="mac"', {
                    "postbuilds": [
                        {
                            'postbuild_name': 'Copy vendored libiconv next to the binding',
                            'action': [
                                'bash',
                                '<(module_root_dir)/script/copy-libiconv.sh',
                                '<(module_root_dir)/ext/lib/libiconv.2.dylib',
                                '<(PRODUCT_DIR)/libiconv.2.dylib'
                            ]
                        },
                        {
                            'postbuild_name': 'Adjust vendored libiconv install name',
                            'action': [
                                'install_name_tool',
                                "-change",
                                "libiconv.2.dylib",
                                "@loader_path/libiconv.2.dylib",
                                "<(PRODUCT_DIR)/superstring.node"
                            ]
                        }
                    ]
                }]
            ]
        },
        {
            # Superstring's native Tree-sitter language mode.
            "target_name": "superstring_tree_sitter",
            "type": "static_library",
            "dependencies": [
                "superstring_core",
                "tree_sitter",
            ],
            "export_dependent_settings": ["tree_sitter"],
            "sources": [
                "src/core/tree-sitter/wasm-engine.cc",
                "src/core/tree-sitter/grammar.cc",
                "src/core/tree-sitter/query.cc",
                "src/core/tree-sitter/parser.cc",
            ],
            "include_dirs": [
                "src/core",
            ],
        },
        {
            # Native Tree-sitter with WASM grammar support (see
            # vendor/tree-sitter/README.md). Wasmtime's C API is fetched by
            # script/fetch-wasmtime.js.
            "target_name": "tree_sitter",
            "type": "static_library",
            "sources": [
                "vendor/tree-sitter/lib/src/lib.c",
            ],
            "include_dirs": [
                "vendor/tree-sitter/lib/include",
                "vendor/tree-sitter/lib/src",
                "vendor/tree-sitter/lib/src/wasm",
                "<(wasmtime_dir)/include",
            ],
            "defines": [
                "TREE_SITTER_FEATURE_WASM",
                "_POSIX_C_SOURCE=200112L",
                "_DEFAULT_SOURCE",
            ],
            "cflags_c": ["-std=c11", "-fvisibility=hidden", "-Wno-unused-parameter"],
            "xcode_settings": {
                "OTHER_CFLAGS": ["-std=c11"],
            },
            "direct_dependent_settings": {
                "include_dirs": [
                    "vendor/tree-sitter/lib/include",
                    "<(wasmtime_dir)/include",
                ],
                "defines": ["TREE_SITTER_FEATURE_WASM"],
            },
            "link_settings": {
                "conditions": [
                    ['OS=="linux"', {
                        "libraries": [
                            "<(wasmtime_dir)/lib/libwasmtime.a",
                            "-lpthread",
                            "-ldl",
                            "-lm",
                        ],
                    }],
                    ['OS=="mac"', {
                        "libraries": ["<(wasmtime_dir)/lib/libwasmtime.a"],
                    }],
                    ['OS=="win"', {
                        "libraries": [
                            "<(wasmtime_dir)/lib/wasmtime.lib",
                            "ws2_32.lib",
                            "advapi32.lib",
                            "userenv.lib",
                            "ntdll.lib",
                            "shell32.lib",
                            "ole32.lib",
                            "bcrypt.lib",
                        ],
                    }],
                ],
            },
            "conditions": [
                ['OS=="win"', {
                    "defines": ["WASM_API_EXTERN=", "WASI_API_EXTERN="],
                    "direct_dependent_settings": {
                        "defines": ["WASM_API_EXTERN=", "WASI_API_EXTERN="],
                    },
                }],
            ],
        },
        {
            "target_name": "superstring_core",
            "type": "static_library",
            "dependencies": [
                "./vendor/pcre/pcre.gyp:pcre"
            ],
            "sources": [
                "src/core/display-layer.cc",
                "src/core/encoding-conversion.cc",
                "src/core/marker-index.cc",
                "src/core/patch.cc",
                "src/core/point.cc",
                "src/core/range.cc",
                "src/core/regex.cc",
                "src/core/text.cc",
                "src/core/text-buffer.cc",
                "src/core/text-slice.cc",
                "src/core/text-diff.cc",
                "src/core/libmba-diff.cc",
            ],
            "include_dirs": [
                "vendor/libcxx"
            ],
            "conditions": [
                ['OS=="mac"', {
                    'dependencies': [
                        'build_libiconv'
                    ],
                    'include_dirs': [
                        '<(module_root_dir)/ext/include'
                    ],
                    'link_settings': {
                        'libraries': ['<(module_root_dir)/ext/lib/libiconv.2.dylib']
                    }
                }],
                ['OS=="win"', {
                    'sources': [
                       'vendor/win-iconv/win_iconv.c',
                    ],
                    'include_dirs': [
                        'vendor/win-iconv'
                    ],
                    'defines': [
                        'WINICONV_CONST=',
                        'PCRE2_STATIC',
                    ]
                }],
            ],
        }
    ],

    "variables": {
        "tests": 0,
        "wasmtime_dir%": "<!(node script/fetch-wasmtime.js)",
        "node_version_major%": "<!(node -p \"process.versions.node.split('.')[0]\")"
    },

    "conditions": [
        ['OS=="mac"', {
            'targets+': [
                {
                    "target_name": "build_libiconv",
                    "target_type": "none",
                    "actions": [
                        {
                            "action_name": "Run script",
                            "message": "Building GNU libiconv...",
                            "inputs": [],
                            "outputs": ["<(module_root_dir)/ext/lib/libiconv.2.dylib"],
                            "action": [
                                "bash",
                                "script/fetch-libiconv-61.sh"
                            ]
                        }
                    ]
                }
            ]
        }],

        # If --tests is passed to node-gyp configure, we'll build a standalone
        # executable that runs tests on the patch.
        ['tests != 0', {
            "targets": [{
                "target_name": "tests",
                "type": "executable",
                "cflags_cc!": ["-fno-exceptions", "-std=c++17"],
                "defines": [
                    "CATCH_CONFIG_CPP11_NO_IS_ENUM"
                ],
                'xcode_settings': {
                    'CLANG_CXX_LIBRARY': 'libc++',
                    'CLANG_CXX_LANGUAGE_STANDARD': 'c++17',
                },
                "sources": [
                    "test/native/test-helpers.cc",
                    "test/native/tests.cc",
                    "test/native/encoding-conversion-test.cc",
                    "test/native/patch-test.cc",
                    "test/native/text-buffer-test.cc",
                    "test/native/text-test.cc",
                    "test/native/text-diff-test.cc",
                    "test/native/display-layer-test.cc",
                    "test/native/tree-sitter-test.cc",
                    "vendor/catch_amalgamated.cpp"
                ],
                "include_dirs": [
                    "vendor",
                    "src/core",
                ],
                "dependencies": [
                    "superstring_core",
                    "superstring_tree_sitter",
                ],
                "conditions": [
                    ['node_version_major>=18', {
                        "defines+": ["CATCH_CONFIG_CPP17_STRING_VIEW"]
                    }],
                    ['OS=="mac"', {
                        'dependencies+': [
                            'build_libiconv'
                        ],
                        'cflags': [
                            '-mmacosx-version-min=10.8'
                        ],
                        "xcode_settings": {
                            "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
                            'MACOSX_DEPLOYMENT_TARGET': '10.12',
                        },
                        "postbuilds": [
                            {
                                'postbuild_name': 'Copy vendored libiconv next to the binding',
                                'action': [
                                    'bash',
                                    '<(module_root_dir)/script/copy-libiconv.sh',
                                    '<(module_root_dir)/ext/lib/libiconv.2.dylib',
                                    '<(PRODUCT_DIR)/libiconv.2.dylib'
                                ]
                            },
                            {
                                'postbuild_name': 'Adjust vendored libiconv install name',
                                'action': [
                                    'install_name_tool',
                                    "-change",
                                    "libiconv.2.dylib",
                                    "@executable_path/libiconv.2.dylib",
                                    "<(PRODUCT_DIR)/tests"
                                ]
                            }
                        ]
                    }]
                ]
            }]
        }]
    ],

    "target_defaults": {
        "cflags_cc": ["-std=c++17"],
        "conditions": [
            ['OS=="mac"', {
                'cflags+': ['-fvisibility=hidden'],
                'cflags_cc+': ['-fvisibility=hidden'],
                "xcode_settings": {
                    'MACOSX_DEPLOYMENT_TARGET': '10.8',
                    'CLANG_CXX_LIBRARY': 'libc++',
                    'CLANG_CXX_LANGUAGE_STANDARD': 'c++17',
                    'GCC_SYMBOLS_PRIVATE_EXTERN': 'YES',  # -fvisibility=hidden
                }
            }],
            ['OS=="win"', {
                "link_settings": {
                    "libraries": ["ws2_32.lib"]
                },
                "defines": [
                    "NOMINMAX"
                ],
                # These sources are UTF-8. Absent this flag — and absent a BOM,
                # which none of them have — MSVC decodes them using the system
                # ANSI codepage, which mangles non-ASCII characters in string
                # literals without any diagnostic.
                "msvs_settings": {
                    "VCCLCompilerTool": {
                        "AdditionalOptions": ["/utf-8"]
                    }
                },
            }]
        ]
    }
}
