#include "status_probe.h"
#include <string.h>

bool status_probe_response(const char *data, size_t length)
{
    if (!data || length < 16 ||
        (memcmp(data, "HTTP/1.0 204", 12) && memcmp(data, "HTTP/1.1 204", 12)) ||
        (data[12] != ' ' && data[12] != '\r')) return false;
    for (size_t i = 12; i + 3 < length; ++i) {
        if (!memcmp(data + i, "\r\n\r\n", 4)) return true;
        if (!data[i]) return false;
    }
    return false;
}
