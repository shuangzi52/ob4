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

#define USING_LOG_PREFIX STORAGE

#include "lib/oblog/ob_log.h"
#include "lib/utility/ob_tracepoint.h"
#include "share/ob_thread_mgr.h"
#include "storage/tx_storage/ob_tablet_gc_service.h"
#include "storage/checkpoint/ob_data_checkpoint.h"
#include "storage/tablet/ob_tablet_iterator.h"
#include "storage/tx_storage/ob_tenant_freezer.h"
#include "storage/tx_storage/ob_ls_map.h"     // ObLSIterator
#include "storage/tx_storage/ob_ls_service.h" // ObLSService
#include "logservice/ob_log_service.h"
#include "logservice/palf/log_define.h"
#include "observer/ob_server_event_history_table_operator.h"
#include "share/ob_tenant_info_proxy.h"
#include "storage/tablet/ob_tablet.h" // ObTablet
#include "rootserver/ob_tenant_info_loader.h"
#include "share/ob_tenant_info_proxy.h"
#include "storage/meta_mem/ob_tenant_meta_mem_mgr.h"

namespace oceanbase
{
using namespace share;
namespace storage
{
namespace checkpoint
{

// The time interval for waiting persist tablet successful after freezing is 5s
const int64_t ObTabletGCHandler::FLUSH_CHECK_INTERVAL = 5 * 1000 * 1000L;

// The time interval for waiting persist tablet successful after freezing in total is 72 * 5s = 6m
const int64_t ObTabletGCHandler::FLUSH_CHECK_MAX_TIMES = 72;

// The time interval for checking tablet_persist_trigger_ is 5s
const int64_t ObTabletGCService::GC_CHECK_INTERVAL = 5 * 1000 * 1000L;

// The time interval for gc tablet and persist tablet whether the tablet_persist_trigger_ is 24 * 720 * 5s = 1d
const int64_t ObTabletGCService::GLOBAL_GC_CHECK_INTERVAL_TIMES = 24 * 720;

int ObTabletGCService::mtl_init(ObTabletGCService* &m)
{
  return m->init();
}

int ObTabletGCService::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    STORAGE_LOG(WARN, "ObTabletGCService init twice.", KR(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObTabletGCService::start()
{
  int ret = OB_SUCCESS;
  timer_for_tablet_change_.set_run_wrapper(MTL_CTX());
  timer_for_tablet_shell_.set_run_wrapper(MTL_CTX());
  if (OB_FAIL(timer_for_tablet_change_.init())) {
    STORAGE_LOG(ERROR, "fail to init timer", KR(ret));
  } else if (OB_FAIL(timer_for_tablet_shell_.init("TabletShellTimer", ObMemAttr(MTL_ID(), "TabShellTimer")))) {
    STORAGE_LOG(ERROR, "fail to init timer", KR(ret));
  } else if (OB_FAIL(timer_for_tablet_change_.schedule(tablet_change_task_, GC_CHECK_INTERVAL, true))) {
    STORAGE_LOG(ERROR, "fail to schedule task", KR(ret));
  } else if (OB_FAIL(timer_for_tablet_shell_.schedule(tablet_shell_task_, ObEmptyShellTask::GC_EMPTY_TABLET_SHELL_INTERVAL, true))) {
    STORAGE_LOG(ERROR, "fail to schedule task", KR(ret));
  }
  return ret;
}

int ObTabletGCService::stop()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObTabletGCService is not initialized", KR(ret));
  } else {
    STORAGE_LOG(INFO, "ObTabletGCService stoped");
  }
  timer_for_tablet_change_.stop();
  timer_for_tablet_shell_.stop();
  return ret;
}

void ObTabletGCService::wait()
{
  timer_for_tablet_change_.wait();
  timer_for_tablet_shell_.wait();
}

void ObTabletGCService::destroy()
{
  is_inited_ = false;
  timer_for_tablet_change_.destroy();
  timer_for_tablet_shell_.destroy();
}

void ObTabletGCService::ObTabletChangeTask::runTimerTask()
{
  STORAGE_LOG(INFO, "====== [tabletchange] timer task ======", K(GC_CHECK_INTERVAL));
  RLOCAL_STATIC(int64_t, times) = 0;
  times = (times + 1) % GLOBAL_GC_CHECK_INTERVAL_TIMES;
  int ret = OB_SUCCESS;
  ObLSIterator *iter = NULL;
  common::ObSharedGuard<ObLSIterator> guard;
  ObLSService *ls_svr = MTL(ObLSService*);
  bool skip_gc_task = false;

  skip_gc_task = (OB_SUCCESS != (OB_E(EventTable::EN_TABLET_GC_TASK_FAILED) OB_SUCCESS));

  if (OB_ISNULL(ls_svr)) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "mtl ObLSService should not be null", KR(ret));
  } else if (OB_UNLIKELY(skip_gc_task)) {
    // do nothing
  } else if (OB_FAIL(ls_svr->get_ls_iter(guard, ObLSGetMod::TXSTORAGE_MOD))) {
    STORAGE_LOG(WARN, "get log stream iter failed", KR(ret));
  } else if (OB_ISNULL(iter = guard.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "iter is NULL", KR(ret));
  } else {
    ObLS *ls = NULL;
    int ls_cnt = 0;
    for (; OB_SUCC(iter->get_next(ls)); ++ls_cnt) {
      ObTabletGCHandler *tablet_gc_handler = NULL;
      if (OB_ISNULL(ls)) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "ls is NULL", KR(ret));
      } else if (FALSE_IT(tablet_gc_handler = ls->get_tablet_gc_handler())) {
      } else if (tablet_gc_handler->check_stop()) {
        STORAGE_LOG(INFO, "[tabletchange] tablet_gc_handler is offline", K(ls->get_ls_id()));
      } else {
        uint8_t tablet_persist_trigger = tablet_gc_handler->get_tablet_persist_trigger_and_reset();
        STORAGE_LOG(INFO, "[tabletchange] task check ls", K(ls->get_ls_id()), K(tablet_persist_trigger));
        if (times == 0
            || ObTabletGCHandler::is_set_tablet_persist_trigger(tablet_persist_trigger)
            || ObTabletGCHandler::is_tablet_gc_trigger(tablet_persist_trigger)) {
          const bool only_persist = 0 != times && !ObTabletGCHandler::is_tablet_gc_trigger(tablet_persist_trigger);
          obsys::ObRLockGuard lock(tablet_gc_handler->wait_lock_);
          bool need_retry = false;
          SCN decided_scn;
          ObFreezer *freezer = ls->get_freezer();
          common::ObTabletIDArray unpersist_tablet_ids;
          common::ObSEArray<ObTabletHandle, 16> deleted_tablets;
          const bool is_deleted = true;

          if (OB_ISNULL(freezer)) {
            ret = OB_ERR_UNEXPECTED;
            STORAGE_LOG(WARN, "freezer should not null", K(ls->get_ls_id()), KR(ret));
          }
          // 1. get minor merge point
          else if (OB_FAIL(ls->get_max_decided_scn(decided_scn))) {
            need_retry = true;
            STORAGE_LOG(WARN, "decide_max_decided_scn failed", KR(ret), K(freezer->get_ls_id()));
          } else if (!decided_scn.is_valid()
                     || SCN::min_scn() == decided_scn
                     || decided_scn < ls->get_tablet_change_checkpoint_scn()) {
            STORAGE_LOG(INFO, "no any log callback and no need to update clog checkpoint",
              K(freezer->get_ls_id()), K(decided_scn), KPC(ls), K(ls->get_ls_meta()));
          }
          // 2. get gc tablet and get unpersist_tablet_ids
          else if (OB_FAIL(tablet_gc_handler->get_unpersist_tablet_ids(deleted_tablets, unpersist_tablet_ids, only_persist, need_retry, decided_scn))) {
            need_retry = true;
            STORAGE_LOG(WARN, "failed to get_unpersist_tablet_ids", KPC(ls), KR(ret));
          }
          // 3. flush unpersit_tablet_ids
          else if (OB_FAIL(tablet_gc_handler->flush_unpersist_tablet_ids(unpersist_tablet_ids, decided_scn))) {
            need_retry = true;
            STORAGE_LOG(WARN, "failed to flush_unpersist_tablet_ids", KPC(ls), KR(ret), K(unpersist_tablet_ids));
          }
          // 4. update tablet_change_checkpoint in log meta
          else if (decided_scn > ls->get_tablet_change_checkpoint_scn()
                  && OB_FAIL(tablet_gc_handler->set_tablet_change_checkpoint_scn(decided_scn))) {
            need_retry = true;
            STORAGE_LOG(WARN, "failed to set_tablet_change_checkpoint_scn", KPC(ls), KR(ret), K(decided_scn));
          }
          // 5. set ls transfer scn
          else if (!only_persist && OB_FAIL(tablet_gc_handler->set_ls_transfer_scn(deleted_tablets))) {
            need_retry = true;
            STORAGE_LOG(WARN, "failed to set ls transfer scn", KPC(ls), KR(ret), K(decided_scn));
          }
          // 6. check and gc deleted_tablets
          else if (!only_persist) {
            if (!deleted_tablets.empty() && OB_FAIL(tablet_gc_handler->gc_tablets(deleted_tablets))) {
              need_retry = true;
              STORAGE_LOG(WARN, "failed to gc tablet", KR(ret));
            }
          }
          STORAGE_LOG(INFO, "[tabletchange] tablet in a ls persist and gc process end", KR(ret), KPC(ls), K(decided_scn), K(unpersist_tablet_ids));
          if (need_retry) {
            STORAGE_LOG(INFO, "[tabletchange] persist or gc error, need try", KR(ret), KPC(ls), K(decided_scn), K(tablet_persist_trigger));
            if (ObTabletGCHandler::is_set_tablet_persist_trigger(tablet_persist_trigger)) {
              tablet_gc_handler->set_tablet_persist_trigger();
            }
            if (ObTabletGCHandler::is_tablet_gc_trigger(tablet_persist_trigger)) {
              tablet_gc_handler->set_tablet_gc_trigger();
            }
          }
        }
      }
    }
    if (ret == OB_ITER_END) {
      ret = OB_SUCCESS;
      if (ls_cnt > 0) {
        STORAGE_LOG(INFO, "[tabletchange] succeed to gc_tablet", KR(ret), K(ls_cnt), K(times));
      } else {
        STORAGE_LOG(INFO, "[tabletchange] no logstream", KR(ret), K(ls_cnt), K(times));
      }
    }
  }
}

