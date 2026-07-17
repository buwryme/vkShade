data_dir = get_option('datadir')

vulkan_layer_dir = join_paths(data_dir, 'vulkan', 'implicit_layer.d')

configure_file(
    input : 'vkShade.json.in',
    output : 'vkShade.json',
    configuration : {'layer_library_path' : join_paths(get_option('prefix'), lib_dir, 'libvkshade.so')},
    install : true,
    install_dir : vulkan_layer_dir,
)
