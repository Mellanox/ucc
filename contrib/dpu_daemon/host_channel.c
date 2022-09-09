/*
 * Copyright (C) Mellanox Technologies Ltd. 2020.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include "host_channel.h"
#include <unistd.h>
#include <ucc/api/ucc.h>

#define UCC_DT_PREDEFINED_ID(_dt) ((_dt) >> UCC_DATATYPE_SHIFT)

#define UCC_DT_IS_PREDEFINED(_dt) \
    (((_dt) & UCC_DATATYPE_CLASS_MASK) == UCC_DATATYPE_PREDEFINED)

size_t ucc_dt_predefined_sizes[UCC_DT_PREDEFINED_LAST] = {
     [UCC_DT_PREDEFINED_ID(UCC_DT_INT8)]     = 1,
     [UCC_DT_PREDEFINED_ID(UCC_DT_UINT8)]    = 1,
     [UCC_DT_PREDEFINED_ID(UCC_DT_INT16)]    = 2,
     [UCC_DT_PREDEFINED_ID(UCC_DT_UINT16)]   = 2,
     [UCC_DT_PREDEFINED_ID(UCC_DT_FLOAT16)]  = 2,
     [UCC_DT_PREDEFINED_ID(UCC_DT_BFLOAT16)] = 2,
     [UCC_DT_PREDEFINED_ID(UCC_DT_INT32)]    = 4,
     [UCC_DT_PREDEFINED_ID(UCC_DT_UINT32)]   = 4,
     [UCC_DT_PREDEFINED_ID(UCC_DT_FLOAT32)]  = 4,
     [UCC_DT_PREDEFINED_ID(UCC_DT_INT64)]    = 8,
     [UCC_DT_PREDEFINED_ID(UCC_DT_UINT64)]   = 8,
     [UCC_DT_PREDEFINED_ID(UCC_DT_FLOAT64)]  = 8,
     [UCC_DT_PREDEFINED_ID(UCC_DT_INT128)]   = 16,
     [UCC_DT_PREDEFINED_ID(UCC_DT_UINT128)]  = 16,
};

size_t dpu_ucc_dt_size(ucc_datatype_t dt)
{
    if (UCC_DT_IS_PREDEFINED(dt)) {
        return ucc_dt_predefined_sizes[UCC_DT_PREDEFINED_ID(dt)];
    }
    return 0;
}

static int _dpu_host_to_ip(dpu_hc_t *hc)
{
//     printf ("%s\n", __FUNCTION__);
    struct hostent *he;
    struct in_addr **addr_list;
    int i;

    hc->hname = calloc(1, 100 * sizeof(char));
    hc->ip = malloc(100 * sizeof(char));

    int ret = gethostname(hc->hname, 100);
    if (ret) {
        return 1;
    }

    if ( (he = gethostbyname( hc->hname ) ) == NULL)
    {
        // get the host info
        herror("gethostbyname");
        return 1;
    }

    addr_list = (struct in_addr **) he->h_addr_list;
    for(i = 0; addr_list[i] != NULL; i++)
    {
        //Return the first one;
        strcpy(hc->ip , inet_ntoa(*addr_list[i]) );
        return UCC_OK;
    }
    return UCC_ERR_NO_MESSAGE;
}

static int _dpu_listen(dpu_hc_t *hc)
{
    struct sockaddr_in serv_addr;

    if(_dpu_host_to_ip(hc)) {
        return UCC_ERR_NO_MESSAGE;
    }

    /* creates an UN-named socket inside the kernel and returns
     * an integer known as socket descriptor
     * This function takes domain/family as its first argument.
     * For Internet family of IPv4 addresses we use AF_INET
     */
    hc->listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (0 > hc->listenfd) {
        fprintf(stderr, "socket() failed (%s)\n", strerror(errno));
        goto err_ip;
    }
    memset(&serv_addr, 0, sizeof(serv_addr));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(hc->port);

    /* The call to the function "bind()" assigns the details specified
     * in the structure 『serv_addr' to the socket created in the step above
     */
    if (0 > bind(hc->listenfd, (struct sockaddr*)&serv_addr,
                 sizeof(serv_addr))) {
        fprintf(stderr, "Failed to bind() (%s)\n", strerror(errno));
        goto err_sock;
    }

    /* The call to the function "listen()" with second argument as 10 specifies
     * maximum number of client connections that server will queue for this listening
     * socket.
     */
    if (0 > listen(hc->listenfd, 10)) {
        fprintf(stderr, "listen() failed (%s)\n", strerror(errno));
        goto err_sock;
    }

    return UCC_OK;
err_sock:
    close(hc->listenfd);
