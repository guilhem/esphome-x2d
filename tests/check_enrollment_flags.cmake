# Compile the real consumer header against the selected core, exercising private
# builds as well as rejection of options whose old meaning cannot be preserved.
function(check_flags succeeds expected_suffix diagnostic)
  file(WRITE "${CHECK_BINARY}/check_enrollment_flags.cpp"
    "#include \"enrollment_profile.h\"\nstatic_assert(esphome::x2d::enrollment_profile().identity_suffix == ${expected_suffix});\n")
  execute_process(COMMAND "${CXX}" -std=c++17 -fsyntax-only
    "-I${CHECK_SOURCE}/components/x2d" "-I${CORE_SOURCE}/src" ${ARGN}
    "${CHECK_BINARY}/check_enrollment_flags.cpp"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(succeeds)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Valid flags ${ARGN} rejected: ${output}${error}")
    endif()
  elseif(result EQUAL 0 OR NOT error MATCHES "${diagnostic}")
    message(FATAL_ERROR "Invalid flags ${ARGN}: expected ${diagnostic}, got ${result}: ${output}${error}")
  endif()
endfunction()
check_flags(TRUE 1 "") # RF-disabled default keeps the public core profile.
foreach(suffix IN ITEMS 0 1 90 255)
  check_flags(TRUE ${suffix} "" -DX2D_ENROLLMENT_ENABLED "-DX2D_TRIAL_IDENTITY_SUFFIX=${suffix}")
endforeach()
check_flags(TRUE 90 "" -DX2D_TRIAL_IDENTITY_SUFFIX=90) # also honoured without enrollment
check_flags(FALSE 1 "requires a private" -DX2D_ENROLLMENT_ENABLED)
foreach(suffix IN ITEMS -1 256)
  check_flags(FALSE 1 "Trial suffix must be a byte" "-DX2D_TRIAL_IDENTITY_SUFFIX=${suffix}")
endforeach()
foreach(option IN ITEMS X2D_TRIAL_SLOT X2D_TRIAL_EXPECTED_NEXT_COUNTER)
  check_flags(FALSE 1 "are obsolete" "-D${option}=0")
  check_flags(FALSE 90 "are obsolete" -DX2D_ENROLLMENT_ENABLED -DX2D_TRIAL_IDENTITY_SUFFIX=90 "-D${option}=2")
endforeach()
