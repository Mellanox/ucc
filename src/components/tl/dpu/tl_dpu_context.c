/**
 * Copyright (C) Mellanox Technologies Ltd. 2021.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#include "tl_dpu.h"
#include "tl_dpu_coll.h"
#include "utils/arch/cpu.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <assert.h>
#include <netdb.h>
#include <poll.h>
#include <errno.h>
#include <unistd.h>

static ucc_mpool_ops_t ucc_tl_dpu_req_mpool_ops = {
    .chunk_alloc   = ucc_mpool_hugetlb_malloc,
    .chunk_release = ucc_mpool_hugetlb_free,
    .obj_init      = NULL,
    .obj_cleanup   = NULL
};

static void err_cb(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    ucc_error("error handling callback was invoked with status %d (%s)\n",
                    status, ucs_status_string(status));
}

static int _server_connect(ucc_tl_dpu_context_t *ctx, char *hname, uint16_t port)
{
    int sock = 0, n;
    struct addrinfo *res, *t;
    struct addrinfo hints = { 0 };
    char service[64];

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    sprintf(service, "%d", port);
    n = getaddrinfo(hname, service, &hints, &res);

    if (n < 0) {
        tl_error(ctx->super.super.lib, "%s:%d: getaddrinfo(): %s for %s:%s\n", __FILE__,__LINE__, gai_strerror(n), hname, service);
        return -1;
    }

    for (t = res; t; t = t->ai_next) {
        sock = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
        if (sock >= 0) {
            if (!connect(sock, t->ai_addr, t->ai_addrlen))
                break;
            close(sock);
            sock = -1;
        }
    }

    freeaddrinfo(res);
    return sock;
}

UCC_CLASS_INIT_FUNC(ucc_tl_dpu_context_t,
                    const ucc_base_context_params_t *params,
                    const ucc_base_config_t *config)
{ 
    /* TODO: Need handshake with daemon for detection */
    ucc_tl_dpu_context_config_t *tl_dpu_config =
        ucc_derived_of(config, ucc_tl_dpu_context_config_t);

    ucc_status_t        ucc_status = UCC_OK;
    int sockfd = 0, last_dpu_found = 0;
    ucp_worker_params_t worker_params;
    ucp_worker_attr_t   worker_attr;
    ucp_params_t        ucp_params;
    ucp_ep_params_t     ep_params;
    ucp_ep_h            ucp_ep;
    ucp_context_h       ucp_context;
    ucp_worker_h        ucp_worker;
    int i, ret, rail = 0, dpu_count = 0, hca = 0;

    /* Identify DPU */
    char hname[MAX_DPU_HOST_NAME];
    void *rem_worker_addr;
    size_t rem_worker_addr_size; 
    char dpu_hnames[MAX_DPU_COUNT][MAX_DPU_HOST_NAME];
    char dpu_hcanames[MAX_DPU_COUNT][MAX_DPU_HCA_NAME];
    char dpu_tmp[MAX_DPU_HOST_NAME];

    UCC_CLASS_CALL_SUPER_INIT(ucc_tl_context_t, &tl_dpu_config->super,
                              params->context);

    /* Find  DPU based on the host-dpu list */
    gethostname(hname, sizeof(hname) - 1);

    char *h = calloc(1, 256), *dpu;
    FILE *fp = NULL;

    if (strcmp(tl_dpu_config->host_dpu_list,"") != 0) {

        fp = fopen(tl_dpu_config->host_dpu_list, "r");
        if (fp == NULL) {
            tl_error(self->super.super.lib,
                "Unable to open host_dpu_list \"%s\", disabling dpu team\n", tl_dpu_config->host_dpu_list);
            ucc_status = UCC_ERR_NO_MESSAGE;
        }
        else {
            rail = 0, hca = 0;
            while (fscanf(fp,"%s", h) != EOF) {
                if (strcmp(h, hname) == 0) {
                    for (i = 0; i < 2 * MAX_DPU_COUNT; i++) {
                        memset(dpu_tmp, 0, MAX_DPU_HOST_NAME);
                        if (fscanf(fp, "%s", dpu_tmp) != EOF) {

                            if(strchr(dpu_tmp, ',') != NULL)
                            {
                                last_dpu_found = 1;
                                /* remove the tail (,) */
                                memmove(&dpu_tmp[strlen(dpu_tmp) - 1], &dpu_tmp[strlen(dpu_tmp)], 1);
                            }

                            if(strstr(dpu_tmp, "mlx5_") != NULL) {
                                memcpy(dpu_hcanames[hca], dpu_tmp, MAX_DPU_HCA_NAME);
                                hca++;
                                if(last_dpu_found) {
                                    break;
                                }
                                continue;
                            }

                            memcpy(dpu_hnames[rail], dpu_tmp, MAX_DPU_HOST_NAME);
                            rail++;

                            tl_info(self->super.super.lib, "DPU <%s> found!\n",
                                    dpu_tmp);
                        }

                        if(last_dpu_found) { 
                            break;
                        }
                    }
                }
                memset(h, 0, MAX_DPU_HOST_NAME);
            }
            if (rail != hca) {
                fprintf(stderr, "host_to_dpu.list file is not formatted correctly");
                goto err;
            }
            dpu_count = rail;
        }
        if (!dpu_count) {
            ucc_status = UCC_ERR_NO_MESSAGE;
        }
    }
    else {
        tl_error(self->super.super.lib,
            "DPU_ENABLE set, but HOST_LIST not specified. Disabling DPU team!\n");
        ucc_status = UCC_ERR_NO_MESSAGE;
    }
    free(h);

    if (UCC_OK != ucc_status) {
        goto err;
    }


    /* Setting the params for all the DPUs */
    memset(&ucp_params, 0, sizeof(ucp_params));
    ucp_params.field_mask       = UCP_PARAM_FIELD_FEATURES;
    ucp_params.features         = UCP_FEATURE_TAG                        |
                                  UCP_FEATURE_RMA;

    memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask    = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode   = UCS_THREAD_MODE_MULTI;

    worker_attr.field_mask      = UCP_WORKER_ATTR_FIELD_ADDRESS          |
                                  UCP_WORKER_ATTR_FIELD_ADDRESS_FLAGS;
    worker_attr.address_flags   = UCP_WORKER_ADDRESS_FLAG_NET_ONLY;

    ep_params.field_mask        = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS      |
                                  UCP_EP_PARAM_FIELD_ERR_HANDLER         |
                                  UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    ep_params.err_mode          = UCP_ERR_HANDLING_MODE_PEER;
    ep_params.err_handler.cb    = err_cb;

    /* Start connecting to all the DPUs */
    for (rail = 0; rail < dpu_count; rail++) {
        dpu = *(dpu_hnames + rail);

        tl_info(self->super.super.lib, "Connecting to %s with hca id %s", dpu, dpu_hcanames[rail]);

        sockfd = _server_connect(self, dpu, tl_dpu_config->server_port);

        memset(&ucp_context, 0, sizeof(ucp_context_h));
        ucp_config_t *ucp_config;
        ucp_config_read(NULL, NULL, &ucp_config);
        ucp_config_modify(ucp_config, "NET_DEVICES", dpu_hcanames[rail]);
        ucc_status = ucs_status_to_ucc_status(
                        ucp_init(&ucp_params, ucp_config, &ucp_context));
        if (ucc_status != UCC_OK) {
            tl_error(self->super.super.lib,
                "failed ucp_init(%s)\n", ucc_status_string(ucc_status));
            goto err;
        }

        memset(&ucp_worker, 0, sizeof(ucp_worker_h));
        ucc_status = ucs_status_to_ucc_status(
                        ucp_worker_create(ucp_context, &worker_params, &ucp_worker));
        if (ucc_status != UCC_OK) {
            tl_error(self->super.super.lib,
                "failed ucp_worker_create (%s)\n", ucc_status_string(ucc_status));
            goto err_cleanup_context;
        }

        ucp_worker_query(ucp_worker, &worker_attr);

        ret = send(sockfd, &worker_attr.address_length,
                sizeof(&worker_attr.address_length), 0);
        if (ret < 0) {
            tl_error(self->super.super.lib, "send length failed");
            ucc_status = UCC_ERR_NO_MESSAGE;
            goto err;
        }
        ret = send(sockfd, worker_attr.address, worker_attr.address_length, 0);
        if (ret < 0) {
            tl_error(self->super.super.lib, "send address failed");
            ucc_status = UCC_ERR_NO_MESSAGE;
            goto err;
        }
        ret = send(sockfd, &tl_dpu_config->pipeline_buffer_size, sizeof(size_t), 0);
        if (ret < 0) {
            tl_error(self->super.super.lib, "send pipeline size failed");
            ucc_status = UCC_ERR_NO_MESSAGE;
            goto err;
        }
        ret = send(sockfd, &tl_dpu_config->pipeline_num_buffers, sizeof(size_t), 0);
        if (ret < 0) {
            tl_error(self->super.super.lib, "send pipeline num buffers failed");
            ucc_status = UCC_ERR_NO_MESSAGE;
            goto err;
        }
        ret = recv(sockfd, &rem_worker_addr_size, sizeof(rem_worker_addr_size), MSG_WAITALL);
        if (ret < 0) {
            tl_error(self->super.super.lib, "recv address length failed");
            ucc_status = UCC_ERR_NO_MESSAGE;
            goto err;
        }
        rem_worker_addr = ucc_malloc(rem_worker_addr_size, "rem_worker_addr");
        ep_params.address = rem_worker_addr;
        if (NULL == rem_worker_addr) {
            tl_error(self->super.super.lib, "failed to allocate rem_worker_addr");
            ucc_status = UCC_ERR_NO_MESSAGE;
            goto err;
        }
        ret = recv(sockfd, rem_worker_addr, rem_worker_addr_size, MSG_WAITALL);
        if (ret < 0) {
            tl_error(self->super.super.lib, "recv address failed");
            ucc_status = UCC_ERR_NO_MESSAGE;
            goto err;
        }
        
        uint32_t global_rank = UCC_TL_CTX_OOB(self).oob_ep;
        ret = send(sockfd, &global_rank, sizeof(uint32_t), 0);
        if (ret < 0) {
            tl_error(self->super.super.lib, "send global rank failed");
            ucc_status = UCC_ERR_NO_MESSAGE;
            goto err;
        }

        uint32_t global_size = UCC_TL_CTX_OOB(self).n_oob_eps;
        ret = send(sockfd, &global_size, sizeof(uint32_t), 0);
        if (ret < 0) {
            tl_error(self->super.super.lib, "send global size failed");
            ucc_status = UCC_ERR_NO_MESSAGE;
            goto err;
        }

        memset(&ucp_ep, 0, sizeof(ucp_ep_h));
        ucc_status = ucs_status_to_ucc_status(
                        ucp_ep_create(ucp_worker, &ep_params, &ucp_ep));
        free(worker_attr.address);
        ucc_free(rem_worker_addr);
        close(sockfd);
        if (ucc_status != UCC_OK) {
            tl_error(self->super.super.lib, "failed to connect to %s (%s)\n",
                           dpu, ucc_status_string(ucc_status));
            goto err_cleanup_worker;
        }

        self->dpu_ctx_list[rail].ucp_context                 = ucp_context;
        self->dpu_ctx_list[rail].ucp_worker                  = ucp_worker;
        self->dpu_ctx_list[rail].ucp_ep                      = ucp_ep;
        self->dpu_ctx_list[rail].inflight                    = 0;
        self->dpu_ctx_list[rail].coll_id_issued              = 0;
        self->dpu_ctx_list[rail].coll_id_completed           = 0;
        self->dpu_ctx_list[rail].get_sync.count_serviced     = 0;
        self->dpu_ctx_list[rail].get_sync.coll_id            = 0;
    }

    ucc_status = ucc_mpool_init(&self->req_mp, 0,
            sizeof(ucc_tl_dpu_task_t), 0, UCC_CACHE_LINE_SIZE, 8, UINT_MAX,
            &ucc_tl_dpu_req_mpool_ops, worker_params.thread_mode,
            "tl_dpu_req_mp");
    if (UCC_OK != ucc_status) {
        tl_error(self->super.super.lib, "failed to initialize tl_dpu_req mpool");
        goto err_cleanup_mpool;
    }

    self->dpu_per_node_cnt = dpu_count;

    tl_info(self->super.super.lib, "context created for %d DPUs", dpu_count);
    return ucc_status;

err_cleanup_mpool:
    ucc_mpool_cleanup(&self->req_mp, 1);
err_cleanup_worker:
    ucp_worker_destroy(self->dpu_ctx_list[rail].ucp_worker);
    ucp_cleanup(self->dpu_ctx_list[rail].ucp_context);
err_cleanup_context:
    for (i = 0; i < rail-1; i++) {
        ucp_worker_destroy(self->dpu_ctx_list[i].ucp_worker);
        ucp_cleanup(self->dpu_ctx_list[i].ucp_context);
    }

err:
    return ucc_status;
}

UCC_CLASS_CLEANUP_FUNC(ucc_tl_dpu_context_t)
{
    ucp_request_param_t param;
    ucs_status_t ucs_status;
    ucs_status_ptr_t close_req;
    int rail;

    tl_info(self->super.super.lib, "finalizing tl context: %p", self);
    
    for (rail = 0; rail < self->dpu_per_node_cnt; rail++) {
    
        ucp_worker_flush(self->dpu_ctx_list[rail].ucp_worker);

        param.op_attr_mask  = UCP_OP_ATTR_FIELD_FLAGS;
        param.flags         = UCP_EP_CLOSE_FLAG_FORCE;
        close_req           = ucp_ep_close_nbx(self->dpu_ctx_list[rail].ucp_ep, &param);
        if (UCS_PTR_IS_PTR(close_req)) {
            do {
                ucp_worker_progress(self->dpu_ctx_list[rail].ucp_worker);
                ucs_status = ucp_request_check_status(close_req);
            } while (ucs_status == UCS_INPROGRESS);
            ucp_request_free (close_req);
        } else if (UCS_PTR_STATUS(close_req) != UCS_OK) {
            tl_error(self->super.super.lib, "failed to close ep %p\n", (void *)self->dpu_ctx_list[rail].ucp_ep);
        }
        ucp_worker_destroy(self->dpu_ctx_list[rail].ucp_worker);
        ucp_cleanup(self->dpu_ctx_list[rail].ucp_context);
    }

    ucc_mpool_cleanup(&self->req_mp, 1);
}

UCC_CLASS_DEFINE(ucc_tl_dpu_context_t, ucc_tl_context_t);

ucc_status_t ucc_tl_dpu_get_context_attr(const ucc_base_context_t *context,
                                         ucc_base_ctx_attr_t      *attr)
{
    if (attr->attr.mask & UCC_CONTEXT_ATTR_FIELD_CTX_ADDR_LEN) {
        attr->attr.ctx_addr_len = 0;
    }
    return UCC_OK;
}
