project('clipling', 'cpp',
  version : '0.1.1',
  license : 'AGPL-3.0-only',
  meson_version : '>=0.60',
  default_options : ['cpp_std=c++20', 'warning_level=2'])

gtkmm = dependency('gtkmm-4.0')
sqlite = dependency('sqlite3')
layershell = dependency('gtk4-layer-shell-0')

executable('clipling',
  'src/main.cpp',
  dependencies : [gtkmm, sqlite, layershell],
  install : true)

# Themes are embedded in the binary and written to ~/.config/clipling/themes on
# first run; the copies in /usr/share are samples for your own themes.
install_data('themes/dark.css', 'themes/light.css', 'themes/pipboy.css',
  install_dir : get_option('datadir') / 'clipling' / 'themes')

install_data('data/io.github.serlk.clipling.desktop',
  install_dir : get_option('datadir') / 'applications')