int ObTabletGCHandler::init(ObLS *ls)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    STORAGE_LOG(WARN, "ObTabletGCHandler init twice", KR(ret));
  } else if (OB_ISNULL(ls)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "invalid argument", KR(ret));
  } else {
    ls_ = ls;
    is_inited_ = true;
  }
  return ret;
}

void ObTabletGCHandler::set_tablet_persist_trigger()
{
  uint8_t old_v = 0;
  uint8_t new_v = 0;
  do {
    old_v = ATOMIC_LOAD(&tablet_persist_trigger_);
    new_v = old_v | 1;
    if (old_v == new_v) {
      break;
    }
  } while (ATOMIC_CAS(&tablet_persist_trigger_, old_v, new_v) != old_v);
  STORAGE_LOG(INFO, "set_tablet_persist_trigger", KPC(this));
}

void ObTabletGCHandler::set_tablet_gc_trigger()
{
  uint8_t old_v = 0;
  uint8_t new_v = 0;
  do {
    old_v = ATOMIC_LOAD(&tablet_persist_trigger_);
    new_v = old_v | 2;
    if (old_v == new_v) {
      break;
    }
  } while (ATOMIC_CAS(&tablet_persist_trigger_, old_v, new_v) != old_v);
  STORAGE_LOG(INFO, "set_tablet_gc_trigger", KPC(this));
}

