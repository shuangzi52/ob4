/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX PL

#include "ob_pl_dblink_guard.h"
#include "share/rc/ob_tenant_base.h"
#include "pl/ob_pl_type.h"
#include "sql/dblink/ob_dblink_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "pl/ob_pl_stmt.h"
#ifdef OB_BUILD_ORACLE_PL
#include "lib/oracleclient/ob_oci_metadata.h"
#include "pl/dblink/ob_pl_dblink_util.h"
#endif

namespace oceanbase
{
namespace pl
{
typedef common::sqlclient::DblinkDriverProto DblinkDriverProto;

int ObPLDbLinkGuard::get_routine_infos_with_synonym(sql::ObSQLSessionInfo &session_info,
                                    share::schema::ObSchemaGetterGuard &schema_guard,
                                    const ObString &dblink_name,
                                    const ObString &part1,
                                    const ObString &part2,
                                    const ObString &part3,
                                    common::ObIArray<const share::schema::ObIRoutineInfo *> &routine_infos)
{
  int ret = OB_SUCCESS;
#ifndef OB_BUILD_ORACLE_PL
  ret = OB_NOT_SUPPORTED;
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "PL dblink");
#else
  const uint64_t tenant_id = MTL_ID();
  const ObDbLinkSchema *dblink_schema = NULL;
  DblinkDriverProto link_type = common::sqlclient::DBLINK_UNKNOWN;
  common::sqlclient::dblink_param_ctx param_ctx;
  param_ctx.pool_type_ = common::sqlclient::DblinkPoolType::DBLINK_POOL_SCHEMA;
  common::ObDbLinkProxy *dblink_proxy = NULL;
  common::sqlclient::ObISQLConnection *dblink_conn = NULL;
  ObString full_name;
  ObString schema_name;
  ObString object_name;
  ObString sub_object_name;
  int64_t object_type;
  OZ (schema_guard.get_dblink_schema(tenant_id, dblink_name, dblink_schema), tenant_id, dblink_name);
  OV (OB_NOT_NULL(dblink_schema), OB_DBLINK_NOT_EXIST_TO_ACCESS, dblink_name);
  OX (link_type = static_cast<DblinkDriverProto>(dblink_schema->get_driver_proto()));
  OZ (ObPLDblinkUtil::init_dblink(dblink_proxy, dblink_conn, session_info, schema_guard, dblink_name));
  CK (OB_NOT_NULL(dblink_proxy));
  CK (OB_NOT_NULL(dblink_conn));
  OZ (ObPLDblinkUtil::print_full_name(alloc_, full_name, part1, part2, part3));
  OZ (dblink_name_resolve(dblink_proxy,
                          dblink_conn,
                          dblink_schema,
                          full_name,
                          schema_name,
                          object_name,
                          sub_object_name,
                          object_type,
                          alloc_));
  OZ (get_dblink_routine_infos(session_info,
                               schema_guard,
                               dblink_name,
                               schema_name,
                               object_name,
                               sub_object_name,
                               routine_infos));
#endif
  return ret;
}

int ObPLDbLinkGuard::get_dblink_type_with_synonym(sql::ObSQLSessionInfo &session_info,
                                                  share::schema::ObSchemaGetterGuard &schema_guard,
                                                  const ObString &dblink_name,
                                                  const ObString &part1,
                                                  const ObString &part2,
                                                  const ObString &part3,
                                                  const pl::ObUserDefinedType *&udt)
{
  int ret = OB_SUCCESS;
#ifndef OB_BUILD_ORACLE_PL
  ret = OB_NOT_SUPPORTED;
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "PL dblink");
#else
  common::ObDbLinkProxy *dblink_proxy = NULL;
  common::sqlclient::ObISQLConnection *dblink_conn = NULL;
  OZ (ObPLDblinkUtil::init_dblink(dblink_proxy, dblink_conn, session_info, schema_guard, dblink_name));
  CK (OB_NOT_NULL(dblink_proxy));
  CK (OB_NOT_NULL(dblink_conn));
  if (OB_SUCC(ret)) {
    ObString full_name;
    ObString schema_name;
    ObString object_name;
    ObString sub_object_name;
    int64_t object_type;
    const ObDbLinkSchema *dblink_schema = NULL;
    OZ (schema_guard.get_dblink_schema(MTL_ID(), dblink_name, dblink_schema), dblink_name);
    OV (OB_NOT_NULL(dblink_schema), OB_ERR_UNEXPECTED, dblink_name);
    OZ (ObPLDblinkUtil::print_full_name(alloc_, full_name, part1, part2, part3));
    OZ (dblink_name_resolve(dblink_proxy,
                            dblink_conn,
                            dblink_schema,
                            full_name,
                            schema_name,
                            object_name,
                            sub_object_name,
                            object_type,
                            alloc_));
    OV (static_cast<int64_t>(ObObjectType::PACKAGE) == object_type);
    OZ (get_dblink_type_by_name(session_info,
                                schema_guard,
                                dblink_name,
                                schema_name,
                                object_name,
                                sub_object_name,
                                udt));
  }
#endif
  return ret;
}

