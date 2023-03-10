/*
 * Copyright (C) Mellanox Technologies Ltd. 2022.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <libgen.h>
#include <linux/limits.h>

#include <ucc/api/ucc.h>
#include "server_ucc.h"

#define MAX_PPN        128
#define MASTER_PORT    10000

extern char **environ;

/* DPU Master Channel */
typedef struct dpu_mc_t {
    char *hname;
    char *ip;
    int connfd, listenfd;
    uint32_t master_port;
    uint32_t server_port;
    uint32_t local_rank;
} dpu_mc_t;

#ifdef NDEBUG
#define DPU_LOG(...)
#else
#define DPU_LOG(_fmt, ...)                                  \
do {                                                        \
    fprintf(stderr, "%s:%d:%s(): " _fmt,                    \
            __FILE__, __LINE__, __func__, ##__VA_ARGS__);   \
    fflush(stderr);                                         \
} while (0)
#endif

void _cleanup()
{
}

void _sighandler(int signal)
{
    printf("Caught signal %d\n", signal);
}

static int _dpu_host_to_ip(dpu_mc_t *mc)
{
    struct hostent *he;
    struct in_addr **addr_list;
    int i;

    mc->hname = calloc(1, 100 * sizeof(char));
    mc->ip = malloc(100 * sizeof(char));

    int ret = gethostname(mc->hname, 100);
    if (ret) {
        return 1;
    }

    if ( (he = gethostbyname( mc->hname ) ) == NULL)
    {
        // get the host info
        herror("gethostbyname");
        return 1;
    }

    addr_list = (struct in_addr **) he->h_addr_list;
    for(i = 0; addr_list[i] != NULL; i++)
    {
        //Return the first one;
        strcpy(mc->ip , inet_ntoa(*addr_list[i]) );
        return UCC_OK;
    }
    return UCC_ERR_NO_MESSAGE;
}

static int _dpu_master_listen(dpu_mc_t *mc)
{
    struct sockaddr_in serv_addr;
    mc->master_port = MASTER_PORT;

    if(_dpu_host_to_ip(mc)) {
        return UCC_ERR_NO_MESSAGE;
    }

    printf("DPU Master %d listening on %s:%d\n", getpid(), mc->hname, mc->master_port);
    /* creates an UN-named socket inside the kernel and returns
     * an integer known as socket descriptor
     * This function takes domain/family as its first argument.
     * For Internet family of IPv4 addresses we use AF_INET
     */
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > listenfd) {
        fprintf(stderr, "socket() failed (%s)\n", strerror(errno));
        goto err_ip;
    }

    int opt = 1;
    if(0 > setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt))) {
        fprintf(stderr, "setsockopt() failed (%s)\n", strerror(errno));
        goto err_sock;
    }  

    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(mc->master_port);

    /* The call to the function "bind()" assigns the details specified
     * in the structure 『serv_addr' to the socket created in the step above
     */
    if (0 > bind(listenfd, (struct sockaddr*)&serv_addr,
                 sizeof(serv_addr))) {
        fprintf(stderr, "Failed to bind() (%s)\n", strerror(errno));
        goto err_sock;
    }

    /* The call to the function "listen()" with second argument as 10 specifies
     * maximum number of client connections that server will queue for this listening
     * socket.
     */
    if (0 > listen(listenfd, MAX_PPN)) {
        fprintf(stderr, "listen() failed (%s)\n", strerror(errno));
        goto err_sock;
    }

    mc->listenfd = listenfd;
    return UCC_OK;
err_sock:
    close(listenfd);
err_ip:
    return UCC_ERR_NO_MESSAGE;
}

static int _dpu_master_accept(dpu_mc_t *mc)
{
    int ret;
    uint32_t local_rank;

    mc->connfd = accept(mc->listenfd, (struct sockaddr*)NULL, NULL);
    if (0 > mc->connfd) {
        fprintf(stderr, "Error in accept (%s)!\n", strerror(errno));
        ret = UCC_ERR_NO_MESSAGE;
        goto err_accept;
    }

    ret = recv(mc->connfd, &local_rank, sizeof(uint32_t), MSG_WAITALL);
    if (-1 == ret) {
        fprintf(stderr, "recv local rank failed!\n");
        ret = UCC_ERR_NO_MESSAGE;
        goto err_recv;
    } else {
        ret = UCC_OK;
        mc->local_rank = local_rank;
        mc->server_port = mc->master_port + mc->local_rank + 1;
        DPU_LOG("Recvd spawn request for local rank %u\n", local_rank);
    }

    return UCC_OK;

err_recv:
    close(mc->connfd);
err_accept:
    return ret;
}

const char *server_bin = "dpu_server";

static int _dpu_master_spawn_server(dpu_mc_t *mc, char **argv)
{
    pid_t child = fork();
    if (child == 0) {
        /* Add master_port to environment */
        char pstr[32];
        sprintf(pstr, "%d", mc->server_port);
        setenv("DPU_SERVER_LISTEN_PORT", pstr, 1);

        /* Get location for dpu_master */
        char master_cmd[PATH_MAX], server_cmd[PATH_MAX];
        ssize_t count = readlink("/proc/self/exe", master_cmd, PATH_MAX);
        master_cmd[count] = '\0';
        const char *path;
        if (count != -1) {
            path = dirname(master_cmd);
        }
        snprintf(server_cmd, PATH_MAX, "%s/%s", path, server_bin);

        /* Spawn DPU server for local rank */
        execve(server_cmd, argv, environ);

        /* if execve returns it has failed */
        fprintf(stderr, "Could not spawn dpu server [%s] for local rank %u!\n",
                server_cmd, mc->local_rank);
    }

    return UCC_OK;
}

static int _dpu_master_reply(dpu_mc_t *mc)
{
    int ret = send(mc->connfd, &mc->server_port, sizeof(uint32_t), MSG_WAITALL);
    if (-1 == ret) {
        fprintf(stderr, "send server port failed!\n");
        close(mc->connfd);
        return UCC_ERR_NO_MESSAGE;
    }
    close(mc->connfd);
    return UCC_OK;
}

int main(int argc, char **argv, char **envp)
{
    dpu_mc_t mc = {0};
    ucc_status_t status;

    UCCCHECK_GOTO(_dpu_master_listen(&mc), err, status);

    while (1) {
        UCCCHECK_GOTO(_dpu_master_accept(&mc), err, status);
        UCCCHECK_GOTO(_dpu_master_spawn_server(&mc, argv), err, status);
        UCCCHECK_GOTO(_dpu_master_reply(&mc), err, status);
    }


err:
    if (mc.connfd)   { close(mc.connfd);   }
    if (mc.listenfd) { close(mc.listenfd); }
    if (mc.hname)    { free(mc.hname); }
    if (mc.ip)       { free(mc.ip);    }
    return EXIT_FAILURE;
}