uint8_t ObTabletGCHandler::get_tablet_persist_trigger_and_reset()
{
  uint8_t old_v = 0;
  uint8_t new_v = 0;
  do {
    old_v = ATOMIC_LOAD(&tablet_persist_trigger_);
    if (old_v == new_v) {
      break;
    }
  } while (ATOMIC_CAS(&tablet_persist_trigger_, old_v, new_v) != old_v);
  return old_v;
}

int ObTabletGCHandler::disable_gc()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(gc_lock_.lock(GC_LOCK_TIMEOUT))) {
    ret = OB_TABLET_GC_LOCK_CONFLICT;
    LOG_WARN("try lock failed, please retry later", K(ret));
  } else if (check_stop()) {
    gc_lock_.unlock();
    ret = OB_NOT_RUNNING;
    LOG_WARN("gc handler has already been offline", K(ret));
  }

  return ret;
}

void ObTabletGCHandler::enable_gc()
{
  gc_lock_.unlock();
}

int ObTabletGCHandler::set_tablet_change_checkpoint_scn(const share::SCN &scn)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(gc_lock_.lock(GC_LOCK_TIMEOUT))) {
    ret = OB_TABLET_GC_LOCK_CONFLICT;
    LOG_WARN("try lock failed, please retry later", K(ret));
  } else {
    if (OB_FAIL(ls_->set_tablet_change_checkpoint_scn(scn))) {
      LOG_WARN("fail to set tablet_change_checkpoint_scn", K(ret), K(scn));
    } else {
      // do nothing
    }
    gc_lock_.unlock();
  }
  return ret;
}

