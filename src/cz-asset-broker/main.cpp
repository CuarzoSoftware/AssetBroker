#include <CZ/AssetBroker/ABServer.h>
#include <CZ/AssetBroker/ABUtils.h>
#include <CZ/AssetBroker/ABLog.h>
#include <CZ/Ream/RCore.h>
#include <CZ/Core/CZCore.h>
#include <CZ/Core/CZEventSource.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <csignal>
#include <unistd.h>
#include <cstdio>
#include <cstring>

using namespace CZ;

/*
 * cz-asset-broker: shares icons and other assets between Cuarzo apps as DMA-bufs.
 *
 * Runs in the user session (it needs XDG_RUNTIME_DIR, or CZ_ASSET_BROKER_SOCKET). Apps using the
 * AssetBroker library in ABMode::Auto use it when it's running and load assets themselves when not.
 */
int main(int argc, char **argv)
{
    if (argc > 1)
    {
        fprintf(stderr,
            "Usage: %s\n\n"
            "The cz-asset-broker service. It takes no arguments and is normally started by systemd\n"
            "(cz-asset-broker.socket) when an app first connects.\n\n"
            "To load icons through it, see cz-asset-broker-example [auto|local|remote].\n", argv[0]);
        return (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) ? 0 : 1;
    }

    auto cz { CZCore::GetOrMake() };

    if (!cz)
        return 1;

    // Exit cleanly (removing the socket file) on SIGINT/SIGTERM
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, nullptr);

    bool running { true };
    const int sigFd { signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK) };
    auto signals { CZEventSource::Make(sigFd, EPOLLIN, CZOwn::Own, [&running](int fd, UInt32, CZEventSource *)
    {
        signalfd_siginfo info;
        while (read(fd, &info, sizeof(info)) == sizeof(info)) {}
        running = false;
    }) };

    auto ream { ABUtils::MakeHeadlessRCore() };

    if (!ream)
    {
        ABLog(CZFatal, CZLN, "Failed to initialize Ream");
        return 1;
    }

    auto server { ABServer::Make() };

    if (!server)
        return 1;

    while (running)
        if (cz->dispatch(-1) < 0)
            break;

    ABLog(CZInfo, CZLN, "Exiting");
    server.reset();
    return 0;
}
