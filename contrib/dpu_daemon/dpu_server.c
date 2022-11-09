/*
 * Copyright (C) Mellanox Technologies Ltd. 2020.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>

#include "server_ucc.h"
#include "host_channel.h"
#include "dpu_log.h"
#include "ucc/api/ucc.h"

#define NUM_CORES 8

dpu_ucc_global_t ucc_glob;
dpu_hc_t         hc;
dpu_get_sync_t   coll_sync = {0};
dpu_put_sync_t   tmp_sync = {0};
dpu_thread_sync_t thread_sync = {0};

pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;

pthread_barrier_t sync_barrier;
pthread_barrierattr_t barrier_attr;

size_t dpu_coll_counter[UCC_COLL_TYPE_LAST] = {0};

static void dpu_coll_print_summary(void)
{
    int print_summary = 0;
    char *s = getenv("UCC_TL_DPU_PRINT_SUMMARY");
    if (s) { print_summary = !!atoi(s); }
    if (!print_summary) return;

    printf("# Summary ");
    /* TODO: find a more robust way to iterate */
    for (ucc_coll_type_t coll=1; coll<UCC_COLL_TYPE_LAST; coll <<= 1) {
        size_t count = dpu_coll_counter[coll];
        if (count > 0) {
            printf(" %s %zu ", ucc_coll_type_str(coll), count);
        }
        dpu_coll_counter[coll] = 0; /* Reset Counter */
    }
    printf("\n");
}

/* TODO: include ucc_coll_utils.h */
static inline size_t
ucc_coll_args_get_count(const ucc_coll_args_t *args, const ucc_count_t *counts,
                        ucc_rank_t idx)
{
    if ((args->mask & UCC_COLL_ARGS_FIELD_FLAGS) &&
        (args->flags & UCC_COLL_ARGS_FLAG_COUNT_64BIT)) {
        return ((uint64_t *)counts)[idx];
    }
    return ((uint32_t *)counts)[idx];
}

static inline size_t
ucc_coll_args_get_displacement(const ucc_coll_args_t *args,
                               const ucc_aint_t *displacements, ucc_rank_t idx)
{
    if ((args->mask & UCC_COLL_ARGS_FIELD_FLAGS) &&
        (args->flags & UCC_COLL_ARGS_FLAG_DISPLACEMENTS_64BIT)) {
        return ((uint64_t *)displacements)[idx];
    }
    return ((uint32_t *)displacements)[idx];
}

static inline size_t
ucc_coll_args_get_total_count(const ucc_coll_args_t *args,
                              const ucc_count_t *counts, ucc_rank_t size)
{
    size_t count = 0;
    ucc_rank_t i;
    // TODO switch to base args and cache total count there - can we do it ?
    if ((args->mask & UCC_COLL_ARGS_FIELD_FLAGS) &&
        (args->flags & UCC_COLL_ARGS_FLAG_COUNT_64BIT)) {
        for (i = 0; i < size; i++) {
            count += ((uint64_t *)counts)[i];
        }
    } else {
        for (i = 0; i < size; i++) {
            count += ((uint32_t *)counts)[i];
        }
    }

    return count;
}

ucc_ep_map_t ucc_ep_map_from_array(ucc_rank_t **array, ucc_rank_t size,
                                   ucc_rank_t full_size, int need_free);

void signal_workers(thread_ctx_t *ctx)
{
    for (int i = 1; i < ctx->nth; i++) {
        ctx->thread_sync->done[i] = 0;
        ctx->thread_sync->todo[i] = 1;
    }
}

void waitfor_workers(thread_ctx_t *ctx)
{
    int done;
    do {
        done = 0;
        for (int i = 1; i < ctx->nth; i++) {
            if (ctx->thread_sync->done[i]) {
                done++;
            }
        }
    } while (done < ctx->nth - 1);
}

void waitfor_master(thread_ctx_t *ctx)
{
    int i = ctx->idx;
    while (!ctx->thread_sync->todo[i]);
    ctx->thread_sync->todo[i] = 0;
}

void signal_master(thread_ctx_t *ctx)
{
    int i = ctx->idx;
    ctx->thread_sync->done[i] = 1;
}

void thread_barrier(thread_ctx_t *ctx)
{
    // pthread_barrier_wait(&sync_barrier);
    if (!ctx->idx) {
        signal_workers(ctx);
        waitfor_workers(ctx);
    } else {
        waitfor_master(ctx);
        signal_master(ctx);
    }
}