err_ip:
    free(hc->ip);
    free(hc->hname);
    return UCC_ERR_NO_MESSAGE;
}

static void _dpu_listen_cleanup(dpu_hc_t *hc)
{
    DPU_LOG("Cleaning up host channel\n");
    close(hc->listenfd);
    free(hc->ip);
    free(hc->hname);
}


ucc_status_t _dpu_req_test(ucs_status_ptr_t request)
{
    if (request == NULL) {
        return UCS_OK;
    }
    else if (UCS_PTR_IS_ERR(request)) {
        fprintf (stderr, "unable to complete UCX request\n");
        return UCS_PTR_STATUS(request);
    }
    else {
        return ucp_request_check_status(request);
    }
}

static void err_cb(void *arg, ucp_ep_h ep, ucs_status_t status)
{
    printf ("error handling callback was invoked with status %d (%s)\n",
            status, ucs_status_string(status));
}

static ucs_status_t _dpu_flush_host_eps(dpu_hc_t *hc)
{
    int i;
    ucp_request_param_t param = {};
    ucs_status_ptr_t request;

    for (i = 0; i < hc->world_size; i++) {
        request = ucp_ep_flush_nbx(hc->host_eps[i], &param);
        _dpu_request_wait(hc->ucp_worker, request);
    }
    return UCS_OK;
}

static ucs_status_t _dpu_worker_flush(dpu_hc_t *hc)
{
    ucp_request_param_t param = {};
    ucs_status_ptr_t request = ucp_worker_flush_nbx(hc->ucp_worker, &param);
    return _dpu_request_wait(hc->ucp_worker, request);
}

static int _dpu_ucx_init(dpu_hc_t *hc)
{
    ucp_params_t ucp_params;
    ucs_status_t status;
    ucp_worker_params_t worker_params;
    int ret = UCC_OK;

    memset(&ucp_params, 0, sizeof(ucp_params));
    ucp_params.field_mask = UCP_PARAM_FIELD_FEATURES;
    ucp_params.features = UCP_FEATURE_TAG |
                          UCP_FEATURE_RMA;

    status = ucp_init(&ucp_params, NULL, &hc->ucp_ctx);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to ucp_init(%s)\n", ucs_status_string(status));
        ret = UCC_ERR_NO_MESSAGE;
        goto err;
    }

    memset(&worker_params, 0, sizeof(worker_params));
    worker_params.field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_MULTI;

    status = ucp_worker_create(hc->ucp_ctx, &worker_params, &hc->ucp_worker);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to ucp_worker_create (%s)\n", ucs_status_string(status));
        ret = UCC_ERR_NO_MESSAGE;
        goto err_cleanup;
    }

    hc->worker_attr.field_mask = UCP_WORKER_ATTR_FIELD_ADDRESS |
            UCP_WORKER_ATTR_FIELD_ADDRESS_FLAGS;
    hc->worker_attr.address_flags = UCP_WORKER_ADDRESS_FLAG_NET_ONLY;
    status = ucp_worker_query (hc->ucp_worker, &hc->worker_attr);
    if (UCS_OK != status) {
        fprintf(stderr, "failed to ucp_worker_query (%s)\n", ucs_status_string(status));
        ret = UCC_ERR_NO_MESSAGE;
        goto err_worker;
    }

    return ret;
err_worker:
    ucp_worker_destroy(hc->ucp_worker);
err_cleanup:
    ucp_cleanup(hc->ucp_ctx);
err:
    return ret;
}

static int _dpu_ucx_fini(dpu_hc_t *hc){
    ucp_worker_release_address(hc->ucp_worker, hc->worker_attr.address);
    ucp_worker_destroy(hc->ucp_worker);
    ucp_cleanup(hc->ucp_ctx);
}