int ObPLDbLinkGuard::get_dblink_routine_infos(sql::ObSQLSessionInfo &session_info,
                                              share::schema::ObSchemaGetterGuard &schema_guard,
                                              const ObString &dblink_name,
                                              const ObString &db_name,
                                              const ObString &pkg_name,
                                              const ObString &routine_name,
                                              common::ObIArray<const share::schema::ObIRoutineInfo *> &routine_infos)
{
  int ret = OB_SUCCESS;
#ifndef OB_BUILD_ORACLE_PL
  ret = OB_NOT_SUPPORTED;
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "PL dblink");
#else
  routine_infos.reset();
  const uint64_t tenant_id = MTL_ID();
  uint64_t dblink_id = OB_INVALID_ID;
  const share::schema::ObDbLinkSchema *dblink_schema = NULL;
  const ObPLDbLinkInfo *dblink_info = NULL;
  OZ (schema_guard.get_dblink_schema(tenant_id, dblink_name, dblink_schema));
  OX (dblink_id = dblink_schema->get_dblink_id());
  OV (OB_INVALID_ID != dblink_id, OB_DBLINK_NOT_EXIST_TO_ACCESS, dblink_id);
  OZ (get_dblink_info(dblink_id, dblink_info));
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(dblink_info)) {
    ObPLDbLinkInfo *new_dblink_info = static_cast<ObPLDbLinkInfo *>(alloc_.alloc(sizeof(ObPLDbLinkInfo)));
    if (OB_ISNULL(new_dblink_info)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate memory failed", K(ret));
    } else {
      new_dblink_info = new (new_dblink_info)ObPLDbLinkInfo();
      new_dblink_info->set_dblink_id(dblink_id);
      dblink_info = new_dblink_info;
      OZ (dblink_infos_.push_back(dblink_info));
    }
  }
  OZ ((const_cast<ObPLDbLinkInfo *>(dblink_info))->get_routine_infos(session_info,
                                                                      schema_guard,
                                                                      alloc_,
                                                                      dblink_name,
                                                                      db_name,
                                                                      pkg_name,
                                                                      routine_name,
                                                                      routine_infos,
                                                                      next_link_object_id_));
  if (OB_SUCC(ret)) {
    bool is_all_func = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < routine_infos.count(); i++) {
      const ObRoutineInfo *r = static_cast<const ObRoutineInfo *>(routine_infos.at(i));
      CK (OB_NOT_NULL(r));
      if (OB_SUCC(ret) && ObRoutineType::ROUTINE_PROCEDURE_TYPE == r->get_routine_type()) {
        is_all_func = false;
        break;
      }
    }
    if (OB_SUCC(ret) && is_all_func) {
      ret = OB_ERR_NOT_VALID_ROUTINE_NAME;
      LOG_WARN("ORA-06576: not a valid function or procedure name", K(ret), K(pkg_name), K(routine_name));
    }
  }
#endif
  return ret;
}

int ObPLDbLinkGuard::get_dblink_routine_info(uint64_t dblink_id,
                                             uint64_t pkg_id,
                                             uint64_t routine_id,
                                             const share::schema::ObRoutineInfo *&routine_info)
{
  int ret = OB_SUCCESS;
#ifndef OB_BUILD_ORACLE_PL
  ret = OB_NOT_SUPPORTED;
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "PL dblink");
#else
  const ObPLDbLinkInfo *dblink_info = NULL;
  if (OB_FAIL(get_dblink_info(dblink_id, dblink_info))) {
    LOG_WARN("get dblink info failed", K(ret), K(dblink_id));
  } else if (OB_ISNULL(dblink_info)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dblink_info is null", K(ret), K(dblink_id));
  } else if (OB_FAIL(dblink_info->get_routine_info(pkg_id, routine_id, routine_info))) {
    LOG_WARN("get routine info failed", K(ret), KP(dblink_info), K(pkg_id), K(routine_id));
  }
#endif
  return ret;
}

