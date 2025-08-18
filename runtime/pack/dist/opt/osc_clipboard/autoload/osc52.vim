vim9script

export def Available(): bool
  # Check using primary device attributes (DA1) response
  if match(v:termda1, ";52;")
    return true
  endif

  # Check using the Ms entry in the termcap database
  if match(&t_Ms, "\<Esc>]52") != -1
    return true
  endif
  return false
enddef

export def Paste(reg: string): void
  augroup OSCClipboard
    autocmd!
    autocmd TermResponseAll osc feedkeys("\<F30>", 't')
  augroup END

  echoraw("\<Esc>]52;c;?\x07")

  # User can press CTRL-C to stop waiting
  while getchar(-1) != "\<F30>"
  endwhile

  autocmd! OSCClipboard

  var contents: string = matchstr(v:termosc, ']52;c;\zs[0-9A-Za-z+/=]\+\ze')
  var stuff: list<string> = blob2str(base64_decode(decoded))

  setreg(reg, stuff)
enddef

export def Copy(reg: string): void
  var str: string = base64_encode(str2blob(getreg(reg)))

  echoraw("\<Esc>]52;c;" .. str .. "\x07")
enddef

# vim:sts=2:sw=2:et:
