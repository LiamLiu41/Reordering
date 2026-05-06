// 修改后的 rdma_rc_write.c

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>  // 添加 MLX5 直接访问头文件
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <limits.h>
#include <strings.h>

#define IB_PORT 1        // 可能需要修改为正确的端口号
#define MAX_SGE 1
#define MAX_RECV_WR 1
#define TIMEOUT_IN_MS 500
#define DEFAULT_PORT 5555
#define DEFAULT_NUM_WRITES 1  // 默认写入次数
#define DEFAULT_GID_IDX 3
#define DEFAULT_DATA_SIZE (1ULL << 30)
#define MIN_DATA_SIZE (1ULL << 30)
#define MAX_DATA_SIZE_BYTES (12500000000ULL)
#define DEFAULT_START_PSN 0
#define MAX_PSN ((1U << 24) - 1)

// 连接信息结构体
struct connection_info {
    uint16_t lid;
    uint32_t qp_num;
    uint32_t rkey;
    uint64_t addr;
    uint32_t psn;
    uint64_t buffer_size;
    // 添加GID支持
    uint8_t gid[16];
    int gid_idx;
};

// RDMA资源结构体
struct rdma_resources {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    char *buffer;
    struct connection_info local_info;
    struct connection_info remote_info;
    int port_num;
    size_t buffer_size;
    uint32_t start_psn;
    int num_writes;
    int is_sender;
    uint32_t max_qp_wr;
    uint32_t max_msg_sz;
};

// 函数声明
int init_rdma_resources(struct rdma_resources *res, const char *dev_name, int ib_port, int gid_idx,
                        size_t buffer_size, uint32_t start_psn, int num_writes, int is_sender);
int connect_qp(struct rdma_resources *res);
int exchange_connection_info_sender(struct rdma_resources *res, const char *server_addr, int port,
                                    const char *src_ip);
int exchange_connection_info_receiver(struct rdma_resources *res, int port);
int perform_rdma_write(struct rdma_resources *res, int num_writes);
int wait_for_rdma_completion(struct rdma_resources *res);
void cleanup_rdma_resources(struct rdma_resources *res);
void print_usage(const char *program_name);
int get_port_info(struct ibv_context *context, int port, struct ibv_port_attr *port_attr);
int query_gid(struct ibv_context *context, int port, int index, union ibv_gid *gid);
int parse_data_size(const char *arg, size_t *size_bytes);

int parse_data_size(const char *arg, size_t *size_bytes) {
    char *end = NULL;
    double value = strtod(arg, &end);
    unsigned long long result = 0;

    if (!arg || !*arg || end == arg || value <= 0) {
        return 1;
    }

    if (*end == '\0' || strcmp(end, "B") == 0) {
        result = (unsigned long long)value;
    } else if (strcmp(end, "Gb") == 0) {
        result = (unsigned long long)((value * 1000.0 * 1000.0 * 1000.0) / 8.0);
    } else if (strcmp(end, "Mb") == 0) {
        result = (unsigned long long)((value * 1000.0 * 1000.0) / 8.0);
    } else if (strcmp(end, "Kb") == 0) {
        result = (unsigned long long)((value * 1000.0) / 8.0);
    } else if (strcasecmp(end, "G") == 0 || strcasecmp(end, "GB") == 0 ||
               strcasecmp(end, "GiB") == 0) {
        result = (unsigned long long)(value * 1024.0 * 1024.0 * 1024.0);
    } else if (strcasecmp(end, "M") == 0 || strcasecmp(end, "MB") == 0 ||
               strcasecmp(end, "MiB") == 0) {
        result = (unsigned long long)(value * 1024.0 * 1024.0);
    } else if (strcasecmp(end, "K") == 0 || strcasecmp(end, "KB") == 0 ||
               strcasecmp(end, "KiB") == 0) {
        result = (unsigned long long)(value * 1024.0);
    } else {
        return 1;
    }

    if (result < MIN_DATA_SIZE || result > MAX_DATA_SIZE_BYTES) {
        return 1;
    }

    *size_bytes = (size_t)result;
    return 0;
}

// 查询端口信息
int get_port_info(struct ibv_context *context, int port, struct ibv_port_attr *port_attr) {
    if (ibv_query_port(context, port, port_attr)) {
        fprintf(stderr, "Failed to query port %d: %s\n", port, strerror(errno));
        return 1;
    }
    
    printf("Port %d: LID %d, state %d\n", port, port_attr->lid, port_attr->state);
    return 0;
}

// 查询GID
int query_gid(struct ibv_context *context, int port, int index, union ibv_gid *gid) {
    if (ibv_query_gid(context, port, index, gid)) {
        fprintf(stderr, "Failed to query GID at index %d: %s\n", index, strerror(errno));
        return 1;
    }
    return 0;
}

// 初始化RDMA资源
int init_rdma_resources(struct rdma_resources *res, const char *dev_name, int ib_port, int gid_idx,
                        size_t buffer_size, uint32_t start_psn, int num_writes, int is_sender) {
    // 保存IB端口号
    res->port_num = ib_port;
    res->buffer_size = buffer_size;
    res->start_psn = start_psn;
    res->num_writes = num_writes;
    res->is_sender = is_sender;
    
    // 找到指定的设备或使用默认设备
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "Failed to get IB devices list: %s\n", strerror(errno));
        return 1;
    }
    
    struct ibv_device *device = NULL;
    
    if (dev_name) {
        // 寻找指定名称的设备
        int i;
        for (i = 0; dev_list[i]; i++) {
            if (strcmp(ibv_get_device_name(dev_list[i]), dev_name) == 0) {
                device = dev_list[i];
                break;
            }
        }
        if (!device) {
            fprintf(stderr, "Device %s not found\n", dev_name);
            ibv_free_device_list(dev_list);
            return 1;
        }
    } else {
        // 使用第一个可用设备
        device = dev_list[0];
        if (!device) {
            fprintf(stderr, "No IB devices found\n");
            ibv_free_device_list(dev_list);
            return 1;
        }
    }
    
    printf("Using device: %s\n", ibv_get_device_name(device));
    
    // 打开设备上下文
    res->context = ibv_open_device(device);
    ibv_free_device_list(dev_list);
    if (!res->context) {
        fprintf(stderr, "Failed to open IB device: %s\n", strerror(errno));
        return 1;
    }

    struct ibv_device_attr device_attr;
    if (ibv_query_device(res->context, &device_attr)) {
        fprintf(stderr, "Failed to query device attributes: %s\n", strerror(errno));
        ibv_close_device(res->context);
        return 1;
    }

    res->max_qp_wr = device_attr.max_qp_wr;

    if ((uint32_t)res->num_writes > res->max_qp_wr) {
        fprintf(stderr, "Requested num_writes=%d exceeds device max_qp_wr=%u\n",
                res->num_writes, res->max_qp_wr);
        ibv_close_device(res->context);
        return 1;
    }

    // 查询端口
    struct ibv_port_attr port_attr;
    if (get_port_info(res->context, res->port_num, &port_attr)) {
        ibv_close_device(res->context);
        return 1;
    }

    res->max_msg_sz = port_attr.max_msg_sz;
    printf("Device capabilities: max_qp_wr=%u, max_msg_sz=%u bytes\n",
           res->max_qp_wr, res->max_msg_sz);

    if (res->is_sender) {
        uint64_t per_wr_limit = res->max_msg_sz;

        if (per_wr_limit == 0) {
            fprintf(stderr, "Device reported max_msg_sz=0\n");
            ibv_close_device(res->context);
            return 1;
        }

        uint64_t required_writes =
            (res->buffer_size + per_wr_limit - 1) / per_wr_limit;

        if ((uint64_t)res->num_writes < required_writes) {
            fprintf(stderr,
                    "data_size=%lu bytes requires at least %lu WRs on this device (max_msg_sz=%u), but num_writes=%d\n",
                    (unsigned long)res->buffer_size, (unsigned long)required_writes,
                    res->max_msg_sz, res->num_writes);
            ibv_close_device(res->context);
            return 1;
        }
    }
    
    // 检查端口状态
    if (port_attr.state != IBV_PORT_ACTIVE) {
        fprintf(stderr, "Warning: Port %d is not in active state (state=%d)\n", 
                res->port_num, port_attr.state);
    }
    
    // 查询GID表
    union ibv_gid gid;
    
    if (query_gid(res->context, res->port_num, gid_idx, &gid)) {
        fprintf(stderr, "Failed to query GID, trying to continue without it\n");
    } else {
        printf("Using GID index %d\n", gid_idx);
        // printf local GID
        printf("Local GID: ");
        for (int i = 0; i < 16; i++) {
            printf("%02x", (unsigned char)gid.raw[i]);
        }
        printf("\n");
        memcpy(res->local_info.gid, &gid, sizeof(gid));
        res->local_info.gid_idx = gid_idx;
    }
    
    // 分配保护域
    res->pd = ibv_alloc_pd(res->context);
    if (!res->pd) {
        fprintf(stderr, "Failed to allocate PD: %s\n", strerror(errno));
        ibv_close_device(res->context);
        return 1;
    }
    
    // 分配缓冲区
    res->buffer = malloc(res->buffer_size);
    if (!res->buffer) {
        fprintf(stderr, "Failed to allocate buffer: %s\n", strerror(errno));
        ibv_dealloc_pd(res->pd);
        ibv_close_device(res->context);
        return 1;
    }
    
    printf("buffer address: %p\n", res->buffer);
    
    // 注册内存区域
    res->mr = ibv_reg_mr(res->pd, res->buffer, res->buffer_size, IBV_ACCESS_LOCAL_WRITE |
                        IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!res->mr) {
        fprintf(stderr, "Failed to register MR: %s\n", strerror(errno));
        free(res->buffer);
        ibv_dealloc_pd(res->pd);
        ibv_close_device(res->context);
        return 1;
    }
    
    // 创建完成队列
    res->cq = ibv_create_cq(res->context, res->num_writes + MAX_RECV_WR, NULL, NULL, 0);
    if (!res->cq) {
        fprintf(stderr, "Failed to create CQ: %s\n", strerror(errno));
        ibv_dereg_mr(res->mr);
        free(res->buffer);
        ibv_dealloc_pd(res->pd);
        ibv_close_device(res->context);
        return 1;
    }
    
    // 创建队列对
    struct ibv_qp_init_attr qp_init_attr = {
        .send_cq = res->cq,
        .recv_cq = res->cq,
        .cap = {
            .max_send_wr = res->is_sender ? res->num_writes : 1,
            .max_recv_wr = MAX_RECV_WR,
            .max_send_sge = MAX_SGE,
            .max_recv_sge = MAX_SGE
        },
        .qp_type = IBV_QPT_RC
    };
    
    res->qp = ibv_create_qp(res->pd, &qp_init_attr);
    if (!res->qp) {
        fprintf(stderr, "Failed to create QP: %s\n", strerror(errno));
        ibv_destroy_cq(res->cq);
        ibv_dereg_mr(res->mr);
        free(res->buffer);
        ibv_dealloc_pd(res->pd);
        ibv_close_device(res->context);
        return 1;
    }
    
    printf("QP created with qp_num: %u\n", res->qp->qp_num);
    
    // 添加 MLX5 UDP 源端口设置
    // 检查是否是 MLX5 设备
    if (strstr(ibv_get_device_name(device), "mlx5") != NULL) {
        printf("Detected MLX5 device, attempting to set UDP source port to 666\n");
        
        // 使用 MLX5 驱动特定功能设置 UDP 源端口
        int ret = mlx5dv_modify_qp_udp_sport(res->qp, 666);
        if (ret == 0) {
            printf("Successfully set UDP source port to 666\n");
        } else {
            fprintf(stderr, "Warning: Failed to set UDP source port to 666: %s\n", strerror(ret));
            fprintf(stderr, "Continuing without custom UDP source port...\n");
        }
    } else {
        printf("Non-MLX5 device detected, skipping UDP source port configuration\n");
    }
    
    // 设置本地连接信息
    res->local_info.lid = port_attr.lid;
    res->local_info.qp_num = res->qp->qp_num;
    res->local_info.rkey = res->mr->rkey;
    res->local_info.addr = (uintptr_t)res->buffer;
    res->local_info.psn = res->start_psn;
    res->local_info.buffer_size = res->buffer_size;
    
    return 0;
}

// QP状态转换函数
int connect_qp(struct rdma_resources *res) {
    // 转换QP到Init状态
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = res->port_num,
        .qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE
    };
    
    int ret = ibv_modify_qp(res->qp, &attr, 
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) {
        fprintf(stderr, "Failed to modify QP to INIT: %s\n", strerror(ret));
        return 1;
    }
    
    printf("QP transitioned to INIT state\n");
    
    // 打印连接信息
    printf("Remote QP number: %u, Remote LID: %u\n", 
           res->remote_info.qp_num, res->remote_info.lid);
    
    // 转换QP到RTR状态
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = res->remote_info.qp_num;
    attr.rq_psn = res->remote_info.psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 0;  // 使用LID路由
    attr.ah_attr.dlid = res->remote_info.lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = res->port_num;
    
    // 如果LID为0，则尝试使用GID路由
    if (res->remote_info.lid == 0) {
        printf("Remote LID is 0, using GID routing\n");
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.dgid = *((union ibv_gid *)res->remote_info.gid);
        attr.ah_attr.grh.sgid_index = res->local_info.gid_idx;
        attr.ah_attr.grh.flow_label = 0;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.traffic_class = 0;

        printf("Remote GID: ");
        for (int i = 0; i < 16; i++) 
            printf("%02x", (unsigned char)res->remote_info.gid[i]);
        printf("\n");
    }
    
    ret = ibv_modify_qp(res->qp, &attr,
                      IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                      IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (ret) {
        fprintf(stderr, "Failed to modify QP to RTR: %s\n", strerror(ret));
        return 1;
    }
    
    printf("QP transitioned to RTR state\n");
    
    // 转换QP到RTS状态
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 0;
    attr.retry_cnt = 1;
    attr.rnr_retry = 0;
    attr.sq_psn = res->local_info.psn;
    attr.max_rd_atomic = 1;
    
    ret = ibv_modify_qp(res->qp, &attr,
                      IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                      IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) {
        fprintf(stderr, "Failed to modify QP to RTS: %s\n", strerror(ret));
        return 1;
    }
    
    printf("QP transitioned to RTS state\n");
    printf("PSN configured: local sq_psn=%u, remote rq_psn=%u\n",
           res->local_info.psn, res->remote_info.psn);
    printf("QP connection established\n");
    return 0;
}