bool ObTabletGCHandler::is_tablet_gc_trigger_and_reset()
{
  uint8_t old_v = 0;
  uint8_t new_v = 0;
  do {
    old_v = ATOMIC_LOAD(&tablet_persist_trigger_);
    new_v = old_v & (~2);
    if (old_v == new_v) {
      break;
    }
  } while (ATOMIC_CAS(&tablet_persist_trigger_, old_v, new_v) != old_v);
  return 0 != (old_v & 2);
}

int ObTabletGCHandler::check_tablet_need_persist_(
    ObTabletHandle &tablet_handle,
    bool &need_persist,
    bool &need_retry,
    const SCN &decided_scn)
{
  int ret = OB_SUCCESS;
  need_persist = false;
  ObTablet *tablet = NULL;
  bool is_locked = false;
  SCN rec_scn;
  if (OB_ISNULL(tablet = tablet_handle.get_obj())) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "tablet is NULL", KR(ret));
  } else if (OB_FAIL(tablet->is_locked_by_others<ObTabletCreateDeleteMdsUserData>(is_locked))) {
    LOG_WARN("failed to get is locked", KR(ret), KPC(tablet));
  } else if (is_locked) {
    LOG_INFO("tablet_status is changing", KR(ret), KPC(tablet));
    need_retry = true;
  }

  if (OB_FAIL(ret)) {
  } else if (tablet->is_empty_shell()) {
  } else if (OB_FAIL(tablet->get_mds_table_rec_log_scn(rec_scn))) {
    STORAGE_LOG(WARN, "failed to get_mds_table_rec_log_scn", KR(ret), KPC(tablet));
  } else if (!rec_scn.is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "rec_scn is invalid", KR(ret), KPC(tablet));
  } else if (rec_scn > decided_scn) {
    // todo check tablet_status and binding_info scn to mds_checkpoint_scn
  } else {
    need_persist = true;
    STORAGE_LOG(INFO, "[tabletgc] get tablet for persist", K(rec_scn), KPC(tablet));
  }

  return ret;
}

