// Built only into a TMB_PGO=GENERATE runner (scripts/pgo.sh). The instrumented runtime writes its profile when the
// process exits normally, which a run stopped from outside never does: TMB_PGO_SECONDS=<s> writes it after that many
// seconds of play and exits.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" void __gcov_dump(void);

namespace
{
    struct PgoTraining
    {
        PgoTraining()
        {
            const char *value = std::getenv("TMB_PGO_SECONDS");
            const double seconds = value ? std::atof(value) : 0.0;
            if (seconds <= 0.0)
                return;
            std::thread([seconds]()
                        {
                            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
                            __gcov_dump();
                            std::fprintf(stderr, "[tmb:pgo] profile written after %.0f s\n", seconds);
                            std::fflush(stderr);
                            std::_Exit(0); })
                .detach();
        }
    };

    const PgoTraining g_pgoTraining;
}
