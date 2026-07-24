data_dir = get_option('datadir')

vulkan_layer_dir = join_paths(data_dir, 'vulkan', 'implicit_layer.d')

configure_file(
    input : 'VKIntox.json.in',
    output : 'VKIntox.json',
    configuration : {'layer_library_path' : join_paths(get_option('prefix'), lib_dir, 'libvkintox.so')},
    install : true,
    install_dir : vulkan_layer_dir,
)