int ObTabletGCHandler::check_tablet_need_gc_(
    ObTabletHandle &tablet_handle,
    bool &need_gc,
    bool &need_retry,
    const SCN &decided_scn)
{
  int ret = OB_SUCCESS;
  need_gc = false;
  ObTablet *tablet = NULL;
  if (OB_ISNULL(tablet = tablet_handle.get_obj())) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "tablet is NULL", KR(ret));
  // for tablet shell
  } else if (tablet->is_empty_shell()) {
    SCN deleted_commit_scn = tablet->get_tablet_meta().mds_checkpoint_scn_;
    if (!deleted_commit_scn.is_valid()) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "deleted_commit_scn is unvalid", KR(ret), KPC(tablet));
    } else if (deleted_commit_scn > decided_scn) {
      need_retry = true;
      LOG_INFO("tablet cannot be gc, as deleted_commit_scn_ is more than decided_scn, retry", KR(ret), KPC(tablet), K(decided_scn));
    } else {
      LOG_INFO("tablet is shell, need gc", KR(ret), KPC(tablet), K(decided_scn));
      need_gc = true;
    }
  } else {
  // for create tablet abort
    ObTabletCreateDeleteMdsUserData data;
    bool mds_table_not_null = false;
    bool is_finish = false;
    if (OB_FAIL(tablet->check_mds_written(mds_table_not_null))) {
      STORAGE_LOG(WARN, "failed to check mds written", KR(ret), KPC(tablet));
    } else if (OB_FAIL(tablet->ObITabletMdsInterface::get_latest_tablet_status(data, is_finish))) {
      if (OB_EMPTY_RESULT == ret) {
        ret = OB_SUCCESS;
        if (mds_table_not_null) {
          need_gc = true;
          STORAGE_LOG(INFO, "create tablet abort, need gc", KPC(tablet));
        } else {
          STORAGE_LOG(INFO, "tablet_status is not commit", KR(ret), KPC(tablet));
        }
      } else {
        STORAGE_LOG(WARN, "failed to get CreateDeleteMdsUserData", KR(ret), KPC(tablet));
      }
    } else if (!is_finish) {
      need_retry = true;
    }
  }
  return ret;
}

int ObTabletGCHandler::get_unpersist_tablet_ids(common::ObIArray<ObTabletHandle> &deleted_tablets,
                                                common::ObTabletIDArray &unpersist_tablet_ids,
                                                const bool only_persist,
                                                bool &need_retry,
                                                const SCN &decided_scn)
{
  int ret = OB_SUCCESS;
  ObLSTabletIterator tablet_iter(ObMDSGetTabletMode::READ_WITHOUT_CHECK);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "tablet gc handle is not inited", KR(ret));
  } else if (OB_FAIL(ls_->get_tablet_svr()->build_tablet_iter(tablet_iter))) {
    STORAGE_LOG(WARN, "failed to build ls tablet iter", KR(ret), KPC(this));
  } else {
    ObTabletHandle tablet_handle;
    ObTablet *tablet = NULL;
    bool need_gc = false;
    bool need_persist = false;
    while (OB_SUCC(ret)) {
      if (check_stop()) {
        ret = OB_EAGAIN;
        STORAGE_LOG(INFO, "tablet gc handler stop", KR(ret), KPC(this), K(tablet_handle), KPC(ls_), K(ls_->get_ls_meta()));
      } else if (OB_FAIL(tablet_iter.get_next_tablet(tablet_handle))) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          break;
        } else {
          STORAGE_LOG(WARN, "failed to get tablet", KR(ret), KPC(this), K(tablet_handle));
        }
      } else if (OB_UNLIKELY(!tablet_handle.is_valid())) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "invalid tablet handle", KR(ret), KPC(this), K(tablet_handle));
      } else if (OB_ISNULL(tablet = tablet_handle.get_obj())) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "tablet is NULL", KR(ret));
      } else if (tablet->is_ls_inner_tablet()) {
        // skip ls inner tablet
        continue;
      }

      if (OB_FAIL(ret)) {
      } else if (only_persist) {
      } else if (OB_FAIL(check_tablet_need_gc_(tablet_handle, need_gc, need_retry, decided_scn))) {
        STORAGE_LOG(WARN, "failed to check_tablet_need_gc_", KR(ret), KPC(tablet));
      } else if (!need_gc) {
      } else if (OB_FAIL(deleted_tablets.push_back(tablet_handle))) {
        STORAGE_LOG(WARN, "failed to push_back", KR(ret));
      } else {
        STORAGE_LOG(INFO, "[tabletgc] get tablet for gc", KPC(tablet), K(decided_scn));
      }

      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(check_tablet_need_persist_(tablet_handle, need_persist, need_retry, decided_scn))) {
        STORAGE_LOG(WARN, "failed to check_tablet_need_persist_", KR(ret), KPC(tablet));
      } else if (!need_persist) {
      } else if (OB_FAIL(unpersist_tablet_ids.push_back(tablet->get_tablet_meta().tablet_id_))) {
        STORAGE_LOG(WARN, "failed to push_back", KR(ret));
      } else {
        STORAGE_LOG(INFO, "[tabletgc] get tablet for persist", KPC(tablet), K(decided_scn));
      }
    }
  }
  STORAGE_LOG(INFO, "[tabletgc] get unpersist_tablet_ids", KR(ret), K(deleted_tablets.count()), K(unpersist_tablet_ids.count()), K(decided_scn));
  return ret;
}