static int _dpu_hc_buffer_alloc(dpu_hc_t *hc, dpu_mem_t *mem, size_t size)
{
    ucp_mem_map_params_t mem_params;
    ucp_mem_attr_t mem_attr;
    ucs_status_t status;
    int ret = UCC_OK;

    memset(mem, 0, sizeof(*mem));
    mem->base = calloc(size, 1);
    if (mem->base == NULL) {
        fprintf(stderr, "failed to allocate %lu bytes base %p\n", size, mem->base);
        ret = UCC_ERR_NO_MEMORY;
        goto out;
    }

    memset(&mem_params, 0, sizeof(ucp_mem_map_params_t));
    mem_params.address = mem->base;
    mem_params.length = size;

    mem_params.field_mask = UCP_MEM_MAP_PARAM_FIELD_FLAGS |
                       UCP_MEM_MAP_PARAM_FIELD_LENGTH |
                       UCP_MEM_MAP_PARAM_FIELD_ADDRESS;

    status = ucp_mem_map(hc->ucp_ctx, &mem_params, &mem->memh);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to ucp_mem_map (%s)\n", ucs_status_string(status));
        ret = UCC_ERR_NO_MESSAGE;
        goto err_calloc;
    }

    mem_attr.field_mask = UCP_MEM_ATTR_FIELD_ADDRESS |
                          UCP_MEM_ATTR_FIELD_LENGTH;

    status = ucp_mem_query(mem->memh, &mem_attr);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to ucp_mem_query (%s)\n", ucs_status_string(status));
        ret = UCC_ERR_NO_MESSAGE;
        goto err_map;
    }

    DPU_LOG("Requested to map base %p len %zu registered base %p len %zu\n",
            mem_params.address, mem_params.length, mem_attr.address, mem_attr.length);
    assert(mem_attr.length >= mem_params.length);
    assert(mem_attr.address == mem_params.address);

    status = ucp_rkey_pack(hc->ucp_ctx, mem->memh,
                           &mem->rkey.rkey_addr,
                           &mem->rkey.rkey_addr_len);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to ucp_rkey_pack (%s)\n", ucs_status_string(status));
        ret = UCC_ERR_NO_MESSAGE;
        goto err_map;
    }
    
    goto out;
err_map:
    ucp_mem_unmap(hc->ucp_ctx, mem->memh);
err_calloc:
    free(mem->base);
out:
    return ret;
}

static int _dpu_hc_buffer_free(dpu_hc_t *hc, dpu_mem_t *mem)
{
    ucp_rkey_buffer_release(mem->rkey.rkey_addr);
    ucp_mem_unmap(hc->ucp_ctx, mem->memh);
    free(mem->base);
}

static void _dpu_hc_reset_buf(dpu_buf_t *buf)
{
    buf->state = FREE;
    buf->count = 0;
    buf->ucp_req = NULL;
    buf->ucc_req = NULL;
}

static void _dpu_hc_reset_pipeline(dpu_hc_t *hc)
{
    dpu_pipeline_t *pipe = &hc->pipeline;
    for (int i=0; i<pipe->num_buffers; i++) {
        _dpu_hc_reset_buf(&pipe->buffers[i]);
    }
    pipe->my_count = pipe->my_offset = 0;
    pipe->count_requested = pipe->count_serviced = 0;
}

static  int _dpu_hc_init_pipeline(dpu_hc_t *hc)
{
    int i, ret;
    dpu_pipeline_t *pipe = &hc->pipeline;

    assert(pipe->buffer_size > 0);
    assert(pipe->num_buffers > 0);

    ret = _dpu_hc_buffer_alloc(hc, &hc->mem_segs.in, pipe->buffer_size * pipe->num_buffers);
    if (ret) {
        goto out;
    }
    ret = _dpu_hc_buffer_alloc(hc, &hc->mem_segs.out, pipe->buffer_size * pipe->num_buffers); /* FIXME : Remove? */
    if (ret) {
        goto err_put;
    }
    ret = _dpu_hc_buffer_alloc(hc, &hc->mem_segs.sync, sizeof(dpu_put_sync_t));
    if (ret) {
        goto err_get;
    }

    pipe->buffers = calloc(sizeof(dpu_buf_t), pipe->num_buffers);
    for (i=0; i<pipe->num_buffers; i++) {
        pipe->buffers[i].buf = (char*)hc->mem_segs.in.base + pipe->buffer_size * i;
    }

    _dpu_hc_reset_pipeline(hc);
    goto out;
err_get:
    _dpu_hc_buffer_free(hc, &hc->mem_segs.out);
err_put:
    _dpu_hc_buffer_free(hc, &hc->mem_segs.in);
out:
    return ret;
}

int dpu_hc_init(dpu_hc_t *hc)
{
    int ret = UCC_OK;
    // memset(hc, 0, sizeof(*hc));

    /* Start listening */
    ret = _dpu_listen(hc);
    if (ret) {
        goto err_ip;
    }
    goto out;

err_ip:
    _dpu_listen_cleanup(hc);
out:
    return ret;
}

