#include <string>

// The TSDuck-free build does not include dantto4k's remuxerHandler.cpp,
// which owns this optional subtitle diagnostic hook. Keep subtitle conversion
// available while intentionally discarding its debug messages.
void subtitleDebugLog(const std::string&)
{
}