int ObTabletGCHandler::flush_unpersist_tablet_ids(const common::ObTabletIDArray &unpersist_tablet_ids,
                                                  const SCN &decided_scn)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "tablet gc handle is not inited", KR(ret));
  } else if (OB_FAIL(freeze_unpersist_tablet_ids(unpersist_tablet_ids, decided_scn))) {
    STORAGE_LOG(WARN, "fail to freeze unpersist tablet", KR(ret), KPC(this->ls_), K(unpersist_tablet_ids));
  } else if (OB_FAIL(wait_unpersist_tablet_ids_flushed(unpersist_tablet_ids, decided_scn))) {
    STORAGE_LOG(WARN, "fail to wait unpersist tablet ids flushed", KR(ret), KPC(this->ls_), K(unpersist_tablet_ids));
  }
  return ret;
}

int ObTabletGCHandler::freeze_unpersist_tablet_ids(const common::ObTabletIDArray &unpersist_tablet_ids,
                                                   const SCN &decided_scn)
{
  int ret = OB_SUCCESS;
  const int64_t start_ts = ObTimeUtility::fast_current_time();
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "tablet gc handle is not inited", KR(ret));
  } else {
    DEBUG_SYNC(BEFORE_TABLET_MDS_FLUSH);
    // freeze all tablets
    for (int64_t i = 0; i < unpersist_tablet_ids.count() && OB_SUCC(ret); i++) {
      if (!unpersist_tablet_ids.at(i).is_valid()) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "invalid tablet_id", KR(ret), KPC(this->ls_), K(unpersist_tablet_ids));
      } else {
        ObTabletHandle tablet_handle;
        ObTablet *tablet = nullptr;
        if (OB_FAIL(ls_->get_tablet_svr()->get_tablet(unpersist_tablet_ids.at(i), tablet_handle, 0, ObMDSGetTabletMode::READ_WITHOUT_CHECK))) {
          STORAGE_LOG(WARN, "fail to get tablet", KR(ret), K(i), KPC(this->ls_), K(unpersist_tablet_ids));
        } else if (OB_ISNULL(tablet = tablet_handle.get_obj())) {
          ret = OB_ERR_UNEXPECTED;
          STORAGE_LOG(WARN, "tablet is NULL", KR(ret), K(i), KPC(this->ls_), K(unpersist_tablet_ids));
        } else if (OB_FAIL(tablet->mds_table_flush(decided_scn))) {
          STORAGE_LOG(WARN, "failed to persist mds table", KR(ret), KPC(tablet), K(this->ls_), K(unpersist_tablet_ids.at(i)));
        }
      }
    }
  }
  const int64_t end_ts = ObTimeUtility::fast_current_time();
  const int64_t cost = end_ts - start_ts;
  STORAGE_LOG(INFO, "[tabletgc] freeze unpersist_tablet_ids", KR(ret), K(unpersist_tablet_ids), K(unpersist_tablet_ids.count()), K(cost));
  return ret;
}