static void dpu_coll_collect_host_addrs(dpu_ucc_comm_t *comm, void *addr, size_t addr_len, void *outbuf)
{
    ucs_status_t status;
    ucc_coll_req_h request;
    ucc_team_h team = comm->team;
    ucc_rank_t team_size = 0;
    UCC_CHECK(ucc_team_get_size(team, &team_size));
        
    ucc_coll_args_t coll = {
        .coll_type = UCC_COLL_TYPE_ALLGATHER,
        .src.info = {
            .buffer   = addr,
            .count    = addr_len,
            .datatype = UCC_DT_INT8,
            .mem_type = UCC_MEMORY_TYPE_HOST,
        },
        .dst.info = {
            .buffer   = outbuf,
            .count    = addr_len * team_size,
            .datatype = UCC_DT_INT8,
            .mem_type = UCC_MEMORY_TYPE_HOST,
        },
    };

    DPU_LOG("Issue Allgather from ranks %d src %p dst %p bytes %lu\n",
            team_size, addr, outbuf, addr_len);
    UCC_CHECK(ucc_collective_init(&coll, &request, team));
    UCC_CHECK(ucc_collective_post(request));
    while (UCC_OK != ucc_collective_test(request)) {
        ucc_context_progress(comm->ctx);
    }
    UCC_CHECK(ucc_collective_finalize(request));
}

int dpu_hc_connect_localhost_ep(dpu_hc_t *hc)
{
    ucs_status_t status;
    ucp_ep_params_t ep_params;
    ep_params.field_mask    = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                              UCP_EP_PARAM_FIELD_ERR_HANDLER |
                              UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    ep_params.err_mode		= UCP_ERR_HANDLING_MODE_PEER;
    ep_params.err_handler.cb    = err_cb;
    ep_params.address = hc->rem_worker_addr;

    status = ucp_ep_create(hc->ucp_worker, &ep_params, &hc->localhost_ep);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to create endpoint on dpu to local host %d (%s)\n",
                status, ucs_status_string(status));
    }

    return status;
}

static ucs_status_t _dpu_create_remote_host_eps(dpu_hc_t *hc, dpu_ucc_comm_t *comm)
{
    ucs_status_t status;
    ucp_ep_params_t ep_params;
    int i;
    void *remote_addrs = NULL;
    size_t rem_worker_addr_len = hc->rem_worker_addr_len;
    void *rem_worker_addr = hc->rem_worker_addr;

    /* Allgather Host EP addresses */
    hc->host_eps = calloc(hc->world_size, sizeof(ucp_ep_h));
    remote_addrs = calloc(hc->world_size, rem_worker_addr_len);
    dpu_coll_collect_host_addrs(comm, rem_worker_addr, rem_worker_addr_len, remote_addrs);

    ep_params.field_mask    = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS |
                              UCP_EP_PARAM_FIELD_ERR_HANDLER |
                              UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE;
    ep_params.err_mode		= UCP_ERR_HANDLING_MODE_PEER;
    ep_params.err_handler.cb    = err_cb;

    /* Connect to all remote hosts */
    for (i = 0; i < hc->world_size; i++) {
        /* Skip Local Host, Already Connected */
        if (i == hc->world_rank) {
            hc->host_eps[i] = hc->localhost_ep;
            continue;
        }

        ep_params.address = remote_addrs + i * rem_worker_addr_len;
        status = ucp_ep_create(hc->ucp_worker, &ep_params, &hc->host_eps[i]);
        if (status != UCS_OK) {
            fprintf(stderr, "failed to create endpoint on dpu to host %d (%s)\n",
                    i, ucs_status_string(status));
            return UCC_ERR_NO_MESSAGE;
        }
    }

    hc->host_rkeys = calloc(hc->world_size, sizeof(host_rkey_t));
    hc->host_src_rkeys = calloc(hc->world_size, sizeof(ucp_rkey_h));
    hc->host_dst_rkeys = calloc(hc->world_size, sizeof(ucp_rkey_h));
    hc->world_lsyncs = calloc(hc->world_size, sizeof(dpu_put_sync_t));
    
    memset(&hc->req_param, 0, sizeof(hc->req_param));
    // hc->req_param.op_attr_mask = UCP_OP_ATTR_FLAG_NO_IMM_CMPL;

    return UCC_OK;
}

static int _dpu_close_host_eps(dpu_hc_t *hc)
{
    ucp_request_param_t param;
    ucs_status_t status;
    void *close_req;
    int ret = UCC_OK;
    int i;

    param.op_attr_mask  = UCP_OP_ATTR_FIELD_FLAGS;
    param.flags         = UCP_EP_CLOSE_FLAG_FORCE;

    for (i = 0; i < hc->world_size; i++) {
        close_req = ucp_ep_close_nbx(hc->host_eps[i], &param);
        if (UCS_PTR_IS_PTR(close_req)) {
            do {
                ucp_worker_progress(hc->ucp_worker);
                status = ucp_request_check_status(close_req);
            } while (status == UCS_INPROGRESS);

            ucp_request_free(close_req);
        }
        else if (UCS_PTR_STATUS(close_req) != UCS_OK) {
            fprintf(stderr, "failed to close ep %p\n", (void *)hc->host_eps[i]);
            ret = UCC_ERR_NO_MESSAGE;
        }
    }
    free(hc->host_eps);
    free(hc->host_rkeys);
    free(hc->host_src_rkeys);
    free(hc->host_dst_rkeys);
    return ret;
}

