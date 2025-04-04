#include <cutils/uevent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <linux/netlink.h>
ssize_t uevent_kernel_multicast_recv(int socket, void *buffer, size_t length)
{
    uid_t user = -1;
    return uevent_kernel_multicast_uid_recv(socket, buffer, length, &user);
}
ssize_t uevent_kernel_multicast_uid_recv(int socket, void *buffer,
                                         size_t length, uid_t *user)
{
    struct iovec iov = { buffer, length };
    struct sockaddr_nl addr;
    char control[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr hdr = {
        &addr,
        sizeof(addr),
        &iov,
        1,
        control,
        sizeof(control),
        0,
    };
    *user = -1;
    ssize_t n = recvmsg(socket, &hdr, 0);
    if (n <= 0) {
        return n;
    }
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&hdr);
    if (cmsg == NULL || cmsg->cmsg_type != SCM_CREDENTIALS) {
        goto out;
    }
    struct ucred *cred = (struct ucred *)CMSG_DATA(cmsg);
    *user = cred->uid;
    if (cred->uid != 0) {
        goto out;
    }
    if (addr.nl_groups == 0 || addr.nl_pid != 0) {
        goto out;
    }
    return n;
out:
    bzero(buffer, length);
    errno = EIO;
    return -1;
}
int uevent_open_socket(int buf_sz, bool passcred)
{
    struct sockaddr_nl addr;
    int on = passcred;
    int s;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    addr.nl_groups = 0xffffffff;
<<<<<<< HEAD
    s = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
||||||| a47780b99
    s = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
=======
    s = socket(PF_NETLINK, SOCK_DGRAM | O_CLOEXEC, NETLINK_KOBJECT_UEVENT);
>>>>>>> ccfb7619
    if(s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_RCVBUFFORCE, &buf_sz, sizeof(buf_sz));
    setsockopt(s, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));
    if(bind(s, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        close(s);
        return -1;
    }
    return s;
}
