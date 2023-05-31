//
// Created by 操盛春 on 2022/10/26.
//

#ifndef OCEANBASE_CE_OB_CSCH_DEBUG_H
#define OCEANBASE_CE_OB_CSCH_DEBUG_H

#define CSCH_BREAKPOINT_CSTR(prefix, debugData) \
{ \
  std::string cschDebugStr(prefix); \
  cschDebugStr.append(debugData); \
  std::string cschDebugFunc = __FUNCTION__; \
  std::string cschDebugFile = __FILE__;     \
  (new ObCschDebug())->breakpoint(cschDebugStr.c_str(), cschDebugFunc.c_str(), cschDebugFile.c_str(), __LINE__); \
}

#define CSCH_BREAKPOINT_NUM(prefix, number) \
{ \
  std::string cschDebugFunc = __FUNCTION__; \
  std::string cschDebugFile = __FILE__;     \
  std::string cschDebugNumber(prefix); \
  cschDebugNumber.append(std::to_string(number)); \
  (new ObCschDebug())->breakpoint(cschDebugNumber.c_str(), cschDebugFunc.c_str(), cschDebugFile.c_str(), __LINE__); \
}

#define CSCH_DEBUG(debugData) (new ObCschDebug())->debug(debugData);

namespace oceanbase {
namespace common {
class ObCschDebug {
public:
  void breakpoint(const char *data, const char *func, const char *file, int line);
  void debug(std::string data);
};
}
}

#endif  // OCEANBASE_CE_OB_CSCH_DEBUG_H
