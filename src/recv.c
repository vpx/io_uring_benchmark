#include "default.h"
#include <liburing.h>
#include <memory.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct io_stats {
    unsigned packets;
    unsigned goodput;
};

struct args {
    char* ip;
    char* port;
    int sq_entries;
    int cq_entries;
    int sq_idle;
    bool sq_poll;
    size_t buffs_entries;
    size_t buff_size;
};

struct buff_ring_group {
    struct io_uring_buf_ring* buff_ring;
    char** buffs;
    size_t buff_size;
    size_t entries;
    int id;
};

double time_diff_milli(struct timespec* end, struct timespec* start) {
    return (double) (end->tv_sec - start->tv_sec) * 1e3 +
           (double) (end->tv_nsec - start->tv_nsec) / 1e6;
}

double time_diff_milli_now(struct timespec* start) {
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) == 0) {
        printf("timespec_get() failed\n");
        exit(EXIT_FAILURE);
    }
    return time_diff_milli(&now, start);
}

int resolve_hostname(struct addrinfo** info, char* ip, char* port, int sock_type) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = sock_type;
    return getaddrinfo(ip, port, &hints, info);
}

int init_socket(char* ip, char* port) {
    struct addrinfo* info = nullptr;
    int resolve_res = resolve_hostname(&info, ip, port, SOCK_DGRAM);
    if (resolve_res != 0) {
        printf("resolve_hostname() failed %s\n", gai_strerror(resolve_res));
        exit(EXIT_FAILURE);
    }

    const int socket_fd = socket(
        info->ai_family,
        info->ai_socktype,
        info->ai_protocol
    );

    if (socket_fd < 0) {
        perror("socket() failed");
        exit(EXIT_FAILURE);
    }

    if (bind(socket_fd, info->ai_addr, info->ai_addrlen) != 0) {
        perror("bind() failed");
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(info);

    return socket_fd;
}

void submit_recv_fixed_multishot(
    struct io_uring* ring,
    int fixed_fd,
    int group_id
) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr) {
        printf("io_uring_get_sqe() failed\n");
        exit(EXIT_FAILURE);
    }

    io_uring_prep_recv_multishot(
        sqe,
        fixed_fd,
        nullptr,
        0,
        0
    );

    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->flags |= IOSQE_FIXED_FILE;
    sqe->buf_group = group_id;

    int submit_status = io_uring_submit(ring);
    if (submit_status < 0) {
        printf("io_uring_sumbit() failed: %s\n", strerror(-submit_status));
        exit(EXIT_FAILURE);
    }
}

void print_stats(
    struct io_stats* stats,
    struct timespec* timer,
    unsigned delay_in_milli
) {
    double elapsed_milli = time_diff_milli_now(timer);
    if (elapsed_milli > delay_in_milli) {
        printf(
            "Read %u packets totaling %u MB in %f seconds.\n",
            stats->packets, stats->goodput / 1024 / 1024, elapsed_milli / 1e3
        );

        stats->packets = 0;
        stats->goodput = 0;
        if (timespec_get(timer, TIME_UTC) == 0) {
            printf("timespec_get() failed\n");
            exit(EXIT_FAILURE);
        }
    }
}

void print_usage(char* program_name) {
    printf(
        "Usage: %s <ip> <port> [--<option>] [<value>]\n\nOptions:\n"
        "--sq_entries=<entries> Submission queue entries, by default is %d\n"
        "--cq_entries=<entries> Completion queue entries, by default is %d\n"
        "--sq_idle=<milli> Idle before blocking, by default %d\n"
        "--sq_poll Enable SQPOLL mode\n",
        program_name,
        default_sq_entries,
        default_cq_entries,
        default_sq_idle
    );
}

struct args parse_args(int argc, char* argv[]) {
    if (argc < 3) {
        print_usage(argv[0]);
        exit(EXIT_FAILURE);
    }

    struct args args = {
        .ip = argv[1],
        .port = argv[2],
        .sq_entries = default_sq_entries,
        .cq_entries = default_cq_entries,
        .sq_idle = default_sq_idle,
        .sq_poll = default_sq_poll,
        .buffs_entries = default_buffs_entries,
        .buff_size = default_buff_size
    };

