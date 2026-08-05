#ifndef ZMS_UTIL_STRTOK_R_H
#define ZMS_UTIL_STRTOK_R_H

#include <string.h>

#ifdef _WIN32
#define zms_strtok_r(str, delim, save) strtok_s((str), (delim), (save))
#else
#define zms_strtok_r(str, delim, save) strtok_r((str), (delim), (save))
#endif

#endif /* ZMS_UTIL_STRTOK_R_H */
