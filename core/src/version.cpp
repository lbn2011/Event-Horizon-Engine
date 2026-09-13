#include "ehe/core/version.h"

#if defined(EHE_FP64_METRIC)
#define EHE_FP64_METRIC_STR "fp64_metric=1"
#else
#define EHE_FP64_METRIC_STR "fp64_metric=0"
#endif

namespace ehe::core {

const char* version_string() { return EHE_VERSION_STRING; }

const char* build_flags() { return EHE_FP64_METRIC_STR; }

}  // namespace ehe::core