ucs_status_t _dpu_request_wait(ucp_worker_h ucp_worker, ucs_status_ptr_t request)
{
    ucs_status_t status;

    /* immediate completion */
    if (request == NULL) {
        return UCS_OK;
    }
    else if (UCS_PTR_IS_ERR(request)) {
        status = ucp_request_check_status(request);
        fprintf (stderr, "unable to complete UCX request (%s)\n", ucs_status_string(status));
        return UCS_PTR_STATUS(request);
    }
    else {
        do {
            ucp_worker_progress(ucp_worker);
            status = ucp_request_check_status(request);
        } while (status == UCS_INPROGRESS);
        ucp_request_free(request);
    }

    return status;
}

int dpu_hc_accept_job(dpu_hc_t *hc)
{
    int ret;
    hc->job_id++;

    /* init ucx worker */
    ret = _dpu_ucx_init(hc);
    if (ret) {
        goto err_ucx;
    }

    /* In the call to accept(), the server is put to sleep and when for an incoming
    * client request, the three way TCP handshake* is complete, the function accept()
    * wakes up and returns the socket descriptor representing the client socket.
    */
    DPU_LOG("Waiting for connection from Job Id %d at port %u\n", hc->job_id, hc->port);
    hc->connfd = accept(hc->listenfd, (struct sockaddr*)NULL, NULL);
    if (-1 == hc->connfd) {
        fprintf(stderr, "Error in accept (%s)!\n", strerror(errno));
        ret = UCC_ERR_NO_MESSAGE;
        goto err;
    }
    DPU_LOG("Connection established from Job Id %d\n", hc->job_id);

    ret = send(hc->connfd, &hc->worker_attr.address_length, sizeof(size_t), 0);
    if (-1 == ret) {
        fprintf(stderr, "send worker_address_length failed!\n");
        ret = UCC_ERR_NO_MESSAGE;
        goto err;
    }

    ret = send(hc->connfd, hc->worker_attr.address, hc->worker_attr.address_length, 0);
    if (-1 == ret) {
        fprintf(stderr, "send worker_address failed!\n");
        ret = UCC_ERR_NO_MESSAGE;
        goto err;
    }

    ret = recv(hc->connfd, &hc->rem_worker_addr_len, sizeof(size_t), MSG_WAITALL);
    if (-1 == ret) {
        fprintf(stderr, "recv address_length failed!\n");
        ret = UCC_ERR_NO_MESSAGE;
        goto err;
    }

    hc->rem_worker_addr = calloc(1, hc->rem_worker_addr_len);
    ret = recv(hc->connfd, hc->rem_worker_addr, hc->rem_worker_addr_len, MSG_WAITALL);
    if (-1 == ret) {
        fprintf(stderr, "recv worker address failed!\n");
        ret = UCC_ERR_NO_MESSAGE;
        goto err;
    }

    memset(&hc->pipeline, 0, sizeof(hc->pipeline));
    ret = recv(hc->connfd, &hc->pipeline.buffer_size, sizeof(size_t), MSG_WAITALL);
    if (-1 == ret) {
        fprintf(stderr, "recv pipeline buffer size failed!\n");
        ret = UCC_ERR_NO_MESSAGE;
        goto err;
    }

    ret = recv(hc->connfd, &hc->pipeline.num_buffers, sizeof(size_t), MSG_WAITALL);
    if (-1 == ret) {
        fprintf(stderr, "recv pipeline num buffers failed!\n");
        ret = UCC_ERR_NO_MESSAGE;
        goto err;
    }

    ret = _dpu_hc_init_pipeline(hc);
    if (ret) {
        fprintf(stderr, "init pipeline failed!\n");
        goto err;
    }

    ret = recv(hc->connfd, &hc->world_rank, sizeof(uint32_t), MSG_WAITALL);
    if (-1 == ret) {
        fprintf(stderr, "recv world rank failed!\n");
        ret = UCC_ERR_NO_MESSAGE;
        goto err;
    }

    ret = recv(hc->connfd, &hc->world_size, sizeof(uint32_t), MSG_WAITALL);
    if (-1 == ret) {
        fprintf(stderr, "recv world size failed!\n");
        ret = UCC_ERR_NO_MESSAGE;
        goto err;
    }

    ret = 0;
    goto out;

err:
    close(hc->connfd);
err_ucx:
    _dpu_ucx_fini(hc);
out:
    return ret;
}

