/**
 * Copyright (c) 2023 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#define USING_LOG_PREFIX SHARE
#define protected public
#define private public

#include "env/ob_simple_cluster_test_base.h"
#include "lib/ob_errno.h"
#include "share/location_cache/ob_location_service.h" // ObLocationService

namespace oceanbase
{
using namespace unittest;
namespace share
{
using namespace common;

static const int64_t TOTAL_NUM = 110;
static uint64_t g_tenant_id;
static ObSEArray<ObTabletLSPair, TOTAL_NUM> g_tablet_ls_pairs;

class TestLocationService : public unittest::ObSimpleClusterTestBase
{
public:
  TestLocationService() : unittest::ObSimpleClusterTestBase("test_location_service") {}
  int batch_create_table(ObMySQLProxy &sql_proxy, const int64_t TOTAL_NUM, ObIArray<ObTabletLSPair> &tablet_ls_pairs);
};

int TestLocationService::batch_create_table(
    ObMySQLProxy &sql_proxy,
    const int64_t TOTAL_NUM,
    ObIArray<ObTabletLSPair> &tablet_ls_pairs)
{
  int ret = OB_SUCCESS;
  tablet_ls_pairs.reset();
  ObSqlString sql;
  // batch create table
  int64_t affected_rows = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < TOTAL_NUM; ++i) {
    sql.reset();
    if (OB_FAIL(sql.assign_fmt("create table t%ld(c1 int)", i))) {
    } else if (OB_FAIL(sql_proxy.write(sql.ptr(), affected_rows))) {
    }
  }
  // batch get table_id
  sql.reset();
  if (OB_FAIL(sql.assign_fmt("select TABLET_ID, LS_ID from oceanbase.DBA_OB_TABLE_LOCATIONS where table_name in ("))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < TOTAL_NUM; ++i) {
      if (OB_FAIL(sql.append_fmt("%s't%ld'", 0 == i ? "" : ",", i))) {}
    }
    if (FAILEDx(sql.append_fmt(") order by TABLET_ID"))) {};
  }
  SMART_VAR(ObMySQLProxy::MySQLResult, result) {
    if (OB_FAIL(tablet_ls_pairs.reserve(TOTAL_NUM))) {
    } else if (OB_UNLIKELY(!is_valid_tenant_id(g_tenant_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql_proxy.read(result, sql.ptr()))) {
    } else if (OB_ISNULL(result.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sqlclient::ObMySQLResult &res = *result.get_result();
      uint64_t tablet_id = ObTabletID::INVALID_TABLET_ID;
      int64_t ls_id = ObLSID::INVALID_LS_ID;
      while(OB_SUCC(ret) && OB_SUCC(res.next())) {
        EXTRACT_INT_FIELD_MYSQL(res, "TABLET_ID", tablet_id, uint64_t);
        EXTRACT_INT_FIELD_MYSQL(res, "LS_ID", ls_id, int64_t);
        if (OB_FAIL(tablet_ls_pairs.push_back(ObTabletLSPair(tablet_id, ls_id)))) {}
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("fail to generate data", K(sql));
      }
    }
  }
  return ret;
}

TEST_F(TestLocationService, prepare_data)
{
  g_tenant_id = OB_INVALID_TENANT_ID;
  ASSERT_EQ(OB_SUCCESS, create_tenant());
  ASSERT_EQ(OB_SUCCESS, get_tenant_id(g_tenant_id));
  ASSERT_EQ(OB_SUCCESS, get_curr_simple_server().init_sql_proxy2());
  ObMySQLProxy &sql_proxy = get_curr_simple_server().get_sql_proxy2();
  ASSERT_EQ(OB_SUCCESS, batch_create_table(sql_proxy, TOTAL_NUM, g_tablet_ls_pairs));
}

TEST_F(TestLocationService, test_ls_location_service)
{
  int ret = OB_SUCCESS;
  ASSERT_TRUE(is_valid_tenant_id(g_tenant_id));
  ObLocationService *location_service = GCTX.location_service_;
  ASSERT_TRUE(OB_NOT_NULL(location_service));
  ObLSLocationService *ls_location_service = &(location_service->ls_location_service_);
  ASSERT_TRUE(OB_NOT_NULL(ls_location_service));

  ObSEArray<ObLSID, 2> ls_ids;
  ObSEArray<ObLSLocation, 2> ls_locations;
  ASSERT_EQ(OB_SUCCESS, ls_ids.push_back(ObLSID(1)));
  ASSERT_EQ(OB_SUCCESS, ls_ids.push_back(ObLSID(1001)));
  ASSERT_EQ(OB_SUCCESS, ls_location_service->batch_renew_ls_locations(GCONF.cluster_id, g_tenant_id, ls_ids, ls_locations));
  ASSERT_TRUE(ls_ids.count() == ls_locations.count());
  ARRAY_FOREACH(ls_locations, idx) {
    ASSERT_TRUE(ls_locations.at(idx).get_ls_id() == ls_ids.at(idx));
    ASSERT_TRUE(!ls_locations.at(idx).get_replica_locations().empty());
  }

  // nonexistent ls_id return fake empty location
  ls_ids.reset();
  ls_locations.reset();
  ASSERT_EQ(OB_SUCCESS, ls_ids.push_back(ObLSID(1)));
  ASSERT_EQ(OB_SUCCESS, ls_ids.push_back(ObLSID(1234))); // nonexistent ls_id
  ASSERT_EQ(OB_SUCCESS, ls_location_service->batch_renew_ls_locations(GCONF.cluster_id, g_tenant_id, ls_ids, ls_locations));
  ASSERT_TRUE(ls_ids.count() == ls_locations.count() + 1);
  ASSERT_TRUE(ls_locations.at(0).get_ls_id() == ls_ids.at(0));
  ASSERT_TRUE(!ls_locations.at(0).get_replica_locations().empty());

  // duplicated ls_id return error
  ls_ids.reset();
  ls_locations.reset();
  ASSERT_EQ(OB_SUCCESS, ls_ids.push_back(ObLSID(1001)));
  ASSERT_EQ(OB_SUCCESS, ls_ids.push_back(ObLSID(1001))); // duplicated ls_id
  ASSERT_EQ(OB_ERR_DUP_ARGUMENT, ls_location_service->batch_renew_ls_locations(GCONF.cluster_id, g_tenant_id, ls_ids, ls_locations));
}

TEST_F(TestLocationService, test_tablet_ls_service)
{
  int ret = OB_SUCCESS;
  ASSERT_TRUE(g_tablet_ls_pairs.count() == TOTAL_NUM);
  ASSERT_TRUE(is_valid_tenant_id(g_tenant_id));
  ObLocationService *location_service = GCTX.location_service_;
  ASSERT_TRUE(OB_NOT_NULL(location_service));
  ObTabletLSService *tablet_ls_service = &(location_service->tablet_ls_service_);
  ASSERT_TRUE(OB_NOT_NULL(tablet_ls_service));

  ObArenaAllocator allocator;
  ObList<ObTabletID, ObIAllocator> tablet_list(allocator);
  ObSEArray<ObTabletLSCache, TOTAL_NUM> tablet_ls_caches;
  ARRAY_FOREACH(g_tablet_ls_pairs, idx) {
    const ObTabletLSPair &pair = g_tablet_ls_pairs.at(idx);
    ASSERT_EQ(OB_SUCCESS, tablet_list.push_back(pair.get_tablet_id()));
  }
  ASSERT_EQ(OB_SUCCESS, tablet_ls_service->batch_renew_tablet_ls_cache(g_tenant_id, tablet_list, tablet_ls_caches));
  ASSERT_TRUE(tablet_list.size() == tablet_ls_caches.count());
  ASSERT_TRUE(tablet_ls_caches.count() == g_tablet_ls_pairs.count());
  ARRAY_FOREACH(tablet_ls_caches, idx) {
    const ObTabletLSCache &cache = tablet_ls_caches.at(idx);
    bool find = false;
    FOREACH(tablet_id, tablet_list) {
      if (*tablet_id == cache.get_tablet_id()) {
        find = true;
      }
    }
    ASSERT_TRUE(find);
    ARRAY_FOREACH(g_tablet_ls_pairs, j) {
      const ObTabletLSPair &pair = g_tablet_ls_pairs.at(j);
      if (pair.get_tablet_id() == cache.get_tablet_id()) {
        ASSERT_TRUE(pair.get_ls_id() == cache.get_ls_id());
        break;
      }
    }
  }

  // nonexistent tablet_id return OB_SUCCESS
  tablet_list.reset();
  tablet_ls_caches.reset();
  ASSERT_EQ(OB_SUCCESS, tablet_list.push_back(g_tablet_ls_pairs.at(0).get_tablet_id()));
  ASSERT_EQ(OB_SUCCESS, tablet_list.push_back(ObTabletID(654321))); // nonexistent ls_id
  ASSERT_EQ(OB_SUCCESS, tablet_ls_service->batch_renew_tablet_ls_cache(g_tenant_id, tablet_list, tablet_ls_caches));
  ASSERT_TRUE(tablet_ls_caches.count() == 1);
  ASSERT_TRUE(tablet_ls_caches.at(0).get_tablet_id() == g_tablet_ls_pairs.at(0).get_tablet_id());
  ASSERT_TRUE(tablet_ls_caches.at(0).get_ls_id() == g_tablet_ls_pairs.at(0).get_ls_id());

  // duplicated tablet_id return error
  tablet_list.reset();
  tablet_ls_caches.reset();
  ASSERT_EQ(OB_SUCCESS, tablet_list.push_back(g_tablet_ls_pairs.at(0).get_tablet_id()));
  ASSERT_EQ(OB_SUCCESS, tablet_list.push_back(g_tablet_ls_pairs.at(0).get_tablet_id()));
  ASSERT_EQ(OB_SUCCESS, tablet_ls_service->batch_renew_tablet_ls_cache(g_tenant_id, tablet_list, tablet_ls_caches));
}

TEST_F(TestLocationService, test_location_service)
{
  int ret = OB_SUCCESS;
  ASSERT_TRUE(g_tablet_ls_pairs.count() == TOTAL_NUM);
  ASSERT_TRUE(is_valid_tenant_id(g_tenant_id));
  ObLocationService *location_service = GCTX.location_service_;
  ASSERT_TRUE(OB_NOT_NULL(location_service));

  ObArenaAllocator allocator;
  ObList<ObTabletID, ObIAllocator> tablet_list(allocator);
  ObSEArray<ObTabletLSCache, TOTAL_NUM> tablet_ls_caches;
  ARRAY_FOREACH(g_tablet_ls_pairs, idx) {
    const ObTabletLSPair &pair = g_tablet_ls_pairs.at(idx);
    ASSERT_EQ(OB_SUCCESS, tablet_list.push_back(pair.get_tablet_id()));
  }

  ASSERT_EQ(OB_SUCCESS, location_service->batch_renew_tablet_locations(g_tenant_id, tablet_list, OB_MAPPING_BETWEEN_TABLET_AND_LS_NOT_EXIST, true));
  ASSERT_EQ(OB_SUCCESS, location_service->batch_renew_tablet_locations(g_tenant_id, tablet_list, OB_MAPPING_BETWEEN_TABLET_AND_LS_NOT_EXIST, false));
  ASSERT_EQ(OB_SUCCESS, location_service->batch_renew_tablet_locations(g_tenant_id, tablet_list, OB_NOT_MASTER, true));
  ASSERT_EQ(OB_SUCCESS, location_service->batch_renew_tablet_locations(g_tenant_id, tablet_list, OB_NOT_MASTER, false));
}

TEST_F(TestLocationService, test_check_ls_exist)
{
  uint64_t user_tenant_id = OB_INVALID_TENANT_ID;
  ASSERT_EQ(OB_SUCCESS, get_tenant_id(user_tenant_id));
  uint64_t meta_tenant_id = gen_meta_tenant_id(user_tenant_id);

  ObLSID user_ls_id(1001);
  ObLSID uncreated_ls_id(6666);
  ObLSID invalid_ls_id(123);
  ObLSID test_creating_ls_id(1111);
  ObLSID test_creat_abort_ls_id(1112);
  const uint64_t not_exist_tenant_id = 1234;
  ObLSExistState state;

  // user tenant
  ASSERT_EQ(OB_SUCCESS, ObLocationService::check_ls_exist(user_tenant_id, SYS_LS, state));
  ASSERT_TRUE(state.is_existing());
  state.reset();
  ASSERT_EQ(OB_SUCCESS, ObLocationService::check_ls_exist(user_tenant_id, user_ls_id, state));
  ASSERT_TRUE(state.is_existing());
  state.reset();
  ASSERT_EQ(OB_SUCCESS, ObLocationService::check_ls_exist(user_tenant_id, uncreated_ls_id, state));
  ASSERT_TRUE(state.is_uncreated());

  common::ObMySQLProxy &inner_proxy = get_curr_simple_server().get_observer().get_mysql_proxy();
  ObSqlString sql;
  int64_t affected_rows = 0;
  ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("delete from oceanbase.__all_ls_status where tenant_id = %lu and ls_id = %ld", user_tenant_id, user_ls_id.id()));
  ASSERT_EQ(OB_SUCCESS, inner_proxy.write(get_private_table_exec_tenant_id(user_tenant_id), sql.ptr(), affected_rows));
  state.reset();
  ASSERT_EQ(OB_SUCCESS, ObLocationService::check_ls_exist(user_tenant_id, user_ls_id, state));
  ASSERT_TRUE(state.is_deleted());

  ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("insert into oceanbase.__all_ls_status (tenant_id, ls_id, status, ls_group_id, unit_group_id) values (%lu, %ld, 'CREATING', 0, 0)", user_tenant_id, test_creating_ls_id.id()));
  ASSERT_EQ(OB_SUCCESS, inner_proxy.write(get_private_table_exec_tenant_id(user_tenant_id), sql.ptr(), affected_rows));
  state.reset();
  ASSERT_EQ(OB_SUCCESS, ObLocationService::check_ls_exist(user_tenant_id, test_creating_ls_id, state));
  ASSERT_TRUE(state.is_uncreated()); // treat CREATING ls as UNCREATED

  ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("insert into oceanbase.__all_ls_status (tenant_id, ls_id, status, ls_group_id, unit_group_id) values (%lu, %ld, 'CREATE_ABORT', 0, 0)", user_tenant_id, test_creat_abort_ls_id.id()));
  ASSERT_EQ(OB_SUCCESS, inner_proxy.write(get_private_table_exec_tenant_id(user_tenant_id), sql.ptr(), affected_rows));
  state.reset();
  ASSERT_EQ(OB_SUCCESS, ObLocationService::check_ls_exist(user_tenant_id, test_creat_abort_ls_id, state));
  ASSERT_TRUE(state.is_deleted()); // treat CREATE_ABLORT ls as DELETED

  ASSERT_EQ(OB_INVALID_ARGUMENT, ObLocationService::check_ls_exist(user_tenant_id, invalid_ls_id, state));

  // sys tenant
  state.reset();
  ASSERT_EQ(OB_SUCCESS, ObLocationService::check_ls_exist(OB_SYS_TENANT_ID, SYS_LS, state));
  ASSERT_TRUE(state.is_existing());
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObLocationService::check_ls_exist(OB_SYS_TENANT_ID, user_ls_id, state));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObLocationService::check_ls_exist(OB_SYS_TENANT_ID, invalid_ls_id, state));

  // not exist tenant
  ASSERT_EQ(OB_TENANT_NOT_EXIST, ObLocationService::check_ls_exist(not_exist_tenant_id, SYS_LS, state));
  // virtual tenant
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObLocationService::check_ls_exist(OB_SERVER_TENANT_ID, SYS_LS, state));

  // meta tenant
  state.reset();
  ASSERT_EQ(OB_SUCCESS, ObLocationService::check_ls_exist(meta_tenant_id, SYS_LS, state));
  ASSERT_TRUE(state.is_existing());
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObLocationService::check_ls_exist(meta_tenant_id, user_ls_id, state));
  ASSERT_EQ(OB_INVALID_ARGUMENT, ObLocationService::check_ls_exist(meta_tenant_id, invalid_ls_id, state));

  // meta tenant not in normal status
  state.reset();
  ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("alter system set_tp tp_name = EN_CHECK_LS_EXIST_WITH_TENANT_NOT_NORMAL, error_code = 4016, frequency = 1"));
  ASSERT_EQ(OB_SUCCESS, inner_proxy.write(OB_SYS_TENANT_ID, sql.ptr(), affected_rows));
  ASSERT_EQ(OB_SUCCESS, ObLocationService::check_ls_exist(meta_tenant_id, SYS_LS, state));
  ASSERT_TRUE(state.is_existing());

  ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("update oceanbase.__all_ls_status set status = 'CREATING' where tenant_id = %lu and ls_id = %ld", meta_tenant_id, ObLSID::SYS_LS_ID));
  ASSERT_EQ(OB_SUCCESS, inner_proxy.write(get_private_table_exec_tenant_id(meta_tenant_id), sql.ptr(), affected_rows));
  state.reset();
  ASSERT_EQ(OB_SUCCESS, ObLocationService::check_ls_exist(meta_tenant_id, SYS_LS, state));
  ASSERT_TRUE(state.is_uncreated());

  ASSERT_EQ(OB_SUCCESS, sql.assign_fmt("delete from oceanbase.__all_ls_status where tenant_id = %lu and ls_id = %ld", meta_tenant_id, ObLSID::SYS_LS_ID));
  ASSERT_EQ(OB_SUCCESS, inner_proxy.write(get_private_table_exec_tenant_id(meta_tenant_id), sql.ptr(), affected_rows));
  state.reset();
  ASSERT_EQ(OB_SUCCESS, ObLocationService::check_ls_exist(meta_tenant_id, SYS_LS, state));
  ASSERT_TRUE(state.is_uncreated());
}

} // namespace rootserver
} // namespace oceanbase
int main(int argc, char **argv)
{
  oceanbase::unittest::init_log_and_gtest(argc, argv);
  OB_LOGGER.set_log_level("INFO");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