// 发送端交换连接信息
int exchange_connection_info_sender(struct rdma_resources *res, const char *server_addr, int port,
                                    const char *src_ip) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return 1;
    }
    
    struct sockaddr_in server_sockaddr;
    struct hostent *server = gethostbyname(server_addr);
    if (!server) {
        fprintf(stderr, "Failed to resolve server address: %s\n", hstrerror(h_errno));
        close(sock);
        return 1;
    }
    
    memset(&server_sockaddr, 0, sizeof(server_sockaddr));
    server_sockaddr.sin_family = AF_INET;
    memcpy(&server_sockaddr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_sockaddr.sin_port = htons(port);

    if (src_ip) {
        struct sockaddr_in local_sockaddr;

        memset(&local_sockaddr, 0, sizeof(local_sockaddr));
        local_sockaddr.sin_family = AF_INET;
        local_sockaddr.sin_port = htons(0);
        if (inet_pton(AF_INET, src_ip, &local_sockaddr.sin_addr) != 1) {
            fprintf(stderr, "Invalid source IP address: %s\n", src_ip);
            close(sock);
            return 1;
        }

        if (bind(sock, (struct sockaddr *)&local_sockaddr, sizeof(local_sockaddr)) < 0) {
            fprintf(stderr, "Failed to bind sender socket to %s: %s\n", src_ip, strerror(errno));
            close(sock);
            return 1;
        }

        printf("Sender control socket bound to source IP %s\n", src_ip);
    }
    
    if (connect(sock, (struct sockaddr *)&server_sockaddr, sizeof(server_sockaddr)) < 0) {
        fprintf(stderr, "Failed to connect to server: %s\n", strerror(errno));
        close(sock);
        return 1;
    }
    
    printf("Connected to server for exchanging connection info\n");

    struct sockaddr_in local_addr;
    socklen_t local_addr_len = sizeof(local_addr);
    if (getsockname(sock, (struct sockaddr *)&local_addr, &local_addr_len) == 0) {
        char local_ip[INET_ADDRSTRLEN];

        if (inet_ntop(AF_INET, &local_addr.sin_addr, local_ip, sizeof(local_ip))) {
            printf("Sender control connection local endpoint: %s:%u\n",
                   local_ip, ntohs(local_addr.sin_port));
        }
    }
    
    // 发送本地连接信息
    if (write(sock, &res->local_info, sizeof(res->local_info)) != sizeof(res->local_info)) {
        fprintf(stderr, "Failed to send connection info: %s\n", strerror(errno));
        close(sock);
        return 1;
    }
    
    // 接收远程连接信息
    if (read(sock, &res->remote_info, sizeof(res->remote_info)) != sizeof(res->remote_info)) {
        fprintf(stderr, "Failed to receive connection info: %s\n", strerror(errno));
        close(sock);
        return 1;
    }
    
    printf("Received remote connection info: qp_num=%u, lid=%u, psn=%u, remote_buffer_size=%lu bytes\n",
           res->remote_info.qp_num, res->remote_info.lid, res->remote_info.psn,
           (unsigned long)res->remote_info.buffer_size);
    
    close(sock);
    return 0;
}