int dpu_hc_connect_remote_hosts(dpu_hc_t *hc, dpu_ucc_comm_t *comm)
{
    int ret;

    if (ret = _dpu_create_remote_host_eps(hc, comm)) {
        fprintf(stderr, "_dpu_create_remote_host_eps failed!\n");
        ret = UCC_ERR_NO_MESSAGE;
        goto err;
    }

    ret = _dpu_flush_host_eps(hc);
    if (ret) {
        fprintf(stderr, "ep flush failed!\n");
        goto err;
    }

err:
    return ret;
}

int dpu_hc_wait(dpu_hc_t *hc, uint32_t next_coll_id)
{
    dpu_put_sync_t *lsync = (dpu_put_sync_t*)hc->mem_segs.sync.base;
    ucp_request_param_t req_param = {0};
    ucp_tag_t req_tag = 0, tag_mask = 0;
    ucs_status_t status;

    ucs_status_ptr_t recv_req = ucp_tag_recv_nbx(hc->ucp_worker,
            lsync, sizeof(dpu_put_sync_t),
            req_tag, tag_mask, &req_param);
    status = _dpu_request_wait(hc->ucp_worker, recv_req);

    DPU_LOG("Got next coll id from host: %u was expecting %u\n", lsync->coll_id, next_coll_id);
    assert(lsync->coll_id == next_coll_id);

    host_rkey_t *rkeys = &lsync->rkeys;

    status = ucp_ep_rkey_unpack(hc->localhost_ep, (void*)rkeys->src_rkey_buf, &hc->src_rkey);

    status = ucp_ep_rkey_unpack(hc->localhost_ep, (void*)rkeys->dst_rkey_buf, &hc->dst_rkey);

    return 0;
}

int dpu_hc_reply(dpu_hc_t *hc, dpu_get_sync_t *coll_sync)
{
    ucs_status_t status;
    ucp_tag_t req_tag = 0;

    DPU_LOG("Flushing host ep for coll_id: %d\n", coll_sync->coll_id);
    _dpu_worker_flush(hc);

    assert(hc->pipeline.sync_req == NULL);
    ucp_worker_fence(hc->ucp_worker);
    DPU_LOG("Notify host completed coll_id: %d, serviced: %lu\n",
            coll_sync->coll_id, coll_sync->count_serviced);
    hc->pipeline.sync_req = ucp_tag_send_nbx(hc->localhost_ep,
            coll_sync, sizeof(dpu_get_sync_t), req_tag, &hc->req_param);
    status = _dpu_request_wait(hc->ucp_worker, hc->pipeline.sync_req);
    hc->pipeline.sync_req = NULL;
    if (status != UCS_OK) {
        fprintf(stderr, "failed to notify host of completion (%s)\n", ucs_status_string(status));
        return -1;
    }

    ucp_rkey_destroy(hc->src_rkey);
    ucp_rkey_destroy(hc->dst_rkey);
    _dpu_hc_reset_pipeline(hc);
    return 0;
}

ucc_rank_t dpu_get_world_rank(dpu_hc_t *hc,  int dpu_rank, int team_id, thread_ctx_t *ctx)
{
    ucc_rank_t  world_rank;

    if (team_id == UCC_WORLD_TEAM_ID) {
        world_rank = dpu_rank;
    } else {
        world_rank = ctx->comm.dpu_team_ctx_ranks[team_id][dpu_rank];
    }

    return world_rank;
}

ucc_rank_t dpu_get_host_ep_rank(dpu_hc_t *hc,  int host_rank, int team_id, thread_ctx_t *ctx)
{
    /* find my world_rank of the remote process in dpu comm world then find
     * its ep_rank */
    ucc_rank_t ep_rank, world_rank;

    if (team_id == UCC_WORLD_TEAM_ID) {
        world_rank = host_rank;
    } else {
        world_rank = ctx->comm.host_team_ctx_ranks[team_id][host_rank];
    }

    ep_rank = world_rank * hc->dpu_per_node_cnt;
    return ep_rank;
}

void _dpu_hc_get_remaining(dpu_hc_t *hc, dpu_put_sync_t *sync, size_t *count, size_t *offset)
{
    ucc_datatype_t dtype = sync->coll_args.src.info.datatype;
    size_t dt_size = dpu_ucc_dt_size(dtype);
    size_t remaining_elems = hc->pipeline.my_count - hc->pipeline.count_requested;
    *count = DPU_MIN(hc->pipeline.buffer_size/dt_size, remaining_elems);
    *offset = hc->pipeline.count_requested * dt_size;
}