int ObTabletGCHandler::wait_unpersist_tablet_ids_flushed(const common::ObTabletIDArray &unpersist_tablet_ids,
                                                         const SCN &decided_scn)
{
  int ret = OB_SUCCESS;
  const int64_t start_ts = ObTimeUtility::fast_current_time();
  int64_t retry_times = FLUSH_CHECK_MAX_TIMES;
  int64_t i = 0;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "tablet gc handle is not inited", KR(ret));
  } else if (check_stop()) {
    ret = OB_EAGAIN;
    STORAGE_LOG(INFO, "tablet gc handler stop", KR(ret), KPC(this), KPC(ls_), K(ls_->get_ls_meta()));
  }
  // wait all tablet flushed
  while (unpersist_tablet_ids.count() > i && retry_times > 0 && OB_SUCC(ret)) {
    ob_usleep(FLUSH_CHECK_INTERVAL);
    while (unpersist_tablet_ids.count() > i && OB_SUCC(ret)) {
      ObTabletHandle handle;
      ObTablet *tablet = nullptr;
      SCN rec_scn = SCN::min_scn();
      if (check_stop()) {
        ret = OB_EAGAIN;
        STORAGE_LOG(INFO, "tablet gc handler stop", KR(ret), KPC(this), KPC(ls_), K(ls_->get_ls_meta()));
      } else if (OB_FAIL(ls_->get_tablet_svr()->get_tablet(unpersist_tablet_ids.at(i), handle, 0, ObMDSGetTabletMode::READ_WITHOUT_CHECK))) {
        STORAGE_LOG(WARN, "fail to get tablet", KR(ret), K(i), KPC(this->ls_), K(unpersist_tablet_ids));
      } else if (OB_ISNULL(tablet = handle.get_obj())) {
        ret = OB_ERR_UNEXPECTED;
        STORAGE_LOG(WARN, "tablet is NULL", KR(ret), K(i), KPC(this->ls_), K(unpersist_tablet_ids));
      } else if (OB_FAIL(tablet->get_mds_table_rec_log_scn(rec_scn))) {
        STORAGE_LOG(WARN, "fail to get rec log scn", KR(ret), K(handle));
      }

      if (OB_FAIL(ret)) {
      } else if (rec_scn > decided_scn) {
        STORAGE_LOG(INFO, "[tabletgc] finish tablet flush", K(rec_scn), K(decided_scn),
                    K(retry_times), K(i), K(ls_->get_ls_id()), KPC(tablet));
        i++;
      } else {
        STORAGE_LOG(INFO, "[tabletgc] wait tablet flush", K(rec_scn), K(decided_scn),
                    K(retry_times), K(i), K(ls_->get_ls_id()), KPC(tablet));
        break;
      }
    }
    retry_times--;
  }

  if (OB_SUCC(ret) && i != unpersist_tablet_ids.count()) {
    ret = OB_TIMEOUT;
    STORAGE_LOG(WARN, "flush tablet timeout", KR(ret), K(retry_times), K(i), KPC(ls_), K(unpersist_tablet_ids));
  }
  const int64_t end_ts = ObTimeUtility::fast_current_time();
  const int64_t cost = end_ts - start_ts;
  if (OB_SUCC(ret)) {
    STORAGE_LOG(INFO, "[tabletgc] flush tablet finish", KR(ret), KPC(ls_), K(unpersist_tablet_ids), K(unpersist_tablet_ids.count()), K(cost));
  }
  return ret;
}

int ObTabletGCHandler::gc_tablets(const common::ObIArray<ObTabletHandle> &deleted_tablets)
{
  int ret = OB_SUCCESS;

  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "tablet gc handler is not inited", KR(ret));
  } else {
    DEBUG_SYNC(BEFORE_TABLET_GC);

    bool need_retry = false;
    for (int64_t i = 0; OB_SUCC(ret) && i < deleted_tablets.count(); ++i) {
      const ObTabletHandle &tablet_handle = deleted_tablets.at(i);
      if (check_stop()) {
        ret = OB_EAGAIN;
        STORAGE_LOG(INFO, "tablet gc handler stop", KR(ret), KPC(this), KPC(ls_), K(ls_->get_ls_meta()));
      } else if (OB_FAIL(ls_->get_tablet_svr()->remove_tablet(tablet_handle))) {
        if (OB_EAGAIN == ret) {
          need_retry = true;
          ret = OB_SUCCESS;
        } else {
          STORAGE_LOG(WARN, "failed to remove tablet", K(ret), K(tablet_handle));
        }
      } else {
        STORAGE_LOG(INFO, "gc tablet finish", K(ret),
                          "ls_id", tablet_handle.get_obj()->get_tablet_meta().ls_id_,
                          "tablet_id", tablet_handle.get_obj()->get_tablet_meta().tablet_id_);
      }
    }

    if (OB_FAIL(ret)) {
    } else if (need_retry) {
      ret = OB_EAGAIN;
    }
  }
  return ret;
}