// 接收端交换连接信息
int exchange_connection_info_receiver(struct rdma_resources *res, int port) {
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        fprintf(stderr, "Failed to create socket: %s\n", strerror(errno));
        return 1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);
    
    int opt = 1;
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        fprintf(stderr, "Failed to set socket options: %s\n", strerror(errno));
        close(listen_sock);
        return 1;
    }
    
    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        fprintf(stderr, "Failed to bind socket: %s\n", strerror(errno));
        close(listen_sock);
        return 1;
    }
    
    if (listen(listen_sock, 1) < 0) {
        fprintf(stderr, "Failed to listen on socket: %s\n", strerror(errno));
        close(listen_sock);
        return 1;
    }
    
    printf("Waiting for connection on port %d...\n", port);
    
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_len);
    if (client_sock < 0) {
        fprintf(stderr, "Failed to accept connection: %s\n", strerror(errno));
        close(listen_sock);
        return 1;
    }
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("Connection established from %s\n", client_ip);
    
    close(listen_sock);
    
    // 接收远程连接信息
    if (read(client_sock, &res->remote_info, sizeof(res->remote_info)) != sizeof(res->remote_info)) {
        fprintf(stderr, "Failed to receive connection info: %s\n", strerror(errno));
        close(client_sock);
        return 1;
    }
    
    printf("Received remote connection info: qp_num=%u, lid=%u, psn=%u, remote_buffer_size=%lu bytes\n",
           res->remote_info.qp_num, res->remote_info.lid, res->remote_info.psn,
           (unsigned long)res->remote_info.buffer_size);
    
    // 发送本地连接信息
    if (write(client_sock, &res->local_info, sizeof(res->local_info)) != sizeof(res->local_info)) {
        fprintf(stderr, "Failed to send connection info: %s\n", strerror(errno));
        close(client_sock);
        return 1;
    }
    
    close(client_sock);
    return 0;
}

// 添加计时器函数
static inline uint64_t read_tsc() {
    uint32_t low, high;
    asm volatile("lfence\n\t"
                 "rdtsc\n\t" 
                 : "=a" (low), "=d" (high)
                 :
                 : "memory");
    return ((uint64_t)high << 32) | low;
}

// 校准TSC频率
uint64_t calibrate_tsc_freq() {
    struct timespec start, end;
    uint64_t tsc_start, tsc_end;
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    tsc_start = read_tsc();
    
    usleep(100000); // 等待100ms
    
    tsc_end = read_tsc();
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) + 
                    (end.tv_nsec - start.tv_nsec) / 1e9;
    
    return (uint64_t)((tsc_end - tsc_start) / elapsed);
}

