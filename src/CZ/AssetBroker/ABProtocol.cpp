#include <CZ/AssetBroker/ABProtocol.h>
#include <sys/socket.h>
#include <cstring>
#include <cstdlib>
#include <cerrno>

using namespace CZ;

std::filesystem::path ABProtocol::SocketPath() noexcept
{
    if (const char *path { getenv("CZ_ASSET_BROKER_SOCKET") }; path && *path)
        return path;

    if (const char *dir { getenv("XDG_RUNTIME_DIR") }; dir && *dir)
        return std::filesystem::path(dir) / "cz-asset-broker";

    return {};
}

bool ABProtocol::Send(int socket, const void *data, size_t size, const int *fds, int fdCount) noexcept
{
    if (fdCount < 0 || fdCount > MaxFds || size > MaxMessageSize)
        return false;

    iovec iov {};
    iov.iov_base = const_cast<void*>(data);
    iov.iov_len = size;

    msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * MaxFds)] {};

    if (fdCount > 0)
    {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fdCount);
        cmsghdr *cmsg { CMSG_FIRSTHDR(&msg) };
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fdCount);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fdCount);
    }

    ssize_t ret;

    do { ret = sendmsg(socket, &msg, MSG_NOSIGNAL); }
    while (ret < 0 && errno == EINTR);

    return ret == static_cast<ssize_t>(size);
}

ssize_t ABProtocol::Recv(int socket, void *data, size_t capacity, std::vector<CZSpFd> &fds) noexcept
{
    fds.clear();

    iovec iov {};
    iov.iov_base = data;
    iov.iov_len = capacity;

    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * MaxFds)] {};

    msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t ret;

    do { ret = recvmsg(socket, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT); }
    while (ret < 0 && errno == EINTR);

    if (ret < 0)
        return -1;

    for (cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
    {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            continue;

        const size_t count { (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int) };
        const int *received { reinterpret_cast<const int*>(CMSG_DATA(cmsg)) };

        for (size_t i = 0; i < count; i++)
        {
            int fd;
            memcpy(&fd, received + i, sizeof(int));
            fds.emplace_back(fd);
        }
    }

    // Truncated messages (or fds) are protocol errors: treat as a disconnection
    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))
    {
        fds.clear();
        errno = EPROTO;
        return -1;
    }

    return ret;
}

ABProtocol::Op ABProtocol::PeekOp(const void *data, size_t size) noexcept
{
    if (size < sizeof(Op))
        return static_cast<Op>(0);

    Op op;
    memcpy(&op, data, sizeof(Op));
    return op;
}
