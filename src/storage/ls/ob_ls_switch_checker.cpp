#include "ob_ls_switch_checker.h"
#include "lib/ob_errno.h"
#include "ob_ls.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace storage
{

int ObLSSwitchChecker::check_online(ObLS *ls)
{
  int ret = OB_SUCCESS;
  ls_ = ls;
  if (OB_ISNULL(ls)) {
    ret = OB_BAD_NULL_ERROR;
  } else {
    record_switch_epoch_ = ATOMIC_LOAD(&(ls_->switch_epoch_));
    if (!(record_switch_epoch_ & 1)) {
      ret = OB_LS_OFFLINE;
    }
  }
  return ret;
}

int ObLSSwitchChecker::check_ls_switch_state(ObLS *ls, bool &online_state)
{
  int ret = OB_SUCCESS;
  ls_ = ls;
  if (OB_ISNULL(ls)) {
    ret = OB_BAD_NULL_ERROR;
  } else {
    record_switch_epoch_ = ATOMIC_LOAD(&(ls_->switch_epoch_));
    if (!(record_switch_epoch_ & 1)) {
      online_state = false;
    } else {
      online_state = true;
    }
  }
  return ret;
}

int ObLSSwitchChecker::double_check_epoch() const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ls_)) {
    ret = OB_NOT_INIT;
  } else if (record_switch_epoch_ != ATOMIC_LOAD(&(ls_->switch_epoch_))) {
    ret = OB_VERSION_NOT_MATCH;
  }
  return ret;
}

}
}