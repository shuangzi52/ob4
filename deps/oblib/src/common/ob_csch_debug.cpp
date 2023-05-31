//
// Created by 操盛春 on 2022/10/26.
//

#include "ob_csch_debug.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <sys/syscall.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

namespace oceanbase {
namespace common {

void ObCschDebug::breakpoint(const char *data, const char *func, const char *file, int line) {
  std::ifstream in;
  in.open("/opt/data/workspace_c/ob4/debug_config.cnf", std::ios::in);
  if (!in.is_open()) {
    std::cout << "[CSCH] 打开 debug_config.cnf 文件失败";
    return ;
  }

  const char *strRow;
  std::string row;
  std::vector<std::string> rows;
  while (std::getline(in, row)) {
    rows.push_back(row);
  }

  in.close();
  int i = 0;
  unsigned long len = rows.size();
  for (; i < len; ++i) {
    row = rows[i];

    // csch 空行或分号注释表示忽略当前读取行中的断点关键字，如果后面命中了其它关键字，也能命中断点
    if ((row.size() == 0) || (row.substr(0, 1) == ";")) {
      std::cout << "continue " << row << std::endl;
      continue;
    }

    // csch 忽略注释
    if (row.substr(0, 1) == "#") {
      continue;
    }

    strRow = row.c_str();
    if (strstr(data, strRow)) {
      std::cout << "[CSCH] data = [" << data << "], keyword = [" << row << "], func = [" << func << "], file = [" << file << "], line = [" << line << "]" << std::endl;
      break;
    }
    /* if (strstr(func, row.c_str())) {
      std::cout << "[CSCH] hit func, data = [" << data << "], keyword = [" << row << "], func = [" << func << "], file = [" << file << "], line = [" << line << "]" << std::endl;
      break;
    } */
  }
}

void ObCschDebug::debug(std::string data)
{
  // 精确到秒的时间
  const time_t t = time(NULL);
  struct tm *tm = localtime(&t);

  // 微秒
  struct timeval tv;
  (void)gettimeofday(&tv, NULL);

  char buf[32];
  memset(buf, 0, 32);
  sprintf(buf, "[%04d-%02d-%02d %02d:%02d:%02d.%06ld]", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tv.tv_usec);

  // 线程 ID
  unsigned int tid = (unsigned int)syscall(SYS_gettid);

  // 线程名
  char thdName[64];
  memset(thdName, 0, 64);
  prctl(PR_GET_NAME, thdName);

  std::cout << "[CSCH] " << buf << " tid = " << tid << ", thread name = " << thdName << ", data = " << data << std::endl;
}

}
}