// 修改后的执行RDMA Write操作
int perform_rdma_write(struct rdma_resources *res, int num_writes) {
    if (res->buffer_size > res->remote_info.buffer_size) {
        fprintf(stderr,
                "Remote buffer is too small: local transfer size=%lu bytes, remote buffer size=%lu bytes\n",
                (unsigned long)res->buffer_size, (unsigned long)res->remote_info.buffer_size);
        return 1;
    }

    // 前i * 1024字节写入i
    for (size_t i = 0; i < res->buffer_size / 1024; i++) {
        for (int j = 0; j < 1024; j++) {
            res->buffer[i * 1024 + j] = (char)(i);
        }
    }

    // 计算每次写入的大小
    size_t chunk_size = res->buffer_size / num_writes;
    
    printf("Performing %d RDMA Write operations, each of size %lu bytes\n", 
           num_writes, chunk_size);
    
    // 分配WR数组和SGE数组
    struct ibv_send_wr *wrs = malloc(num_writes * sizeof(struct ibv_send_wr));
    struct ibv_sge *sges = malloc(num_writes * sizeof(struct ibv_sge));
    
    if (!wrs || !sges) {
        fprintf(stderr, "Failed to allocate memory for WRs or SGEs\n");
        if (wrs) free(wrs);
        if (sges) free(sges);
        return 1;
    }
    
    // 准备所有的WR和SGE
    for (int i = 0; i < num_writes; i++) {
        size_t offset = i * chunk_size;
        size_t length = chunk_size;
        
        // 最后一个块可能会包含余下的所有字节
        if (i == num_writes - 1) {
            length = res->buffer_size - offset;
        }

        if (length > UINT_MAX || length > res->max_msg_sz) {
            fprintf(stderr,
                    "Chunk size %lu exceeds per-WR limit (min(UINT_MAX, max_msg_sz=%u)); increase --num_writes\n",
                    (unsigned long)length, res->max_msg_sz);
            free(wrs);
            free(sges);
            return 1;
        }
        
        // 设置SGE
        sges[i].addr = (uintptr_t)(res->buffer + offset);
        sges[i].length = (uint32_t)length;
        sges[i].lkey = res->mr->lkey;
        
        // 设置WR
        memset(&wrs[i], 0, sizeof(struct ibv_send_wr));
        wrs[i].wr_id = i + 1;  // 使用不同的wr_id来跟踪请求
        wrs[i].sg_list = &sges[i];
        wrs[i].num_sge = 1;
        wrs[i].opcode = IBV_WR_RDMA_WRITE;
        wrs[i].send_flags = IBV_SEND_SIGNALED;
        wrs[i].wr.rdma.remote_addr = res->remote_info.addr + offset;
        wrs[i].wr.rdma.rkey = res->remote_info.rkey;
        
        // 链接到下一个WR（除了最后一个）
        if (i < num_writes - 1) {
            wrs[i].next = &wrs[i + 1];
        } else {
            wrs[i].next = NULL;
        }
        
        printf("Prepared RDMA Write #%d to remote address: %lx, rkey: %u, size: %lu bytes\n", 
               i+1, res->remote_info.addr + offset, res->remote_info.rkey, length);
    }
    
    // 校准TSC频率
    static uint64_t tsc_freq = 0;
    if (tsc_freq == 0) {
        printf("Calibrating TSC frequency...\n");
        tsc_freq = calibrate_tsc_freq();
        printf("TSC frequency: %lu Hz\n", tsc_freq);
    }
    
    printf("Starting to post all RDMA Write operations...\n");
    
    // 记录开始时间
    uint64_t start_tsc = read_tsc();
    
    // 依次post所有WR
    for (int i = 0; i < num_writes; i++) {
        struct ibv_send_wr *bad_wr;
        int ret = ibv_post_send(res->qp, &wrs[i], &bad_wr);
        if (ret) {
            fprintf(stderr, "Failed to post send WR #%d: %s\n", i+1, strerror(ret));
            free(wrs);
            free(sges);
            return 1;
        }
        
        // printf("RDMA Write request #%d posted\n", i+1);
    }
    
    // printf("All RDMA Write operations posted, waiting for completions...\n");
    
    // 等待所有操作完成
    int finished = 0;
    do {
        int rc = wait_for_rdma_completion(res);
        if (rc < 0) {
            free(wrs);
            free(sges);
            return 1;
        }
        finished += rc;
    } while (finished < num_writes);
    
    // 记录结束时间
    uint64_t end_tsc = read_tsc();
    
    // 计算并显示统计信息
    uint64_t elapsed_cycles = end_tsc - start_tsc;
    double elapsed_seconds = (double)elapsed_cycles / tsc_freq;
    double elapsed_microseconds = elapsed_seconds * 1000000.0;
    double elapsed_nanoseconds = elapsed_seconds * 1000000000.0;
    
    // 修改后的统计信息显示部分
    printf("\n========== Performance Statistics ==========\n");
    printf("Total RDMA Write operations: %d\n", num_writes);
    printf("Total data transferred: %lu bytes (%.2f GiB, %.2f Gb)\n",
        (unsigned long)res->buffer_size,
        res->buffer_size / (1024.0 * 1024.0 * 1024.0),
        (res->buffer_size * 8.0) / 1000000000.0);
    printf("Time from first post to last completion:\n");
    printf("  Cycles: %lu\n", elapsed_cycles);
    printf("  Time: %.6f seconds\n", elapsed_seconds);
    printf("  Time: %.3f microseconds\n", elapsed_microseconds);
    printf("  Time: %.0f nanoseconds\n", elapsed_nanoseconds);
    printf("Average time per operation: %.3f microseconds\n", elapsed_microseconds / num_writes);

    // 计算吞吐量，转换为Gbps
    double throughput_bps = (res->buffer_size * 8.0) / elapsed_seconds;  // bits per second
    double throughput_gbps = throughput_bps / (1000.0 * 1000.0 * 1000.0);  // Gbps

    printf("Throughput: %.2f Gbps\n", throughput_gbps);
    printf("Operations per second: %.0f ops/s\n", num_writes / elapsed_seconds);
    printf("============================================\n");
    // 清理内存
    free(wrs);
    free(sges);
    
    printf("All RDMA Write operations completed successfully\n");
    return 0;
}

