project('vkShade', ['c', 'cpp'], default_options: ['c_std=c11', 'cpp_std=c++2a'])

cpp = meson.get_compiler('cpp')

vkShade_include_path = ''

lib_dir = get_option('libdir')

if get_option('append_libdir_vkshade')
    lib_dir = join_paths(lib_dir, 'vkshade')
endif

if get_option('with_so')
    subdir('src')
endif

if get_option('with_json')
    subdir('config')
endif
