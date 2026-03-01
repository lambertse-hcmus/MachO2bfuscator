#include "logger.h"

#include <atomic>

namespace logger {
namespace {

struct Statics {
  LoggingFunctionType out = [](const std::string& msg) {
    fprintf(stdout, "%s\n", msg.c_str());
  };
  LoggingFunctionType err = [](const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());  // No-op by default
  };
  std::atomic<LogLevels> allowedLevels = LOG_LEVEL_SILENCE;
};

static Statics& statics() {
  static Statics s;
  return s;
}

}  // namespace

void init(LogLevels allowedLevels, LoggingFunctionType outLogFunc,
          LoggingFunctionType errLogFunc) {
  if (outLogFunc) {
    statics().out = std::move(outLogFunc);
  }
  if (errLogFunc) {
    statics().err = std::move(errLogFunc);
  }
  changeLogLevels(allowedLevels);
}

void stopLogging() { statics().allowedLevels = LOG_LEVEL_SILENCE; }

void changeLogLevels(LogLevels allowedLevels) {
  statics().allowedLevels = allowedLevels;
}

bool allowed(LogLevel level) {
  return statics().allowedLevels.load(std::memory_order_relaxed) & level;
}

void enable(LogLevel level) { statics().allowedLevels &= level; }

void disable(LogLevel level) { statics().allowedLevels &= ~level; }

void logImpl(LogLevel filteredLevel, const std::string& msg) {
  switch (filteredLevel) {
    case LOG_LEVEL_INFO:
    case LOG_LEVEL_DEBUG:
    case LOG_LEVEL_VERBOSE:
      statics().out(msg);
      break;
    default:
      statics().err(msg);
      break;
  }
}

}  // namespace logger
