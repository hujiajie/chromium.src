# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# IMPORTANT:
# Please don't directly include this file if you are building via gyp_chromium,
# since gyp_chromium is automatically forcing its inclusion.
{
  # Variables expected to be overriden on the GYP command line (-D) or by
  # ~/.gyp/include.gypi.
  'variables': {
    # Putting a variables dict inside another variables dict looks kind of
    # weird.  This is done so that 'host_arch', 'use_sysroot', etc are defined as
    # variables within the outer variables dict here.  This is necessary
    # to get these variables defined for the conditions within this variables
    # dict that operate on these variables.
    'variables': {
      'variables': {
        'variables': {
          'variables': {
            # Compute the architecture that we're building on.
            'host_arch%': '<!pymod_do_main(detect_host_arch)',
          },
          # Copy conditionally-set variables out one scope.
          'host_arch%': '<(host_arch)',
          # By default we build against a stable sysroot image to avoid
          # depending on the packages installed on the local machine. Set this
          # to 0 to build against locally installed headers and libraries (e.g.
          # if packaging for a linux distro)
          'use_sysroot%': 1,
          # Default architecture we're building for is the architecture we're
          # building on.
          'target_arch%': '<(host_arch)',
        },
        # Copy conditionally-set variables out one scope.
        'host_arch%': '<(host_arch)',
        'target_arch%': '<(target_arch)',
        'use_sysroot%': '<(use_sysroot)',
        'conditions': [
          # The system root for linux builds.
          ['OS=="linux" and use_sysroot==1', {
            # sysroot needs to be an absolute path otherwise it generates
            # incorrect results when passed to C/C++ compiler
            'conditions': [
              ['target_arch=="arm"', {
                'sysroot%': '<!(cd <(DEPTH) && pwd -P)/build/linux/debian_jessie_arm-sysroot',
              }],
              ['target_arch=="arm64"', {
                'sysroot%': '<!(cd <(DEPTH) && pwd -P)/build/linux/debian_jessie_arm64-sysroot',
              }],
              ['target_arch=="x64"', {
                'sysroot%': '<!(cd <(DEPTH) && pwd -P)/build/linux/debian_jessie_amd64-sysroot',
              }],
              ['target_arch=="ia32"', {
                'sysroot%': '<!(cd <(DEPTH) && pwd -P)/build/linux/debian_jessie_i386-sysroot',
              }],
            ],
          }, {
            'sysroot%': '',
          }],
        ],
      },
      # Copy conditionally-set variables out one scope.
      'host_arch%': '<(host_arch)',
      'target_arch%': '<(target_arch)',
      'sysroot%': '<(sysroot)',
      'use_sysroot%': '<(use_sysroot)',
      # By default, component is set to static_library and it can be overriden
      # by the GYP command line or by ~/.gyp/include.gypi.
      'component%': 'static_library',
      # Enable building with ASAN (Clang's -fsanitize=address option).
      # -fsanitize=address only works with clang, but asan=1 implies clang=1
      # See https://sites.google.com/a/chromium.org/dev/developers/testing/addresssanitizer
      'asan%': 0,
      # Clang stuff.
      'clang_dir%': 'third_party/llvm-build/Release+Asserts',
      # Set this to true when building with Clang.
      # See https://chromium.googlesource.com/chromium/src/+/master/docs/clang.md for details.
      # If this is set, clang is used as both host and target compiler in
      # cross-compile builds.
      'clang%': 0,
      # If this is set clang is used as host compiler, but not as target
      # compiler. Always do this by default.
      'host_clang%': 1,
      'conditions': [
        # linux_use_bundled_gold: whether to use the gold linker binary checked
        # into third_party/binutils.  Force this off via GYP_DEFINES when you
        # are using a custom toolchain and need to control -B in ldflags.
        # Do not use 32-bit gold on 32-bit hosts as it runs out address space
        # for component=static_library builds.
        # linux_use_gold_flags: whether to use build flags that rely on gold.
        ['OS=="linux" and (target_arch=="x64" or target_arch=="ia32" or target_arch=="arm")', {
          'linux_use_bundled_gold%': 1,
          'linux_use_gold_flags%': 1,
        }, {
          'linux_use_bundled_gold%': 0,
          'linux_use_gold_flags%': 0,
        }],
        # linux_use_bundled_binutils: whether to use the binary binutils
        # checked into third_party/binutils.  These are not multi-arch so cannot
        # be used except on x86 and x86-64 (the only two architectures which
        # are currently checke in).  Force this off via GYP_DEFINES when you
        # are using a custom toolchain and need to control -B in cflags.
        ['OS=="linux" and (target_arch=="x64" or target_arch=="ia32")', {
          'linux_use_bundled_binutils%': 1,
        }, {
          'linux_use_bundled_binutils%': 0,
        }],
      ],
    },
    # Copy conditionally-set variables out one scope.
    'target_arch%': '<(target_arch)',
    'host_arch%': '<(host_arch)',
    'sysroot%': '<(sysroot)',
    'use_sysroot%': '<(use_sysroot)',
    'component%': '<(component)',
    'asan%': '<(asan)',
    'linux_use_bundled_gold%': '<(linux_use_bundled_gold)',
    'linux_use_bundled_binutils%': '<(linux_use_bundled_binutils)',
    'linux_use_gold_flags%': '<(linux_use_gold_flags)',
    # Clang stuff.
    'clang%': '<(clang)',
    'host_clang%': '<(host_clang)',
    'clang_dir%': '<(clang_dir)',
    # Override where to find binutils
    'binutils_dir%': '',
    'conditions': [
      ['OS=="linux"', {
        # Use a 64-bit linker to avoid running out of address space. The
        # buildbots should have a 64-bit kernel and a 64-bit libc installed.
        'binutils_dir%': 'third_party/binutils/Linux_x64/Release/bin',
      }],
      ['OS=="linux"', {
        'clang%': 1,
      }],
      ['OS=="mac"', {
        'clang%': 1,
      }],
      ['asan==1', {
        'clang%': 1,
      }],
      ['host_clang==1', {
        'host_cc': '<(clang_dir)/bin/clang',
        'host_cxx': '<(clang_dir)/bin/clang++',
      }, {
        'host_cc': '<!(which gcc)',
        'host_cxx': '<!(which g++)',
      }],
    ],
  },
  'target_defaults': {
    'conditions': [
      ['clang==1 or host_clang==1', {
        # This is here so that all files get recompiled after a clang roll and
        # when turning clang on or off.
        # (defines are passed via the command line, and build systems rebuild
        # things when their commandline changes). Nothing should ever read this
        # define.
        'defines': ['CR_CLANG_REVISION=<!(python <(DEPTH)/tools/clang/scripts/update.py --print-revision)'],
      }],
    ],
    'default_configuration': 'Release',
    'configurations': {
      #
      # Abstract base configurations to cover common attributes.
      #
      'x86_Base': {
        'abstract': 1,
      },
      'x64_Base': {
        'abstract': 1,
      },
      #
      # Concrete configurations
      #
      'Debug': {
        'conditions': [
          ['OS=="win" and target_arch=="x64"', {
            'inherit_from': ['x64_Base'],
          }, {
            'inherit_from': ['x86_Base'],
          }],
        ],
      },
      'Release': {
        'conditions': [
          ['OS=="win" and target_arch=="x64"', {
            'inherit_from': ['x64_Base'],
          }, {
            'inherit_from': ['x86_Base'],
          }],
        ],
      },
    },
  },
  'conditions': [
    ['OS=="linux"', {
      'target_defaults': {
        'conditions': [
          ['sysroot!=""', {
            'target_conditions': [
              ['_toolset=="target"', {
                'cflags': [
                  '--sysroot=<(sysroot)',
                ],
                'ldflags': [
                  '--sysroot=<(sysroot)',
                  '<!(<(DEPTH)/content/nw/tools/sysroot_ld_path.sh <(sysroot))',
                ],
              }],
            ],
          }],
          ['linux_use_gold_flags==1', {
            # Newer gccs and clangs support -fuse-ld, use the flag to force gold
            # selection.
            # gcc -- http://gcc.gnu.org/onlinedocs/gcc-4.8.0/gcc/Optimize-Options.html
            'ldflags': [ '-fuse-ld=gold', ],
          }],
          ['linux_use_bundled_binutils==1', {
            'cflags': [
              '-B<!(cd <(DEPTH) && pwd -P)/<(binutils_dir)',
            ],
          }],
          ['linux_use_bundled_gold==1', {
            # Put our binutils, which contains gold in the search path. We pass
            # the path to gold to the compiler. gyp leaves unspecified what the
            # cwd is when running the compiler, so the normal gyp path-munging
            # fails us. This hack gets the right path.
            'ldflags': [
              '-B<!(cd <(DEPTH) && pwd -P)/<(binutils_dir)',
            ],
          }],
        ],
      },
    }],
    ['OS=="mac"', {
      'target_defaults': {
        'variables': {
          'clang_bin_dir': '../third_party/llvm-build/Release+Asserts/bin',
        },
        'xcode_settings': {
          'CC': '$(SOURCE_ROOT)/<(clang_bin_dir)/clang',
          'LDPLUSPLUS': '$(SOURCE_ROOT)/<(clang_bin_dir)/clang++',
        },
      },
    }],
    ['OS=="win"', {
      'target_defaults': {
        'configurations': {
          'x86_Base': {
            'msvs_settings': {
              'conditions': [
                ['asan==1', {
                  # TODO(asan/win): Move this down into the general
                  # win-target_defaults section once the 64-bit asan runtime
                  # exists.  See crbug.com/345874.
                  'VCCLCompilerTool': {
                    'AdditionalIncludeDirectories': [
                      # MSVC needs to be able to find the sanitizer headers when
                      # invoked via /fallback. This is critical for using macros
                      # like ASAN_UNPOISON_MEMORY_REGION in files where we fall
                      # back.
                      '<(DEPTH)/<(clang_dir)/lib/clang/<!(python <(DEPTH)/tools/clang/scripts/update.py --print-clang-version)/include_sanitizer',
                    ],
                  },
                  'VCLinkerTool': {
                    'AdditionalLibraryDirectories': [
                      # TODO: If clang_dir is absolute, this breaks.
                      '<(DEPTH)/<(clang_dir)/lib/clang/<!(python <(DEPTH)/tools/clang/scripts/update.py --print-clang-version)/lib/windows',
                    ],
                  },
                  'target_conditions': [
                    ['component=="shared_library"', {
                      'VCLinkerTool': {
                        'AdditionalDependencies': [
                          'clang_rt.asan_dynamic-i386.lib',
                          'clang_rt.asan_dynamic_runtime_thunk-i386.lib',
                        ],
                      },
                    }],
                    ['_type=="executable" and component=="static_library"', {
                      'VCLinkerTool': {
                        'AdditionalDependencies': [
                          'clang_rt.asan-i386.lib',
                        ],
                      },
                    }],
                    ['(_type=="shared_library" or _type=="loadable_module") and component=="static_library"', {
                      'VCLinkerTool': {
                        'AdditionalDependencies': [
                          'clang_rt.asan_dll_thunk-i386.lib',
                        ],
                      },
                    }],
                  ],
                }],
              ],
            },
          },
        },
      },
    }],
    ['clang==1 and OS!="win"', {
      'make_global_settings': [
        ['CC', '<(clang_dir)/bin/clang'],
        ['CXX', '<(clang_dir)/bin/clang++'],
        ['CC.host', '$(CC)'],
        ['CXX.host', '$(CXX)'],
      ],
    }],
    ['clang==1 and OS=="win"', {
      'make_global_settings': [
        # On Windows, gyp's ninja generator only looks at CC.
        ['CC', '<(clang_dir)/bin/clang-cl'],
      ],
    }],
    ['OS=="linux" and target_arch=="arm" and host_arch!="arm" and clang==0', {
      # Set default ARM cross tools on linux.  These can be overridden
      # using CC,CXX,CC.host and CXX.host environment variables.
      'make_global_settings': [
        ['CC', '<!(which arm-linux-gnueabihf-gcc)'],
        ['CXX', '<!(which arm-linux-gnueabihf-g++)'],
        ['CC.host', '<(host_cc)'],
        ['CXX.host', '<(host_cxx)'],
      ],
    }],
    ['OS=="linux" and target_arch=="arm64" and host_arch!="arm64" and clang==0', {
      'make_global_settings': [
        ['CC', '<!(which aarch64-linux-gnu-gcc)'],
        ['CXX', '<!(which aarch64-linux-gnu-g++)'],
        ['CC.host', '<(host_cc)'],
        ['CXX.host', '<(host_cxx)'],
      ],
    }],
  ],
}
