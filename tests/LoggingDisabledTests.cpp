#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_OFF
#include "utility/Logging.hpp"

int disabled_log_argument_evaluations() {
    int evaluations{};
    for (int i = 0; i < 10; ++i) {
        SPDLOG_INFO_ONCE("{}", ++evaluations);
        SPDLOG_WARN_ONCE("{}", ++evaluations);
        SPDLOG_ERROR_ONCE("{}", ++evaluations);
    }
    return evaluations;
}
