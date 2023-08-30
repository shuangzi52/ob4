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

#ifndef _OCEANBASE_OB_LUA_HANDLER_
#define _OCEANBASE_OB_LUA_HANDLER_

#include <thread>

#include "lib/container/ob_vector.h"
#include "lib/utility/ob_macro_utils.h"

namespace oceanbase
{
namespace diagnose
{
class ObUnixDomainListener
{
  static constexpr int MAX_CONNECTION_QUEUE_LENGTH = 1;
  static constexpr int CODE_BUFFER_SIZE = 1 << 20; // 1M
  static constexpr const char *addr = "run/lua.sock";
public:
  explicit ObUnixDomainListener()
    : is_inited_(false),
      stop_(false),
      running_(false),
      listen_fd_(-1) {}
  ~ObUnixDomainListener();
  int init();
  int run();
  void stop();
  void wait();
  void destroy();
private:
  DISALLOW_COPY_AND_ASSIGN(ObUnixDomainListener);
private:
  bool is_inited_;
  bool stop_;
  bool running_;
  int listen_fd_;
  std::thread worker_;
};

class ObLuaHandler
{
  using Function = std::function<int(void)>;
public:
  static constexpr int64_t LUA_MEMORY_LIMIT = (1UL << 25); // 32M
  ObLuaHandler() :
    alloc_count_(0),
    free_count_(0),
    alloc_size_(0),
    free_size_(0),
    destructors_(16, nullptr, "LuaHandler") {}
  void memory_update(const int size);
  int process(const char* lua_code);
  int64_t memory_usage() { return alloc_size_ - free_size_; }
  int register_destructor(Function func) { return destructors_.push_back(func); }
  int unregister_last_destructor() { return destructors_.remove(destructors_.size() - 1); }
  static ObLuaHandler& get_instance()
  {
    static ObLuaHandler instance;
    return instance;
  }
private:
  int64_t alloc_count_;
  int64_t free_count_;
  int64_t alloc_size_;
  int64_t free_size_;
  common::ObVector<Function> destructors_;
  static void *realloc_functor(void *userdata, void *ptr, size_t osize, size_t nsize);
  DISALLOW_COPY_AND_ASSIGN(ObLuaHandler);
};

}
}

#endif // _OCEANBASE_OB_LUA_HANDLER_