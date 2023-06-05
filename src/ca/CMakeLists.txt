ob_set_subtarget(
  ob_tee ta
  share/datum/ob_datum_funcs.cpp
)


include_directories(
  ob_tee PUBLIC
  ta/include
  $(TEEC_EXPORT)/include
)

link_directories(
  ob_tee
  $(TEEC_EXPORT)/lib
)

link_libraries(teec)

ob_server_add_target(ob_tee)