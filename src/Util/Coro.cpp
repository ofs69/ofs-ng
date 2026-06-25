#include "Coro.h"
#include "Log.h"
#include <exception>

namespace ofs::co {

// A flow throwing is a bug: business logic in our flows catches its own errors. Log with what
// context we have and swallow — the frame then proceeds to final_suspend. Rethrowing here would
// call std::terminate (the coroutine machinery is noexcept around us).
void logCurrentFlowException(const char *flowKind) noexcept {
    try {
        std::rethrow_exception(std::current_exception());
    } catch (const std::exception &e) {
        OFS_CORE_ERROR("Unhandled exception in {} flow: {}", flowKind, e.what());
    } catch (...) {
        OFS_CORE_ERROR("Unhandled non-std exception in {} flow", flowKind);
    }
}

} // namespace ofs::co