int ObPLDbLinkGuard::dblink_name_resolve(common::ObDbLinkProxy *dblink_proxy,
                                         common::sqlclient::ObISQLConnection *dblink_conn,
                                         const ObDbLinkSchema *dblink_schema,
                                         const common::ObString &full_name,
                                         common::ObString &schema,
                                         common::ObString &object_name,
                                         common::ObString &sub_object_name,
                                         int64_t &object_type,
                                         ObIAllocator &alloctor)
{
  int ret = OB_SUCCESS;
#ifndef OB_BUILD_ORACLE_PL
  ret = OB_NOT_SUPPORTED;
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "PL dblink");
#else
  /*
  * dbms_utility.sql
  * PROCEDURE NAME_RESOLVE (NAME IN VARCHAR2,
  *                         CONTEXT IN NUMBER,
  *                         SCHEMA1 OUT VARCHAR2,
  *                         PART1 OUT VARCHAR2,
  *                         PART2 OUT VARCHAR2,
  *                         DBLINK OUT VARCHAR2,
  *                         PART1_TYPE OUT NUMBER,
  *                         OBJECT_NUMBER OUT NUMBER);
  *
  */
  const char *call_proc = "declare "
                          " object_number number; "
                          "begin "
                          " dbms_utility.name_resolve(:name, "
                          "                           :context, "
                          "                           :schema1, "
                          "                           :part1, "
                          "                           :part2, "
                          "                           :dblink, "
                          "                           :part1_type, "
                          "                           object_number); "
                          "end; ";
  if (OB_ISNULL(dblink_proxy)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dblink_proxy is NULL", K(ret));
  } else if (OB_FAIL(dblink_proxy->dblink_prepare(dblink_conn, call_proc))) {
    LOG_WARN("prepare sql failed", K(ret), K(ObString(call_proc)));
  }
  if (OB_SUCC(ret)) {
    ObString full_name_copy = full_name;
    const int64_t ident_size = pl::OB_MAX_PL_IDENT_LENGTH + 1;
    int context = 1;
    char schema1[ident_size];
    char part1[ident_size];
    char part2[ident_size];
    char dblink[ident_size];
    int part1_type = -1;
    int32_t indicator = 0;
    memset(schema1, 0, ident_size);
    memset(part1, 0, ident_size);
    memset(part2, 0, ident_size);
    memset(dblink, 0, ident_size);
    int32_t oci_sql_str = static_cast<int32_t>(OciDataType::OCI_SQLT_STR);
    int32_t oci_sql_int = static_cast<int32_t>(OciDataType::OCI_SQLT_INT);
#define BIND_BASIC_BY_POS(param_pos, param, param_size, param_type)         \
    if (FAILEDx(dblink_proxy->dblink_bind_basic_type_by_pos(dblink_conn,    \
                                                            param_pos,      \
                                                            param,          \
                                                            param_size,     \
                                                            param_type,     \
                                                            indicator))) {  \
      LOG_WARN("bind param failed", K(ret), K(param_pos), K(param_size), K(param_type)); \
    }
    BIND_BASIC_BY_POS(1, full_name_copy.ptr(), static_cast<int64_t>(full_name_copy.length() + 1), oci_sql_str);
    BIND_BASIC_BY_POS(2, &context, static_cast<int64_t>(sizeof(int)), oci_sql_int);
    BIND_BASIC_BY_POS(3, schema1, ident_size, oci_sql_str);
    BIND_BASIC_BY_POS(4, part1, ident_size, oci_sql_str);
    BIND_BASIC_BY_POS(5, part2, ident_size, oci_sql_str);
    BIND_BASIC_BY_POS(6, dblink, ident_size, oci_sql_str);
    BIND_BASIC_BY_POS(7, &part1_type, static_cast<int64_t>(sizeof(int)), oci_sql_int);
    if (FAILEDx(dblink_proxy->dblink_execute_proc(dblink_conn))) {
      const DblinkDriverProto link_type = static_cast<DblinkDriverProto>(dblink_schema->get_driver_proto());
      LOG_WARN("read link failed", K(ret), K(ObString(call_proc)));
    } else {
      switch (part1_type) {
        case OracleObjectType::ORA_PROCEUDRE:
          // procedure
          object_type = static_cast<int64_t>(ObObjectType::PROCEDURE);
        break;
        case OracleObjectType::ORA_PACKAGE:
          // package
          object_type = static_cast<int64_t>(ObObjectType::PACKAGE);
        break;
        default: {
          ret = OB_ERR_NOT_VALID_ROUTINE_NAME;
          LOG_WARN("remote object type not support", K(ret), K(full_name));
        }
      }
      OZ (ob_write_string(alloctor, ObString(schema1), schema));
      OZ (ob_write_string(alloctor, ObString(part1), object_name));
      OZ (ob_write_string(alloctor, ObString(part2), sub_object_name));
    }
#undef BIND_BASIC_BY_POS
  }
  if ((NULL != dblink_conn)) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = static_cast<ObOciConnection *>(dblink_conn)->free_oci_stmt())) {
      LOG_WARN("failed to close oci result", K(tmp_ret));
      if (OB_SUCC(ret)) {
        ret = tmp_ret;
      }
    }
  }
#endif
  return ret;
}