    for (int i = 3; i < argc; ++i) {
        const char sq_poll_opt[] = "--sq_poll";
        if (strcmp(sq_poll_opt, argv[i]) == 0) {
            args.sq_poll = true;
            continue;
        }

        const char sq_entries_opt[] = "--sq_entries=";
        if (strncmp(sq_entries_opt, argv[i], sizeof(sq_entries_opt)) == 0) {
            args.sq_entries = atoi(argv[i] + sizeof(sq_entries_opt));
            if (args.sq_entries == 0) {
                printf("Invalid value for sq_entries\n");
                exit(EXIT_FAILURE);
            }
            continue;
        }

        const char cq_entries_opt[] = "--cq_entries=";
        if (strncmp(cq_entries_opt, argv[i], sizeof(cq_entries_opt)) == 0) {
            args.cq_entries = atoi(argv[i] + sizeof(cq_entries_opt));
            if (args.cq_entries == 0) {
                printf("Invalid value for cq_entries\n");
                exit(EXIT_FAILURE);
            }
            continue;
        }

        const char buffs_entries_opt[] = "--buffs_entries=";
        if (strncmp(buffs_entries_opt, argv[i], sizeof(buffs_entries_opt)) == 0) {
            args.buffs_entries = atoi(argv[i] + sizeof(buffs_entries_opt));
            if (args.buffs_entries == 0) {
                printf("Invalid value for buffs_entries\n");
                exit(EXIT_FAILURE);
            }
            continue;
        }

        const char buff_size_opt[] = "--buff_size=";
        if (strncmp(buff_size_opt, argv[i], sizeof(buff_size_opt)) == 0) {
            args.buff_size = atoi(argv[i] + sizeof(buff_size_opt));
            if (args.buff_size == 0) {
                printf("Invalid value for buff_size\n");
                exit(EXIT_FAILURE);
            }
            continue;
        }

        const char sq_idle_opt[] = "--sq_idle=";
        if (strncmp(sq_idle_opt, argv[i], sizeof(sq_idle_opt)) == 0) {
            args.sq_idle = atoi(argv[i] + sizeof(sq_idle_opt));
            if (args.sq_idle == 0) {
                printf("Invalid value for sq_idle\n");
                exit(EXIT_FAILURE);
            }
            continue;
        }

        printf("Invalid argument: %s\n", argv[i]);
        exit(EXIT_FAILURE);
    }

    return args;
}

struct buff_ring_group* create_buff_ring_group(
    struct io_uring* ring,
    size_t entries,
    size_t buff_size
) {
    struct buff_ring_group* group 
        = (struct buff_ring_group*) malloc(sizeof(struct buff_ring_group));

    static int latest_group_id = 0;
    int buff_ring_err = 0;
    group->id = latest_group_id;
    group->buff_ring = io_uring_setup_buf_ring(
        ring,
        entries,
        latest_group_id++,
        0,
        &buff_ring_err
    );

    if (group->buff_ring == nullptr) {
        printf(
            "io_uring_setup_buf_ring() failed: %s\n",
            strerror(-buff_ring_err)
        );
        exit(EXIT_FAILURE);
    }

    group->buffs = (char**) malloc(entries * sizeof(char*));
    group->buff_size = buff_size;
    group->entries = entries;

    for (size_t i = 0; i < entries; ++i) {
        group->buffs[i] = (char*) malloc(buff_size);
        
        io_uring_buf_ring_add(
            group->buff_ring,
            group->buffs[i],
            buff_size,
            (int) i,
            io_uring_buf_ring_mask(entries),
            (int) i
        );
    }

    io_uring_buf_ring_advance(group->buff_ring, (int) entries);
    return group;
}

void destroy_buff_ring_group(struct buff_ring_group* group) {
    for (size_t i = 0; i < group->entries; ++i) {
        free(group->buffs[i]);
    }

    free(group->buffs);
    free(group);
}

void handle_blocking_cqe(
    struct io_uring* ring,
    int fixed_fd,
    struct buff_ring_group* group,
    struct io_stats* stats
) {
    struct io_uring_cqe* cqe = nullptr;

    int wait_res = io_uring_wait_cqe(ring, &cqe);
    if (wait_res < 0) {
        printf(
            "io_uring_wait_cqe() failed: %s\n",
            strerror(-wait_res)
        );
        exit(EXIT_FAILURE);
    }

    if (!(cqe->flags & IORING_CQE_F_MORE)) {
        submit_recv_fixed_multishot(ring, fixed_fd, group->id);
        return;
    }

    int read_bytes = cqe->res;
    if (read_bytes < 0) {
        printf("read() failed: %s\n", strerror(-read_bytes));
        exit(EXIT_FAILURE);
    }

    int buff_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;

    io_uring_buf_ring_add(
        group->buff_ring,
        group->buffs[buff_id],
        group->buff_size,
        buff_id,
        0,
        group->id
    );

    io_uring_cqe_seen(ring, cqe);
    io_uring_buf_ring_advance(group->buff_ring, 1);
    stats->goodput += read_bytes;
    ++stats->packets;
}