ucs_status_t dpu_hc_issue_get(dpu_hc_t *hc, dpu_put_sync_t *sync, dpu_buf_t *getbuf)
{
    assert(getbuf->state == READING && getbuf->ucp_req == NULL && getbuf->count > 0);

    ucc_datatype_t dtype = sync->coll_args.src.info.datatype;
    size_t dt_size = dpu_ucc_dt_size(dtype);
    size_t count = getbuf->count;
    size_t get_offset = getbuf->offset;

    size_t data_size = count * dt_size;
    void *src_addr = sync->rkeys.src_buf + get_offset;
    void *dst_addr = getbuf->buf;

    DPU_LOG("Issue Get from offset %lu src %p dst %p count %lu bytes %lu\n",
            get_offset, src_addr, dst_addr, count, data_size);
    
    ucp_worker_fence(hc->ucp_worker);
    getbuf->ucp_req =
            ucp_get_nbx(hc->localhost_ep, dst_addr, data_size,
            (uint64_t)src_addr, hc->src_rkey, &hc->req_param);
    
    return UCS_OK;
}

ucs_status_t dpu_hc_issue_put(dpu_hc_t *hc, dpu_put_sync_t *sync, dpu_buf_t *putbuf)
{
    assert(putbuf->state == WRITING && putbuf->ucp_req == NULL);
    ucc_datatype_t dtype = sync->coll_args.src.info.datatype;
    size_t dt_size = dpu_ucc_dt_size(dtype);
    size_t count = putbuf->count;
    size_t put_offset = putbuf->offset;

    size_t data_size = count * dt_size;
    void *src_addr = putbuf->buf;
    void *dst_addr = sync->rkeys.dst_buf + put_offset;

    DPU_LOG("Issue Put to offset %lu src %p dst %p count %lu bytes %lu\n",
            put_offset, src_addr, dst_addr, count, data_size);
    assert(count > 0 && dt_size > 0);

    // int32_t *pbuf = putbuf->buf;
    // DPU_LOG("## PUT DATA %ld %ld\n", pbuf[0], pbuf[1]);
    
    ucp_worker_fence(hc->ucp_worker);
    putbuf->ucp_req =
            ucp_put_nbx(hc->localhost_ep, src_addr, data_size,
            (uint64_t)dst_addr, hc->dst_rkey, &hc->req_param);

    return UCS_OK;
}

ucs_status_t dpu_hc_issue_allreduce(dpu_hc_t *hc, dpu_put_sync_t *sync, thread_ctx_t *ctx, dpu_buf_t *getbuf)
{
    assert(getbuf->state == REDUCING && getbuf->ucp_req == NULL && getbuf->ucc_req == NULL);
    uint64_t team_rank;
    uint32_t team_size;
    ucc_team_h team = ctx->comm.team_pool[sync->team_id];
    ucc_datatype_t dtype = sync->coll_args.src.info.datatype;

    UCC_CHECK(ucc_team_get_size(team, &team_size));
    UCC_CHECK(ucc_team_get_my_ep(team, &team_rank));

    DPU_LOG("Calling sharp allreduce on team id %d rank %d size %d count %lu offset %lu\n",
            sync->team_id, team_rank, team_size, getbuf->count, getbuf->offset);
    ucc_coll_args_t coll = {
        .op = sync->coll_args.op,
        .coll_type = UCC_COLL_TYPE_ALLREDUCE,
        .src.info = {
            .buffer   = getbuf->buf,
            .count    = getbuf->count,
            .datatype = dtype,
            .mem_type = UCC_MEMORY_TYPE_HOST,
        },
        .dst.info = {
            .buffer   = getbuf->buf,
            .count    = getbuf->count,
            .datatype = dtype,
            .mem_type = UCC_MEMORY_TYPE_HOST,
        },
    };

    UCC_CHECK(ucc_collective_init(&coll, &getbuf->ucc_req, team));
    UCC_CHECK(ucc_collective_post(getbuf->ucc_req));

    return UCS_OK;
}

ucs_status_t dpu_hc_issue_hangup(dpu_hc_t *hc, dpu_put_sync_t *sync, thread_ctx_t *ctx)
{
    thread_sub_sync->accbuf = NULL;
    thread_sub_sync->getbuf = NULL;
    dpu_signal_comp_thread(ctx, thread_sub_sync);
    return UCS_OK;
}

ucc_status_t dpu_check_comp_status(dpu_buf_t *redbuf, thread_ctx_t *ctx)
{
    ucc_status_t status;
    assert(redbuf->state == REDUCING && redbuf->ucc_req != NULL);
    
    // ucc_context_progress(ctx->comm.ctx);
    
    status = ucc_collective_test(redbuf->ucc_req);

    if (status != UCC_OK && status != UCC_INPROGRESS) {
        fprintf (stderr, "unable to complete UCC request (%s)\n",
                ucs_status_string(status));
    }

    return status;
}