// 等待RDMA操作完成
int wait_for_rdma_completion(struct rdma_resources *res) {
    struct ibv_wc wc;
    int n;
    
    printf("Waiting for completion...\n");
    
    do {
        n = ibv_poll_cq(res->cq, 1, &wc);
    } while (n == 0);
    
    if (n < 0) {
        fprintf(stderr, "Failed to poll CQ: %s\n", strerror(errno));
        return -1;
    }
    
    if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "WR completed with error: %s (status=%d, vendor_err=%d)\n", 
                ibv_wc_status_str(wc.status), wc.status, wc.vendor_err);
        return -1;
    } else {
        printf("RDMA operation #%ld completed successfully\n", wc.wr_id);
    }
    return 1;
}

// 清理RDMA资源
void cleanup_rdma_resources(struct rdma_resources *res) {
    printf("Cleaning up resources\n");
    if (res->qp) {
        ibv_destroy_qp(res->qp);
        printf("QP destroyed\n");
    }
    if (res->cq) {
        ibv_destroy_cq(res->cq);
        printf("CQ destroyed\n");
    }
    if (res->mr) {
        ibv_dereg_mr(res->mr);
        printf("MR deregistered\n");
    }
    if (res->buffer) {
        free(res->buffer);
        printf("Buffer freed\n");
    }
    if (res->pd) {
        ibv_dealloc_pd(res->pd);
        printf("PD deallocated\n");
    }
    if (res->context) {
        ibv_close_device(res->context);
        printf("Device closed\n");
    }
}

// 打印使用方法
void print_usage(const char *program_name) {
    printf("Usage: %s [-s|-r] [-d device_name] [-i ib_port] [-a server_address] [-p port] [-n num_writes] [-g gid_idx] [-S src_ip] [-P start_psn] [-z data_size]\n", program_name);
    printf("Options:\n");
    printf("  -s, --sender        Run as sender\n");
    printf("  -r, --receiver      Run as receiver\n");
    printf("  -d, --device        RDMA device name to use\n");
    printf("  -i, --ib_port       IB port number to use (default: 1)\n");
    printf("  -a, --address       Server address (required for sender)\n");
    printf("  -p, --port          TCP port number for connection exchange (default: %d)\n", DEFAULT_PORT);
    printf("  -n, --num_writes    Number of RDMA write operations to use (default: %d)\n", DEFAULT_NUM_WRITES);
    printf("  -g, --gid-idx       Local GID index for RoCE routing (default: %d)\n", DEFAULT_GID_IDX);
    printf("  -S, --src-ip        Sender TCP control-plane source IPv4 address\n");
    printf("  -P, --start-psn     Local starting PSN in range [0, %u] (default: %u)\n", MAX_PSN, DEFAULT_START_PSN);
    printf("  -z, --data-size     Transfer buffer size, supports 1G..100Gb (examples: 1G, 8G, 100Gb)\n");
    printf("  -h, --help          Show this help message\n");
}