static void dpu_thread_set_affinity(thread_ctx_t *ctx)
{
    int coreid = ctx->idx;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    if (coreid >=0 && coreid < NUM_CORES) {
        CPU_SET(coreid, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
    }
}

static ucc_status_t dpu_coll_do_blocking_alltoall(thread_ctx_t *ctx, dpu_put_sync_t *lsync)
{
    ucs_status_t status;
    size_t team_rank, team_size;
    dpu_hc_t *hc = ctx->hc;
    ucc_team_h team = ctx->comm->team_pool[lsync->team_id];
    UCC_CHECK(ucc_team_get_size(team, (uint32_t*)&team_size));
    UCC_CHECK(ucc_team_get_my_ep(team, (uint64_t*)&team_rank));

    size_t count_total   = lsync->count_total;
    size_t my_count      = count_total / team_size;
    ucc_datatype_t dtype = lsync->coll_args.src.info.datatype;
    size_t dt_size       = dpu_ucc_dt_size(dtype);

    CTX_LOG("Doing alltoall on team id %u team size %lu count %lu\n", lsync->team_id, team_size, count_total);

    for(int i = 0; i < team_size; i++) {
        int src_rank = (team_rank + i) % team_size;
        size_t src_offset = team_rank * my_count * dt_size;
        size_t dst_offset = src_rank * my_count * dt_size;
        size_t count_done = 0;

        while (count_done < my_count) {
            ucs_status_ptr_t ucp_req = NULL;
            size_t remaining_elems = my_count - count_done;
            size_t count_step = DPU_MIN(hc->pipeline.buffer_size/dt_size, remaining_elems);
            size_t bytes_step = count_step * dt_size;

            void * src_addr  = hc->host_rkeys[src_rank].src_buf + src_offset;
            void * tmp_addr  = hc->pipeline.stages[0].accbuf.buf;
            void * dst_addr  = lsync->rkeys.dst_buf + dst_offset;

            DPU_LOG("Issue Get from %d src offset %zu count %zu bytes %zu\n",
                    src_rank, src_offset, my_count, bytes_step);
            ucp_worker_fence(hc->ucp_worker);
            ucp_req = ucp_get_nbx(
                hc->host_eps[src_rank], tmp_addr, bytes_step, (uint64_t)src_addr,
                hc->host_src_rkeys[src_rank], &hc->req_param);
            status = _dpu_request_wait(hc->ucp_worker, ucp_req);
            if (status != UCS_OK) {
                return UCC_ERR_NO_RESOURCE;
            }

            DPU_LOG("Issue Put to localhost dst offset %zu count %zu bytes %zu\n",
                    dst_offset, my_count, bytes_step);
            ucp_worker_fence(hc->ucp_worker);
            ucp_req = ucp_put_nbx(
                hc->localhost_ep, tmp_addr, bytes_step, (uint64_t)dst_addr,
                hc->dst_rkey, &hc->req_param);
            status = _dpu_request_wait(hc->ucp_worker, ucp_req);
            if (status != UCS_OK) {
                return UCC_ERR_NO_RESOURCE;
            }

            count_done += count_step;
            src_offset += bytes_step;
            dst_offset += bytes_step;
        }
    }

    return UCC_OK;
}

static ucc_status_t dpu_coll_do_blocking_alltoallv(thread_ctx_t *ctx, dpu_put_sync_t *lsync)
{
    ucs_status_t status;
    ucc_rank_t team_rank, team_size;
    dpu_hc_t *hc = ctx->hc;
    ucc_coll_args_t *args = &lsync->coll_args;
    ucc_team_h team = ctx->comm->team_pool[lsync->team_id];
    UCC_CHECK(ucc_team_get_size(team, (uint32_t*)&team_size));
    UCC_CHECK(ucc_team_get_my_ep(team, (uint64_t*)&team_rank));

    CTX_LOG("Doing alltoallv on team id %u team size %u\n", lsync->team_id, team_size);

    for(int i = 0; i < team_size; i++) {
        int src_rank = (team_rank + i) % team_size;
        
        dpu_put_sync_t *src_lsync = &hc->world_lsyncs[src_rank];
        size_t src_count = ucc_coll_args_get_count(args, src_lsync->src_v.counts, team_rank);
        size_t src_displ = ucc_coll_args_get_displacement(args, src_lsync->src_v.displs, team_rank);

        size_t dst_count = ucc_coll_args_get_count(args, lsync->dst_v.counts, src_rank);
        size_t dst_displ = ucc_coll_args_get_displacement(args, lsync->dst_v.displs, src_rank);

        ucc_datatype_t sdt   = src_lsync->coll_args.src.info_v.datatype;
        ucc_datatype_t rdt   = lsync->coll_args.dst.info_v.datatype;
        size_t sdt_size      = dpu_ucc_dt_size(sdt);
        size_t rdt_size      = dpu_ucc_dt_size(rdt);

        CTX_LOG("src rank %d count %zu displ %zu dtsize %zu dst rank %d count %zu displ %zu dtsize %zu\n",
                src_rank,  src_count, src_displ, sdt_size,
                team_rank, dst_count, dst_displ, rdt_size);

        assert(src_count * sdt_size == dst_count * rdt_size);

        size_t src_offset = src_displ * sdt_size;
        size_t dst_offset = dst_displ * rdt_size;

        size_t count_done = 0;
        while (count_done < src_count) {
            ucs_status_ptr_t ucp_req = NULL;
            size_t remaining_elems = src_count - count_done;
            size_t count_step = DPU_MIN(hc->pipeline.buffer_size/sdt_size, remaining_elems);
            size_t bytes_step = count_step * sdt_size;

            DPU_LOG("Element count %zu done %zu remaining %zu this step %zu\n",
                    src_count, count_done, remaining_elems, count_step);

            void * src_addr  = hc->host_rkeys[src_rank].src_buf + src_offset;
            void * tmp_addr  = hc->pipeline.stages[0].accbuf.buf;
            void * dst_addr  = lsync->rkeys.dst_buf + dst_offset;

            DPU_LOG("Issue Get from %d src offset %zu count %zu bytes %zu\n",
                    src_rank, src_offset, src_count, bytes_step);
            ucp_worker_fence(hc->ucp_worker);
            ucp_req = ucp_get_nbx(
                hc->host_eps[src_rank], tmp_addr, bytes_step, (uint64_t)src_addr,
                hc->host_src_rkeys[src_rank], &hc->req_param);
            status = _dpu_request_wait(hc->ucp_worker, ucp_req);
            if (status != UCS_OK) {
                return UCC_ERR_NO_RESOURCE;
            }

            DPU_LOG("Issue Put to localhost dst offset %zu count %zu bytes %zu\n",
                    dst_offset, dst_count, bytes_step);
            ucp_worker_fence(hc->ucp_worker);
            ucp_req = ucp_put_nbx(
                hc->localhost_ep, tmp_addr, bytes_step, (uint64_t)dst_addr,
                hc->dst_rkey, &hc->req_param);
            status = _dpu_request_wait(hc->ucp_worker, ucp_req);
            if (status != UCS_OK) {
                return UCC_ERR_NO_RESOURCE;
            }

            count_done += count_step;
            src_offset += bytes_step;
            dst_offset += bytes_step;
        }
    }

    return UCC_OK;
}

static void dpu_coll_collect_host_rkeys(thread_ctx_t *ctx, dpu_hc_t *hc, dpu_put_sync_t *lsync)
{
    CTX_LOG("Collecting Host rkeys on team id %u\n", lsync->team_id);

    int i, ep_rank;
    ucs_status_t status;
    ucc_coll_req_h request;
    ucc_team_h team = ctx->comm->team_pool[lsync->team_id];
    ucc_rank_t team_size = 0;
    UCC_CHECK(ucc_team_get_size(team, &team_size));
    void *src_buf = lsync;
    void *dst_buf = hc->world_lsyncs;

    assert(NULL != lsync->rkeys.src_rkey_buf);
    assert(NULL != lsync->rkeys.dst_rkey_buf);
    assert(0    <  lsync->rkeys.src_rkey_len);
    assert(0    <  lsync->rkeys.dst_rkey_len);
    assert(NULL != lsync->rkeys.src_buf);
    assert(NULL != lsync->rkeys.dst_buf);
        
    ucc_coll_args_t coll = {
        .coll_type = UCC_COLL_TYPE_ALLGATHER,
        .src.info = {
            .buffer   = src_buf,
            .count    = sizeof(dpu_put_sync_t),
            .datatype = UCC_DT_INT8,
            .mem_type = UCC_MEMORY_TYPE_HOST,
        },
        .dst.info = {
            .buffer   = dst_buf,
            .count    = sizeof(dpu_put_sync_t) * team_size,
            .datatype = UCC_DT_INT8,
            .mem_type = UCC_MEMORY_TYPE_HOST,
        },
    };

    CTX_LOG("Issue Allgather from ranks %d src %p dst %p bytes %zu\n",
            team_size, src_buf, dst_buf, sizeof(host_rkey_t));
    UCC_CHECK(ucc_collective_init(&coll, &request, team));
    UCC_CHECK(ucc_collective_post(request));
    while (UCC_OK != ucc_collective_test(request)) {
        ucc_context_progress(ctx->comm->ctx);
    }
    UCC_CHECK(ucc_collective_finalize(request));

    /*memset(hc->host_rkeys, 0, sizeof(host_rkey_t) * hc->world_size);

    for (i = 0; i < team_size; i++) {
        ep_rank  = dpu_get_world_rank(hc, i, lsync->team_id, ctx);
        memcpy(&hc->host_rkeys[ep_rank], &hc->world_lsyncs[i].rkeys, sizeof(host_rkey_t));
        assert(NULL != hc->host_rkeys[ep_rank].src_rkey_buf);
        assert(NULL != hc->host_rkeys[ep_rank].dst_rkey_buf);
        assert(0    <  hc->host_rkeys[ep_rank].src_rkey_len);
        assert(0    <  hc->host_rkeys[ep_rank].dst_rkey_len);
        status = ucp_ep_rkey_unpack(hc->host_eps[ep_rank], (void*)hc->host_rkeys[ep_rank].src_rkey_buf, &hc->host_src_rkeys[ep_rank]);
        assert(UCS_OK == status);
        assert(NULL != hc->host_rkeys[ep_rank].src_buf);
        status = ucp_ep_rkey_unpack(hc->host_eps[ep_rank], (void*)hc->host_rkeys[ep_rank].dst_rkey_buf, &hc->host_dst_rkeys[ep_rank]);
        assert(UCS_OK == status);
        assert(NULL != hc->host_rkeys[ep_rank].dst_buf);
        CTX_LOG("Rank %d with EP Rank %d  team_id  %d src buf %p dst buf %p\n", 
                i, ep_rank, lsync->team_id, hc->host_rkeys[ep_rank].src_buf, hc->host_rkeys[ep_rank].dst_buf);
    }*/

    hc->rail = lsync->rail;
    hc->dpu_per_node_cnt = lsync->dpu_per_node_cnt;
    assert(hc->dpu_per_node_cnt > 0 && hc->rail >= 0 && hc->rail < hc->dpu_per_node_cnt);
}

static void dpu_import_dc_rkeys(thread_ctx_t *ctx, dpu_hc_t *_hc, dpu_hc_t *dc, dpu_put_sync_t *lsync)
{
    int i, ep_rank;
    ucs_status_t status;
    ucc_coll_req_h request;
    ucc_team_h team = ctx->comm->team_pool[lsync->team_id];
    ucc_rank_t team_size = 0;
    UCC_CHECK(ucc_team_get_size(team, &team_size));

    dc->world_lsyncs = _hc->world_lsyncs;
    memset(dc->host_rkeys, 0, sizeof(host_rkey_t) * dc->world_size);

    for (i = 0; i < team_size; i++) {
        ep_rank  = dpu_get_world_rank(dc, i, lsync->team_id, ctx);
        memcpy(&dc->host_rkeys[ep_rank], &dc->world_lsyncs[i].rkeys, sizeof(host_rkey_t));
        status = ucp_ep_rkey_unpack(dc->host_eps[ep_rank], (void*)dc->host_rkeys[ep_rank].src_rkey_buf, &dc->host_src_rkeys[ep_rank]);
        assert(UCS_OK == status);
        assert(NULL != dc->host_rkeys[ep_rank].src_buf);
        status = ucp_ep_rkey_unpack(dc->host_eps[ep_rank], (void*)dc->host_rkeys[ep_rank].dst_rkey_buf, &dc->host_dst_rkeys[ep_rank]);
        assert(UCS_OK == status);
        assert(NULL != dc->host_rkeys[ep_rank].dst_buf);
        CTX_LOG("Rank %d with EP Rank %d  team_id  %d src buf %p dst buf %p\n", 
                i, ep_rank, lsync->team_id, dc->host_rkeys[ep_rank].src_buf, dc->host_rkeys[ep_rank].dst_buf);
    }

    dc->rail = lsync->rail;
    dc->dpu_per_node_cnt = lsync->dpu_per_node_cnt;
    assert(dc->dpu_per_node_cnt > 0 && dc->rail >= 0 && dc->rail < dc->dpu_per_node_cnt);
}

void dpu_team_barrier(ucc_context_h ucc_ctx, ucc_team_h team)
{
    ucs_status_t status;
    ucc_coll_req_h request;
    ucc_coll_args_t coll = {
        .mask = 0,
        .coll_type = UCC_COLL_TYPE_BARRIER,
    };

    UCC_CHECK(ucc_collective_init(&coll, &request, team));
    UCC_CHECK(ucc_collective_post(request));
    while (UCC_OK != ucc_collective_test(request)) {
        ucc_context_progress(ucc_ctx);
    }
    UCC_CHECK(ucc_collective_finalize(request));
}

void dpu_coll_do_barrier(thread_ctx_t *ctx, dpu_put_sync_t *lsync)
{
    ucc_team_h team = ctx->comm->team_pool[lsync->team_id];
    CTX_LOG("Issue Synchronizing Barrier on team %d\n", lsync->team_id);
    dpu_team_barrier(ctx->comm->ctx, team);
}

void dpu_coll_world_barrier(dpu_ucc_comm_t *comm)
{
    dpu_team_barrier(comm->ctx, comm->team);
}

static void dpu_coll_free_host_rkeys(thread_ctx_t *ctx, dpu_hc_t *hc, dpu_put_sync_t *lsync)
{
    int i;
    unsigned int team_size = 0;
    ucc_team_h team = ctx->comm->team_pool[lsync->team_id];
    UCC_CHECK(ucc_team_get_size(team, &team_size));
    CTX_LOG("Freeing src/dst rkeys for %u hosts\n", team_size);
    for (i = 0; i < team_size; i++) {
        if (hc->host_src_rkeys[i] != NULL)
            ucp_rkey_destroy(hc->host_src_rkeys[i]);
        if (hc->host_dst_rkeys[i] != NULL)
            ucp_rkey_destroy(hc->host_dst_rkeys[i]);
    }
}

void dpu_wait_for_next_coll(thread_ctx_t *ctx)
{
    CTX_LOG("Waiting for host to initiate coll id: %u\n", ctx->coll_sync->coll_id);
    dpu_hc_wait(ctx->hc, ctx->coll_sync->coll_id);
    
    memcpy(&tmp_sync, (dpu_put_sync_t*)ctx->hc->mem_segs.sync.base, sizeof(tmp_sync));
    __sync_synchronize();
}

void dpu_mark_coll_done(thread_ctx_t *ctx, dpu_put_sync_t *lsync)
{
    assert(ctx->coll_sync->coll_id == lsync->coll_id);
    ctx->coll_sync->count_serviced = lsync->count_total;
    dpu_hc_reply(ctx->hc, ctx->coll_sync);
}

static ucc_status_t dpu_create_comm_team(thread_ctx_t *ctx, dpu_put_sync_t *lsync)
{
    /* read in the rank list in comm world */
    int i = 0, idx = 0, rail = 0;
    uint16_t team_id = lsync->team_id;
    ucc_rank_t full_size = ctx->hc->world_size;
    ucc_team_h new_team = NULL;
    ucc_team_params_t team_params = {0};
    uint16_t dpu_per_node_cnt = lsync->dpu_per_node_cnt;
    ucc_rank_t host_team_size = lsync->num_ranks;
    ucc_rank_t dpu_team_size = host_team_size * dpu_per_node_cnt;
    ucc_status_t status;
    
    CTX_LOG("creating new team with team_id %u coll_id %d\n",
            team_id, lsync->coll_id);

    ucc_rank_t *dpu_rank_list = malloc(sizeof(ucc_rank_t) * dpu_team_size);
    ucc_rank_t *host_rank_list = malloc(sizeof(ucc_rank_t) * host_team_size);

    for (i = 0; i < host_team_size; i++) {
        for (rail = 0; rail < dpu_per_node_cnt; rail++) {
            dpu_rank_list[idx++] = (lsync->rank_list[i] * dpu_per_node_cnt) + rail;
        }
    }

    memcpy(host_rank_list, lsync->rank_list, sizeof(ucc_rank_t) * host_team_size);

    CTX_LOG("got the rank list from host, new dpu team size: %d and host team size: %d\n",
            dpu_team_size, host_team_size);

    /* now we have the rank list in comm world available  */
    team_params.ep_range = UCC_COLLECTIVE_EP_RANGE_CONTIG;
    team_params.mask     = UCC_TEAM_PARAM_FIELD_EP |
                           UCC_TEAM_PARAM_FIELD_EP_RANGE |
                           UCC_TEAM_PARAM_FIELD_EP_MAP;

    /* find my new rank in the new team */
    for(i = 0; i < dpu_team_size; i++) {
        if (dpu_rank_list[i] == ctx->hc->world_rank) {
            break;
        }
    }
    team_params.ep = i; 
    team_params.ep_map = ucc_ep_map_from_array(&dpu_rank_list, dpu_team_size, full_size, 0);

    status = ucc_team_create_post(&ctx->comm->ctx, 1, &team_params, &new_team);
    if (UCC_OK != status) {
        fprintf(stderr, "ucc_team_create_post failed with %d\n", status);
        goto err;
    }

    do {
        status = ucc_team_create_test(new_team);
        ucc_context_progress(ctx->comm->ctx);
    } while (UCC_INPROGRESS == status);
        
    if (UCC_OK != status) {
        fprintf(stderr, "ucc_team_create_test failed with %d\n", status);
        goto err;
    }

    /* a new team has been created, insert it into the thread context */
    assert(new_team != NULL);
    ctx->comm->team_pool[team_id] = new_team; 
    ctx->comm->dpu_team_ctx_ranks[team_id] = dpu_rank_list;
    ctx->comm->host_team_ctx_ranks[team_id] = host_rank_list;
    CTX_LOG("created new team with team_id %u size %d\n",
            team_id, dpu_team_size);
    return UCC_OK;

err:
    free(dpu_rank_list);
    free(host_rank_list);
    return status;
}

static ucc_status_t dpu_destroy_comm_team(thread_ctx_t *ctx, dpu_put_sync_t *lsync)
{
    uint16_t team_id = lsync->team_id;
    ucc_team_h team = ctx->comm->team_pool[team_id]; 
    ucc_status_t status;

    CTX_LOG("destroying team with team_id %d coll_id %d\n",
            team_id, lsync->coll_id);

    assert(team != NULL);
    status = ucc_team_destroy(team);
    if (status < 0) {
        fprintf(stderr, "ucc_team_destroy failed with %d\n", status);
        return status;
    }

    ctx->comm->team_pool[team_id] = NULL; 
    if (ctx->comm->dpu_team_ctx_ranks[team_id] != NULL) {
        free(ctx->comm->dpu_team_ctx_ranks[team_id]);
        ctx->comm->dpu_team_ctx_ranks[team_id] = NULL;
    }
    if (ctx->comm->host_team_ctx_ranks[team_id] != NULL) {
        free(ctx->comm->host_team_ctx_ranks[team_id]);
        ctx->comm->host_team_ctx_ranks[team_id] = NULL;
    }

    CTX_LOG("destroyed team with team_id %d\n", team_id);
    return status;
}

void *dpu_comm_thread(void *arg)
{
    thread_ctx_t    *ctx = (thread_ctx_t *)arg;
    dpu_hc_t        *hc = ctx->hc;
    dpu_hc_t        *dc = ctx->dc;
    uint32_t        coll_id, dpu_team_size;
    size_t          dpu_team_rank;
    ucc_coll_type_t coll_type; 
    size_t          count_total; 
    uint16_t        team_id; 
    uint16_t        create_team;
    uint16_t        rail; 
    uint16_t        dpu_per_node_cnt;

    dpu_put_sync_t  *lsync = &tmp_sync; //comm_thread_ctx->hc->mem_segs.sync.base;
    ucc_status_t    status;

    dpu_thread_set_affinity(ctx);
    CTX_LOG("Started comm thread %d\n", ctx->idx);
    pthread_barrier_wait(&sync_barrier);

    while (1) {
        if (ctx->idx == 0) {
            ctx->coll_sync->coll_id++;
            ctx->coll_sync->count_serviced = 0;
            CTX_LOG("Waiting for coll id: %u from host\n", ctx->coll_sync->coll_id);
            dpu_wait_for_next_coll(ctx);
        }
        thread_barrier(ctx);

        coll_id     = lsync->coll_id;
        coll_type   = lsync->coll_args.coll_type;
        count_total = lsync->count_total;
        team_id     = lsync->team_id;
        create_team = lsync->create_new_team;
        rail        = lsync->rail;
        dpu_per_node_cnt = lsync->dpu_per_node_cnt;

        assert(0 <= team_id && team_id < DPU_TEAM_POOL_SIZE);
        if (ctx->idx == 0) {
            dpu_coll_counter[coll_type]++;
        }

        CTX_LOG(
            "Start coll id: %u, type: %d, count total: %lu on team: %u "
            "rail: %d, dpu count: %d, create: %d\n",
            coll_id, coll_type, count_total, team_id, rail, dpu_per_node_cnt, create_team);


        if (coll_type == UCC_COLL_TYPE_LAST) {
            if (ctx->idx > 0) {
                goto end;
            }
            if (create_team == 1) {

                dpu_create_comm_team(ctx, lsync);
                continue;

            } else if (team_id == UCC_WORLD_TEAM_ID) {

                /* World team free so Hang up */
                /* Don't send a response back to Host */
                ucp_rkey_destroy(hc->src_rkey);
                ucp_rkey_destroy(hc->dst_rkey);
                break;

            } else {

                /* releasing a subcomm's team that was already created
                 * on the dpu world */

                dpu_destroy_comm_team(ctx, lsync);
                continue;
            }
        }

        else if (coll_type == UCC_COLL_TYPE_ALLREDUCE) {
            ucc_team_h team = ctx->comm->team_pool[lsync->team_id];
            assert(team != NULL);
            UCC_CHECK(ucc_team_get_size(team, &dpu_team_size));
            UCC_CHECK(ucc_team_get_my_ep(team, &dpu_team_rank));

            if (ctx->idx == 0) {
                dpu_coll_collect_host_rkeys(ctx, hc, lsync);
            }
            thread_barrier(ctx);
            dpu_import_dc_rkeys(ctx, hc, dc, lsync);
 
            ucc_datatype_t dtype = lsync->coll_args.src.info.datatype;
            size_t dt_size = dpu_ucc_dt_size(dtype);
            dc->pipeline.my_count  = lsync->count_total / dpu_team_size;
            dc->pipeline.my_offset = dc->pipeline.my_count * dt_size * dpu_team_rank;
            if (dpu_team_rank == dpu_team_size - 1) {
                dc->pipeline.my_count += lsync->count_total % dpu_team_size;
            }

            /* Adjust count and offset for thread id */
            dc->pipeline.my_count /= ctx->nth;
            dc->pipeline.my_offset += dc->pipeline.my_count * dt_size * ctx->idx;
            CTX_LOG("count total %u my count %zu offset %zu\n",
                    lsync->count_total, dc->pipeline.my_count, dc->pipeline.my_offset);

            while (dc->pipeline.count_serviced < dc->pipeline.my_count) {
                dpu_hc_progress_allreduce(dc, lsync, ctx);
            }

            CTX_LOG("count total %u my count %zu offset %zu serviced %zu\n",
                    lsync->count_total, dc->pipeline.my_count,
                    dc->pipeline.my_offset, dc->pipeline.count_serviced);

            thread_barrier(ctx);
            if (ctx->idx == 0) {
                CTX_LOG("Waiting for all ranks to complete coll id: %u, type: %d\n",
                    coll_id, coll_type);
                dpu_coll_do_barrier(ctx, lsync);
                dpu_mark_coll_done(ctx, lsync);
                dpu_coll_free_host_rkeys(ctx, hc, lsync);
            }
            dpu_coll_free_host_rkeys(ctx, dc, lsync);
            dpu_hc_reset_pipeline(dc);

            CTX_LOG("End coll id: %u, type: %d, count total: %lu, count serviced: %zu\n",
                    coll_id, coll_type, count_total, (size_t)ctx->coll_sync->count_serviced);
        }

        else if (coll_type == UCC_COLL_TYPE_ALLTOALL) {
            dpu_coll_collect_host_rkeys(ctx, dc, lsync);
            
            dpu_coll_do_blocking_alltoall(ctx, lsync);

            CTX_LOG("Waiting for all ranks to complete coll id: %u, type: %d\n",
                    coll_id, coll_type);
            dpu_coll_do_barrier(ctx, lsync);

            dpu_mark_coll_done(ctx, lsync);
            CTX_LOG("End coll id: %u, type: %d, count total: %lu, count serviced: %zu\n",
                    coll_id, coll_type, count_total, (size_t)ctx->coll_sync->count_serviced);

            dpu_coll_free_host_rkeys(ctx, dc, lsync);
        }

        else if (coll_type == UCC_COLL_TYPE_ALLTOALLV) {
            dpu_coll_collect_host_rkeys(ctx, dc, lsync);
            
            dpu_coll_do_blocking_alltoallv(ctx, lsync);

            CTX_LOG("Waiting for all ranks to complete coll id: %u, type: %d\n",
                    coll_id, coll_type);
            dpu_coll_do_barrier(ctx, lsync);

            dpu_mark_coll_done(ctx, lsync);
            CTX_LOG("End coll id: %u, type: %d, count total: %lu, count serviced: %zu\n",
                    coll_id, coll_type, count_total, (size_t)ctx->coll_sync->count_serviced);

            dpu_coll_free_host_rkeys(ctx, dc, lsync);
        }
    }

end:
    pthread_barrier_wait(&sync_barrier);
    CTX_LOG("Communication thread %d is finalized \n", ctx->idx);
}

void _cleanup()
{
    dpu_hc_finalize(ucc_glob.hc);
    dpu_ucc_finalize(&ucc_glob);
}

void _sighandler(int signal)
{
    printf("Caught signal %d\n", signal);
}

int main(int argc, char **argv)
{
    char *s = NULL;
    int num_threads = 8;
    // s = getenv("UCC_MC_CPU_REDUCE_NUM_THREADS");
    // if (s) { num_threads = atoi(s); }
    
    int window_size = 1;
    // s = getenv("UCC_TL_DPU_BCAST_WINDOW");
    // if (s) { window_size = atoi(s); }
    hc.window_size = window_size;

    int listen_port = DEFAULT_PORT;
    s = getenv("LISTEN_PORT");
    if (s) { listen_port = atoi(s); }
    hc.port = listen_port;

    printf("DPU server: Running with %d OpenMP threads on port %d\n", num_threads, listen_port);
    UCC_CHECK(dpu_ucc_init(argc, argv, &ucc_glob));
    UCC_CHECK(dpu_hc_init(&hc));

    ucc_glob.hc = &hc;

    /* Try to clean up on Exit */
    // atexit(_cleanup);
    // signal(SIGINT, _sighandler);

    UCC_CHECK(dpu_hc_accept_job(&hc));
    UCS_CHECK(dpu_hc_connect_localhost_ep(&hc));

    dpu_ucc_comm_t comm = {0};
    thread_ctx_t comm_ctx[MAX_THREADS] = {0};
    UCC_CHECK(dpu_ucc_alloc_team(&ucc_glob, &comm));
    dpu_hc_connect_remote_hosts(&hc, &comm);

    dpu_coll_world_barrier(&comm);
    pthread_barrier_init(&sync_barrier, &barrier_attr, num_threads);
    
    for (int i=0; i<num_threads; i++) {
        thread_ctx_t *ctx = &comm_ctx[i];
        ctx->idx = i;
        ctx->nth = num_threads;
        ctx->hc = &hc;
        ctx->coll_sync = &coll_sync;
        ctx->thread_sync = &thread_sync;
        ctx->comm = &comm;
        ctx->dc = malloc(sizeof(dpu_hc_t));
        dpu_dc_create(ctx, ctx->hc, ctx->dc);
        dpu_hc_connect_remote_hosts(ctx->dc, &comm);
        pthread_create(&ctx->id, NULL, dpu_comm_thread, ctx);
        dpu_coll_world_barrier(&comm);
    }
    
    UCS_CHECK(dpu_send_init_completion(&hc));

    for (int i=0; i<num_threads; i++) {
        pthread_join(comm_ctx[i].id, NULL);
    }
    pthread_barrier_destroy(&sync_barrier);
    dpu_coll_world_barrier(&comm);
    dpu_ucc_free_team(&ucc_glob, &comm);
    
    for (int i=0; i<num_threads; i++) {
        thread_ctx_t *ctx = &comm_ctx[i];
        dpu_dc_reset(ctx->dc);
        free(ctx->dc);
    }
    dpu_hc_reset_job(&hc);
    dpu_hc_finalize(&hc);
    dpu_ucc_finalize(&ucc_glob);

    dpu_coll_print_summary();

    return EXIT_SUCCESS;
}