int ObPLDbLinkGuard::get_dblink_type_by_name(sql::ObSQLSessionInfo &session_info,
                                             share::schema::ObSchemaGetterGuard &schema_guard,
                                             const common::ObString &dblink_name,
                                             const common::ObString &db_name,
                                             const common::ObString &pkg_name,
                                             const common::ObString &udt_name,
                                             const pl::ObUserDefinedType *&udt)
{
  int ret = OB_SUCCESS;
#ifndef OB_BUILD_ORACLE_PL
  ret = OB_NOT_SUPPORTED;
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "PL dblink");
#else
  const uint64_t tenant_id = MTL_ID();
  uint64_t dblink_id = OB_INVALID_ID;
  const share::schema::ObDbLinkSchema *dblink_schema = NULL;
  const ObPLDbLinkInfo *dblink_info = NULL;
  OZ (schema_guard.get_dblink_schema(tenant_id, dblink_name, dblink_schema));
  OV (OB_NOT_NULL(dblink_schema), OB_DBLINK_NOT_EXIST_TO_ACCESS, dblink_name);
  OV (OB_INVALID_ID != (dblink_id = dblink_schema->get_dblink_id()), OB_DBLINK_NOT_EXIST_TO_ACCESS, dblink_id);
  OZ (get_dblink_info(dblink_id, dblink_info));
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(dblink_info)) {
    ObPLDbLinkInfo *new_dblink_info = static_cast<ObPLDbLinkInfo *>(alloc_.alloc(sizeof(ObPLDbLinkInfo)));
    if (OB_ISNULL(new_dblink_info)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate memory failed", K(ret));
    } else {
      new_dblink_info = new (new_dblink_info)ObPLDbLinkInfo();
      new_dblink_info->set_dblink_id(dblink_id);
      dblink_info = new_dblink_info;
      OZ (dblink_infos_.push_back(dblink_info));
    }
  }
  OZ ((const_cast<ObPLDbLinkInfo *>(dblink_info))->get_udt_by_name(session_info,
       schema_guard, alloc_, dblink_name, db_name, pkg_name,
       udt_name, udt, next_link_object_id_));
#endif
  return ret;
}

int ObPLDbLinkGuard::get_dblink_type_by_id(const uint64_t mask_dblink_id,
                                           const uint64_t udt_id,
                                           const pl::ObUserDefinedType *&udt)
{
  int ret = OB_SUCCESS;
#ifndef OB_BUILD_ORACLE_PL
  ret = OB_NOT_SUPPORTED;
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "PL dblink");
#else
  uint64_t dblink_id = mask_dblink_id & ~common::OB_MOCK_DBLINK_UDT_ID_MASK;
  const ObPLDbLinkInfo *dblink_info = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && OB_ISNULL(dblink_info) && i < dblink_infos_.count(); i++) {
    if (OB_ISNULL(dblink_infos_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("dblink_info is null", K(ret), K(i));
    } else if (dblink_id == dblink_infos_.at(i)->get_dblink_id()) {
      dblink_info = dblink_infos_.at(i);
      OZ (dblink_info->get_udt_by_id(udt_id, udt));
    }
  }
#endif
  return ret;
}

int ObPLDbLinkGuard::get_dblink_type_by_name(const uint64_t dblink_id,
                                             const common::ObString &db_name,
                                             const common::ObString &pkg_name,
                                             const common::ObString &udt_name,
                                             const pl::ObUserDefinedType *&udt)
{
  int ret = OB_SUCCESS;
#ifndef OB_BUILD_ORACLE_PL
  ret = OB_NOT_SUPPORTED;
  LOG_USER_ERROR(OB_NOT_SUPPORTED, "PL dblink");
#else
  const ObPLDbLinkInfo *dblink_info = NULL;
  for (int64_t i = 0; OB_SUCC(ret) && i < dblink_infos_.count(); i++) {
    if (OB_ISNULL(dblink_infos_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("dblink_info is null", K(ret), K(i));
    } else if (dblink_id == dblink_infos_.at(i)->get_dblink_id()) {
      dblink_info = dblink_infos_.at(i);
      bool find_pkg = false;
      OZ (dblink_info->get_udt_from_cache(db_name, pkg_name, udt_name, udt, find_pkg));
      break;
    }
  }
#endif
  return ret;
}

#ifdef OB_BUILD_ORACLE_PL
int ObPLDbLinkGuard::get_dblink_info(const uint64_t dblink_id,
                                     const ObPLDbLinkInfo *&dblink_info)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && OB_ISNULL(dblink_info) && i < dblink_infos_.count(); i++) {
    if (OB_ISNULL(dblink_infos_.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("dblink_info is null", K(ret), K(i));
    } else {
      dblink_info = dblink_infos_.at(i);
    }
  }
  return ret;
}
#endif

}
}
