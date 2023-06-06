ob_set_subtarget(
  ob_ca ca
  share/datum/ob_datum_funcs.cpp
)

# set(TEEC_EXPORT /opt/optee/optee_client/out/export/usr)
message(STATUS ${TEEC_EXPORT})

ob_add_new_object_target(ob_ca ob_ca)

target_include_directories(
  ob_ca PUBLIC
  ta/include
  ${TEEC_EXPORT}/include
)

target_link_directories(
  ob_ca PUBLIC
  ${TEEC_EXPORT}/lib
)

target_link_libraries(ob_ca PUBLIC ob_base teec)

# target_link_libraries(ob_ca PUBLIC teec)