ucs_status_t dpu_hc_progress_allreduce(dpu_hc_t *hc,
                    dpu_put_sync_t *sync,
                    thread_ctx_t *ctx)
{
    ucc_status_t status;
    ucs_status_ptr_t request;
    dpu_pipeline_t *pp = &hc->pipeline;
    int nbufs = pp->num_buffers;

    ucp_worker_progress(hc->ucp_worker);
    ucc_context_progress(ctx->comm.ctx);
    
    for (size_t i=0; i < nbufs; i++) {
        dpu_buf_t *buf = &pp->buffers[i];
        // dpu_buf_t *buf2 = &pp->buffers[(i+nbufs-1)%nbufs];

        switch(buf->state) {
            case FREE:
                _dpu_hc_get_remaining(hc, sync, &buf->count, &buf->offset);
                if (buf->count > 0) {
                    DPU_LOG("Issue get for %ld bytes into buf %d offset %lu\n",
                            buf->count, i, buf->offset);
                    buf->state = READING;
                    pp->count_requested += buf->count;
                    dpu_hc_issue_get(hc, sync, buf);
                }
                break;
            case READING:
                request = buf->ucp_req;
                if (_dpu_req_test(request) == UCS_OK) {
                    if (request) ucp_request_free(request);
                    buf->ucp_req = NULL;
                    buf->state = READY;
                    DPU_LOG("Received %ld bytes into buf %d offset %lu\n",
                            buf->count, i, buf->offset);
                }
                break;
            case READY:
                buf->state = REDUCING;
                dpu_hc_issue_allreduce(hc, sync, ctx, buf);
                break;
            case REDUCING:
                if (dpu_check_comp_status(buf, ctx) == UCC_OK) {
                    UCC_CHECK(ucc_collective_finalize(buf->ucc_req));
                    buf->ucc_req = NULL;
                    buf->state = REDUCED;
                    DPU_LOG("Reduced %ld bytes from buf %d offset %lu\n",
                            buf->count, i, buf->offset);
                }
                break;
            case REDUCED:
                buf->state = WRITING;
                dpu_hc_issue_put(hc, sync, buf);
                break;
            case WRITING:
                request = buf->ucp_req;
                if (_dpu_req_test(request) == UCS_OK) {
                    if (request) ucp_request_free(request);
                    buf->ucp_req = NULL;
                    buf->state = DONE;
                    DPU_LOG("Sent %ld bytes from buf %d offset %lu\n",
                            buf->count, i, buf->offset);
                }
                break;
            case DONE:
                pp->count_serviced += buf->count;
                buf->state = FREE;
                break;
            default:
                break;
        }

    }
}

ucs_status_t dpu_send_init_completion(dpu_hc_t *hc)
{
    ucs_status_t status;
    ucp_tag_t req_tag = 0;
    ucs_status_ptr_t request;

    dpu_get_sync_t coll_sync;
    coll_sync.coll_id = -1;
    coll_sync.count_serviced = -1;

    printf("# Accepted Job Id %d with rank %d size %d\n",
           hc->job_id, hc->world_rank, hc->world_size);
    _dpu_worker_flush(hc);

    ucp_worker_fence(hc->ucp_worker);
    request = ucp_tag_send_nbx(hc->localhost_ep,
            &coll_sync, sizeof(dpu_get_sync_t), req_tag, &hc->req_param);
    status = _dpu_request_wait(hc->ucp_worker, request);
    if (status != UCS_OK) {
        fprintf(stderr, "failed to notify host of init completion (%s)\n", ucs_status_string(status));
        return status;
    }

    return UCS_OK;
}

int dpu_hc_reset_job(dpu_hc_t *hc)
{
    _dpu_flush_host_eps(hc);
    _dpu_worker_flush(hc);
    _dpu_hc_buffer_free(hc, &hc->mem_segs.in);
    _dpu_hc_buffer_free(hc, &hc->mem_segs.out);
    _dpu_hc_buffer_free(hc, &hc->mem_segs.sync);
    _dpu_close_host_eps(hc);
    _dpu_ucx_fini(hc);
    printf("# Completed Job Id %d\n", hc->job_id);
    return UCC_OK;
}

/* TODO: call from atexit() */
int dpu_hc_finalize(dpu_hc_t *hc)
{
    printf("Finalizing DPU Server, Job Id %d\n", hc->job_id);
    _dpu_listen_cleanup(hc);
    return UCC_OK;
}