int ObTabletGCHandler::get_max_tablet_transfer_scn(
    const common::ObIArray<ObTabletHandle> &deleted_tablets,
    share::SCN &transfer_scn)
{
  int ret = OB_SUCCESS;
  const bool need_initial_state = false;
  ObHALSTabletIDIterator iter(ls_->get_ls_id(), need_initial_state);
  share::SCN max_transfer_scn = share::SCN::min_scn();
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "tablet gc handler is not inited", KR(ret));
  } else {
    ObTenantMetaMemMgr *t3m = MTL(ObTenantMetaMemMgr*);
    common::ObTabletID tablet_id;
    ObTabletMapKey key;
    key.ls_id_ = ls_->get_ls_id();
    ObTabletCreateDeleteMdsUserData mds_data;
    ObTabletHandle tablet_handle;
    const WashTabletPriority priority = WashTabletPriority::WTP_LOW;
    for (int i = 0; OB_SUCC(ret) && i < deleted_tablets.count(); i++) {
      mds_data.reset();
      const ObTabletHandle &tablet_handle = deleted_tablets.at(i);
      if (OB_FAIL(tablet_handle.get_obj()->ObITabletMdsInterface::get_tablet_status(
          share::SCN::max_scn(), mds_data, ObTabletCommon::DEFAULT_GET_TABLET_DURATION_US))) {
        if (OB_EMPTY_RESULT == ret) {
          ret = OB_SUCCESS;
          LOG_INFO("create tablet abort, need gc", K(tablet_id));
          continue;
        } else {
          LOG_WARN("failed to get mds table", KR(ret), K(tablet_handle));
        }
      } else if (share::SCN::invalid_scn() == mds_data.transfer_scn_) {
        // do nothing
      } else {
        transfer_scn = mds_data.transfer_scn_;
        max_transfer_scn = MAX(transfer_scn, max_transfer_scn);
      }
    }
    if (OB_SUCC(ret)) {
      transfer_scn = max_transfer_scn;
    }
  }
  return ret;
}

int ObTabletGCHandler::set_ls_transfer_scn(const common::ObIArray<ObTabletHandle> &deleted_tablets)
{
  int ret = OB_SUCCESS;
  share::SCN tablet_max_transfer_scn;
  if (0 == deleted_tablets.count()) {
    // do nothing
  } else if (OB_FAIL(get_max_tablet_transfer_scn(deleted_tablets, tablet_max_transfer_scn))) {
    STORAGE_LOG(WARN, "failed to get max tablet transfer scn", K(ret));
  } else if (OB_FAIL(ls_->inc_update_transfer_scn(tablet_max_transfer_scn))) {
    LOG_WARN("failed to set transfer scn", K(ret));
  }
  return ret;
}

int ObTabletGCHandler::offline()
{
  int ret = OB_SUCCESS;
  set_stop();
  if (!is_finish()) {
    ret = OB_EAGAIN;
    STORAGE_LOG(INFO, "tablet gc handler not finish, retry", KR(ret), KPC(this), KPC(ls_), K(ls_->get_ls_meta()));
  } else if (OB_FAIL(gc_lock_.lock(GC_LOCK_TIMEOUT))) {
    // make sure 'gc_lock_' is not using.
    ret = OB_TABLET_GC_LOCK_CONFLICT;
    LOG_WARN("tablet gc handler not finish, retry", K(ret));
  } else {
    gc_lock_.unlock();
    STORAGE_LOG(INFO, "tablet gc handler offline", KPC(this), KPC(ls_), K(ls_->get_ls_meta()));
  }
  return ret;
}

void ObTabletGCHandler::online()
{
  set_tablet_persist_trigger();
  set_tablet_gc_trigger();
  set_start();
  STORAGE_LOG(INFO, "tablet gc handler online", KPC(this), KPC(ls_), K(ls_->get_ls_meta()));
}

} // checkpoint
} // storage
} // oceanbase