// 主函数
int main(int argc, char *argv[]) {
    int is_sender = 0;
    int is_receiver = 0;
    const char *dev_name = NULL;
    const char *server_addr = NULL;
    const char *src_ip = NULL;
    int port = DEFAULT_PORT;
    int ib_port = 1;
    int num_writes = DEFAULT_NUM_WRITES;
    int gid_idx = DEFAULT_GID_IDX;
    size_t data_size = DEFAULT_DATA_SIZE;
    uint32_t start_psn = DEFAULT_START_PSN;
    
    // 解析命令行参数
    static struct option long_options[] = {
        {"sender",     no_argument,       0, 's'},
        {"receiver",   no_argument,       0, 'r'},
        {"device",     required_argument, 0, 'd'},
        {"ib_port",    required_argument, 0, 'i'},
        {"address",    required_argument, 0, 'a'},
        {"port",       required_argument, 0, 'p'},
        {"num_writes", required_argument, 0, 'n'},
        {"gid-idx",    required_argument, 0, 'g'},
        {"src-ip",     required_argument, 0, 'S'},
        {"start-psn",  required_argument, 0, 'P'},
        {"data-size",  required_argument, 0, 'z'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "srd:i:a:p:n:g:S:P:z:h", long_options, &option_index)) != -1) {
        switch (opt) {
            case 's':
                is_sender = 1;
                break;
            case 'r':
                is_receiver = 1;
                break;
            case 'd':
                dev_name = optarg;
                break;
            case 'i':
                ib_port = atoi(optarg);
                break;
            case 'a':
                server_addr = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                break;
            case 'n':
                num_writes = atoi(optarg);
                if (num_writes <= 0) {
                    fprintf(stderr, "Number of writes must be positive\n");
                    return 1;
                }
                break;
            case 'g':
                gid_idx = atoi(optarg);
                break;
            case 'S':
                src_ip = optarg;
                break;
            case 'P': {
                char *end = NULL;
                unsigned long psn = strtoul(optarg, &end, 10);

                if (*optarg == '\0' || (end && *end != '\0') || psn > MAX_PSN) {
                    fprintf(stderr, "Invalid start PSN: %s\n", optarg);
                    return 1;
                }
                start_psn = (uint32_t)psn;
                break;
            }
            case 'z':
                if (parse_data_size(optarg, &data_size)) {
                    fprintf(stderr,
                            "Invalid data size: %s (supported range: 1G to 100Gb, examples: 1G, 8G, 100Gb)\n",
                            optarg);
                    return 1;
                }
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // 检查必要参数
    if (!is_sender && !is_receiver) {
        fprintf(stderr, "Error: Must specify either sender (-s) or receiver (-r)\n");
        print_usage(argv[0]);
        return 1;
    }
    
    if (is_sender && is_receiver) {
        fprintf(stderr, "Error: Cannot be both sender and receiver\n");
        print_usage(argv[0]);
        return 1;
    }
    
    if (is_sender && !server_addr) {
        fprintf(stderr, "Error: Sender requires server address (-a)\n");
        print_usage(argv[0]);
        return 1;
    }
    
    if (ib_port <= 0) {
        fprintf(stderr, "Error: Invalid IB port number: %d\n", ib_port);
        print_usage(argv[0]);
        return 1;
    }

    if (gid_idx < 0) {
        fprintf(stderr, "Error: Invalid GID index: %d\n", gid_idx);
        print_usage(argv[0]);
        return 1;
    }

    if (is_sender && ((data_size + (size_t)num_writes - 1) / (size_t)num_writes) > UINT_MAX) {
        fprintf(stderr,
                "Error: data size %lu bytes is too large for %d WRs; increase num_writes\n",
                (unsigned long)data_size, num_writes);
        return 1;
    }
    
    // 打印所有参数
    printf("dev_name: %s, ib_port: %d, server_addr: %s, port: %d, num_writes: %d, gid_idx: %d, src_ip: %s, start_psn: %u, data_size: %lu bytes\n",
           dev_name, ib_port, server_addr, port, num_writes, gid_idx,
           src_ip ? src_ip : "(auto)", start_psn, (unsigned long)data_size);
    
    // 初始化RDMA资源
    struct rdma_resources res = {0};
    if (init_rdma_resources(&res, dev_name, ib_port, gid_idx, data_size, start_psn, num_writes,
                            is_sender)) {
        fprintf(stderr, "Failed to initialize RDMA resources\n");
        return 1;
    }
    
    int ret = 0;
    
    if (is_sender) {
        printf("Running as sender, connecting to %s:%d\n", server_addr, port);
        
        // 清空接收缓冲区
        memset(res.buffer, 0, res.buffer_size);
        
        // 交换连接信息
        if (exchange_connection_info_sender(&res, server_addr, port, src_ip)) {
            fprintf(stderr, "Failed to exchange connection info\n");
            cleanup_rdma_resources(&res);
            return 1;
        }
        
        // 建立RDMA连接
        if (connect_qp(&res)) {
            fprintf(stderr, "Failed to connect QP\n");
            cleanup_rdma_resources(&res);
            return 1;
        }
        
        // 执行RDMA Write，使用指定的写入次数
        if (perform_rdma_write(&res, num_writes)) {
            fprintf(stderr, "Failed to perform RDMA Write\n");
            cleanup_rdma_resources(&res);
            return 1;
        }
        
    } else {
        printf("Running as receiver on port %d\n", port);
        
        // 清空接收缓冲区
        memset(res.buffer, 0, res.buffer_size);
        
        // 交换连接信息
        if (exchange_connection_info_receiver(&res, port)) {
            fprintf(stderr, "Failed to exchange connection info\n");
            cleanup_rdma_resources(&res);
            return 1;
        }
        
        // 建立RDMA连接
        if (connect_qp(&res)) {
            fprintf(stderr, "Failed to connect QP\n");
            cleanup_rdma_resources(&res);
            return 1;
        }
        
        printf("Waiting for RDMA Write operation(s)...\n");
        
        // 等待一段时间，确保RDMA Write完成
        sleep(10);   
    }
    
    // 清理资源
    cleanup_rdma_resources(&res);
    
    return ret;
}
