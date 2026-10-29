#include "platform/paths.hpp"
#include <cstdlib>
#include <limits.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace nativetv {
std::string resourceDirectory(const char *executable) {
    char path[PATH_MAX];
#ifdef __APPLE__
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size))
        path[0] = 0;
#else
    ssize_t size = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (size > 0)
        path[size] = 0;
    else
        path[0] = 0;
#endif
    char resolved[PATH_MAX];
    const char *source = path[0] ? path : executable;
    std::string value = realpath(source, resolved) ? resolved : source;
    size_t slash = value.find_last_of('/');
    return slash == std::string::npos ? "." : value.substr(0, slash);
}
} // namespace nativetv
