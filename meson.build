project(
    'cz-asset-broker',
    'c','cpp',
    version : run_command('cat', files('./VERSION'), check : false).stdout().strip(),
    meson_version: '>= 0.60.0',
    default_options: [
        'warning_level=2',
        'buildtype=release',
        'cpp_std=c++20'])

NAME = 'AssetBroker'
VERSION_MAJOR = meson.project_version().split('.')[0]
VERSION_MINOR = meson.project_version().split('.')[1]
VERSION_PATCH = meson.project_version().split('.')[2]
VERSION_BUILD = run_command('cat', './BUILD', check : false).stdout()
HEADERS_INSTALL_DIR = join_paths(get_option('prefix'), get_option('includedir'), 'CZ/AssetBroker')
CONF_FILE_NAME = 'CZAssetBrokerVersion.h'

conf = configuration_data()
conf.set('VERSION_MAJOR', VERSION_MAJOR)
conf.set('VERSION_MINOR', VERSION_MINOR)
conf.set('VERSION_PATCH', VERSION_PATCH)
conf_file = configure_file(
    input : 'src/CZ/AssetBroker/' + CONF_FILE_NAME + '.in',
    output : CONF_FILE_NAME,
    configuration : conf)

install_data(
    conf_file,
    install_dir: HEADERS_INSTALL_DIR)

# -------------- DEPENDENCIES --------------

pkg           = import('pkgconfig')
cz_core_dep   = dependency('cz-core',   fallback:['cz-core',   'cz_core_dep'])
cz_ream_dep   = dependency('cz-ream',   fallback:['cz-ream',   'cz_ream_dep'])
cz_xdgkit_dep = dependency('cz-xdgkit', fallback:['cz-xdgkit', 'cz_xdgkit_dep'])
drm_dep       = dependency('libdrm', version: '>= 2.4.113')

deps = [
    cz_core_dep,
    cz_ream_dep,
    cz_xdgkit_dep,
    drm_dep
]

# -------------- HEADERS --------------

header_dirs = [
    ['./src/CZ/AssetBroker', '']
]

foreach header_dir : header_dirs
    header_files = run_command('find', header_dir[0], '-type', 'f', '-name', '*.h', '-maxdepth', '1', check : false).stdout().strip().split('\n')
    install_headers(header_files, install_dir : join_paths(HEADERS_INSTALL_DIR, header_dir[1]))
endforeach

# -------------- LIBRARY --------------

cz_asset_broker = library(
    'cz-asset-broker',
    sources : run_command('find', './src/CZ/AssetBroker', '-type', 'f', '-name', '*.cpp', check : false).stdout().strip().split('\n'),
    include_directories : ['./src'],
    dependencies : deps,
    soversion: VERSION_MAJOR,
    install : true)

cz_asset_broker_dep = declare_dependency(
    dependencies: deps,
    include_directories : ['./src/', './src/CZ'],
    link_with : cz_asset_broker)

pkg.generate(
    cz_asset_broker,
    name: 'AssetBroker',
    description: 'Shared icon and asset loading for Cuarzo, optionally served across processes as DMA-bufs.',
    version: meson.project_version(),
    subdirs: ['CZ/AssetBroker'],
    filebase: 'cz-asset-broker')

# -------------- SERVICE --------------

if get_option('build_service')
    subdir('src/cz-asset-broker')

    # systemd user units: the service is started on the first connection
    unit_dir = get_option('systemd_user_unit_dir')

    if unit_dir == ''
        unit_dir = join_paths(get_option('prefix'), 'lib', 'systemd', 'user')
    endif

    if unit_dir != 'no'
        unit_conf = configuration_data()
        unit_conf.set('BINDIR', join_paths(get_option('prefix'), get_option('bindir')))

        configure_file(
            input : 'data/cz-asset-broker.service.in',
            output : 'cz-asset-broker.service',
            configuration : unit_conf,
            install_dir : unit_dir)

        install_data('data/cz-asset-broker.socket', install_dir : unit_dir)
    endif
endif

# -------------- EXAMPLES --------------

if get_option('build_examples')
    subdir('src/examples/cz-asset-broker-example')
    subdir('src/examples/cz-asset-broker-bench')
endif
