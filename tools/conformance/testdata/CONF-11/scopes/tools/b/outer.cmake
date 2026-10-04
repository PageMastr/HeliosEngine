# _outer learns its output only after _inner (tools/c, read after it) is known: a second learning pass.
function(_outer out)
  _inner(_tmp)
  set(${out} ${_tmp} PARENT_SCOPE)
endfunction()
