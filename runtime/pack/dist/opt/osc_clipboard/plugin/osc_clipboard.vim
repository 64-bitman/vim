vim9script

if get(g:, 'loaded_osc_clipboard', false)
  finish
endif
g:loaded_osc_clipboard = true

import autoload '../autoload/osc52.vim'

# vim:sts=2:sw=2:et:
