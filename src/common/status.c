#include "ziputils.h"

const char* zu_status_str(int code) {
    switch (code) {
        case ZU_STATUS_OK:
            return "ok";
        case ZU_STATUS_IO:
            return "i/o error";
        case ZU_STATUS_OOM:
            return "out of memory";
        case ZU_STATUS_USAGE:
            return "invalid usage";
        case ZU_STATUS_NOT_IMPLEMENTED:
            return "not implemented";
        case ZU_STATUS_PASSWORD_REQUIRED:
            return "password required";
        case ZU_STATUS_BAD_PASSWORD:
            return "bad password";
        default:
            return "unknown";
    }
}
