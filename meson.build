project('VKIntox', ['c', 'cpp'], default_options: ['c_std=c11', 'cpp_std=c++2a'])

cpp = meson.get_compiler('cpp')

VKIntox_include_path = ''

lib_dir = get_option('libdir')

if get_option('append_libdir_vkintox')
    lib_dir = join_paths(lib_dir, 'vkintox')
endif

if get_option('with_so')
    subdir('src')
endif

if get_option('with_json')
    subdir('config')
endif
