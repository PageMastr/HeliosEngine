# Functions are global: engine/z calls these before this file is read, so the scan learns them first.
function(_zh_flags)
  set(_zh_out -mavx2)
  return(PROPAGATE _zh_out)
endfunction()
function(_zh_into out)
  set(${out} -mfma PARENT_SCOPE)
endfunction()