void handle_peek_cqes(
    struct io_uring* ring,
    int fixed_fd,
    struct buff_ring_group* group,
    struct io_uring_cqe** cqes,
    unsigned cqe_entries,
    struct io_stats* stats
) {
    unsigned processed_packets = 0;
    for (unsigned int i = 0; i < cqe_entries; ++i) {
        struct io_uring_cqe* cqe = cqes[i];

        int read_bytes = cqe->res;
        if (read_bytes < 0) {
            printf("read() failed: %s\n", strerror(-cqe->res));
            exit(EXIT_FAILURE);
        }

        if (!(cqe->flags & IORING_CQE_F_MORE)) {
            submit_recv_fixed_multishot(ring, fixed_fd, group->id);
            continue;
        }

        int buff_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;

        io_uring_buf_ring_add(
            group->buff_ring,
            group->buffs[buff_id],
            group->buff_size,
            buff_id,
            0,
            group->id
        );

        stats->goodput += read_bytes;
        ++processed_packets;
    }

    io_uring_buf_ring_cq_advance(ring, group->buff_ring, (int) processed_packets);
    stats->packets += processed_packets;
}

int main(int argc, char* argv[]) {
    struct args args = parse_args(argc, argv);

    int local_fd = init_socket(args.ip, args.port);

    struct io_uring_params ring_params = {
        .flags = args.sq_poll ? IORING_SETUP_SQPOLL : 0,
        .sq_thread_idle = (unsigned int) args.sq_idle,
        .cq_entries = args.cq_entries,
    };

    struct io_uring ring;
    int ring_res = io_uring_queue_init_params(args.sq_entries, &ring, &ring_params);
    if (ring_res < 0) {
        printf("io_uring_init() failed: %s\n", strerror(-ring_res));
        exit(EXIT_FAILURE);
    }

    int fd_index = 0;
    int reg_res = io_uring_register(
        ring.ring_fd,
        IORING_REGISTER_FILES,
        &local_fd,
        1
    );

    if (reg_res < 0) {
        printf("io_uring_register() failed: %s\n", strerror(-reg_res));
        exit(EXIT_FAILURE);
    }

    struct buff_ring_group* group 
        = create_buff_ring_group(&ring, args.buffs_entries, args.buff_size);

    submit_recv_fixed_multishot(&ring, fd_index, group->id);

    struct io_stats stats = {};
    struct timespec timer;
    if (timespec_get(&timer, TIME_UTC) == 0) {
        printf("timespec_get() failed\n");
        exit(EXIT_FAILURE);
    }

    struct io_uring_cqe* cqes[args.cq_entries];
    struct timespec peek_timer;
    if (timespec_get(&peek_timer, TIME_UTC) == 0) {
        printf("timespec_get() failed\n");
        exit(EXIT_FAILURE);
    }

    while (true) {
        unsigned cqe_entries = io_uring_peek_batch_cqe(&ring, cqes, args.cq_entries);

        if (cqe_entries < 0) {
            printf(
                "io_uring_peek_cqe() failed: %s\n",
                strerror(-((int)cqe_entries))
            );
            exit(EXIT_FAILURE);
        }

        if (cqe_entries == 0) {
            if (time_diff_milli_now(&peek_timer) > args.sq_idle) {
                handle_blocking_cqe(&ring, fd_index, group, &stats);

                if (timespec_get(&peek_timer, TIME_UTC) == 0) {
                    printf("timespec_get() failed\n");
                    exit(EXIT_FAILURE);
                }
            }
            continue;
        }

        handle_peek_cqes(&ring, fd_index, group, cqes, cqe_entries, &stats);

        print_stats(&stats, &timer, 1000);
    }

    destroy_buff_ring_group(group);

    io_uring_queue_exit(&ring);

    if (close(local_fd) != 0) {
        perror("close() failed");
        exit(EXIT_FAILURE);
    }
}
