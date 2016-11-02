/*
 * Copyright 2015-2016, Bromium, Inc.
 * Author: Piotr Foltyn <piotr.foltyn@gmail.com>
 * SPDX-License-Identifier: ISC
 */

#include <windows.h>
#include <stdint.h>
#include <assert.h>
#define V4V_USE_INLINE_API
#include <windows/uxenv4vlib/gh_v4vapi.h>

#include "uxenconsolelib.h"
#include "uxendisp-common.h"

#define ONE_MS_IN_HNS 10000
#define DUE_TIME_MS 100

struct disp_context {
    OVERLAPPED oread;
    OVERLAPPED owrite;
    void *priv;
    invalidate_rect_t inv_rect;
    v4v_channel_t v4v;
    uint8_t read_buf[UXENDISP_MAX_MSG_LEN];
    struct {
        v4v_datagram_t dgram;
        int dummy;
    } conn_msg;
    HANDLE timer;
    LARGE_INTEGER due_time;
    DWORD thread_id;
    BOOL exit;
};

static void CALLBACK write_done(DWORD ec, DWORD count, LPOVERLAPPED ovlpd);

static void CALLBACK
timer_done(LPVOID context, DWORD unused1, DWORD unused2)
{
    struct disp_context *c = (struct disp_context *)context;

    if (c->exit) {
        c->exit = FALSE;
        return;
    }

    WriteFileEx(c->v4v.v4v_handle,
                (void *)&c->conn_msg,
                sizeof(c->conn_msg),
                &c->owrite,
                write_done);
}

static int
parse_message(struct disp_context *c, void *buf, int size)
{
    if (size >= sizeof(struct dirty_rect)) {
        struct dirty_rect *rect = (struct dirty_rect*)buf;

        if (c->inv_rect)
            c->inv_rect(c->priv,
                        rect->left,
                        rect->top,
                        rect->right - rect->left,
                        rect->bottom - rect->top);
        return sizeof(struct dirty_rect);
    } else
        return size; /* eat unrecognized content */
}

static void
parse_messages(struct disp_context *c, void *buf, int size)
{
    void *p = buf;

    while (size >= sizeof(v4v_datagram_t)) {
        int bytes = parse_message(c, p + sizeof(v4v_datagram_t),
                                  size - sizeof(v4v_datagram_t));
        bytes += sizeof(v4v_datagram_t);
        p += bytes;
        size -= bytes;
    }
}

static void CALLBACK
read_done(DWORD ec, DWORD count, LPOVERLAPPED ovlpd)
{
    struct disp_context *c =
        CONTAINING_RECORD(ovlpd, struct disp_context, oread);

    if (c->exit) {
        c->exit = FALSE;
        return;
    }

    if (ec == 0)
        parse_messages(c, c->read_buf, count);

    WriteFileEx(c->v4v.v4v_handle,
                (void *)&c->conn_msg,
                sizeof(c->conn_msg),
                &c->owrite,
                write_done);
}

static void CALLBACK
write_done(DWORD ec, DWORD count, LPOVERLAPPED ovlpd)
{
    struct disp_context *c =
        CONTAINING_RECORD(ovlpd, struct disp_context, owrite);
    BOOL res;

    if (c->exit) {
        c->exit = FALSE;
        return;
    }

    if (ec != 0) {
        res = SetWaitableTimer(c->timer, &c->due_time, 0, timer_done, c, FALSE);
        if (!res) {
            // Last resort
            Sleep(DUE_TIME_MS);
            WriteFileEx(c->v4v.v4v_handle,
                        (void *)&c->conn_msg,
                        sizeof(c->conn_msg),
                        &c->owrite,
                        write_done);
        }
    } else {
        ReadFileEx(c->v4v.v4v_handle,
                   c->read_buf,
                   UXENDISP_MAX_MSG_LEN,
                   &c->oread,
                   read_done);
    }
}

disp_context_t
uxenconsole_disp_init(int vm_id, unsigned char *idtoken,
                      void *priv, invalidate_rect_t inv_rect)
{
    struct disp_context *c;
    v4v_bind_values_t bind = { };
    DWORD err;
    BOOL rc;

    c = calloc(1, sizeof (*c));
    if (!c)
        return NULL;

    c->thread_id = GetCurrentThreadId();

    if (!v4v_open(&c->v4v, UXENDISP_RING_SIZE, V4V_FLAG_ASYNC)) {
        err = GetLastError();
        goto error;
    }

    bind.ring_id.addr.port = UXENDISP_PORT;
    bind.ring_id.addr.domain = V4V_DOMID_ANY;
    if (vm_id == -1) {
        bind.ring_id.partner = V4V_DOMID_UUID;
        memcpy(&bind.partner, idtoken, sizeof(bind.partner));
    } else
        bind.ring_id.partner = vm_id;

    if (!v4v_bind(&c->v4v, &bind)) {
        // Allow one additional console to be connected.
        bind.ring_id.addr.port = UXENDISP_ALT_PORT;
        if (!v4v_bind(&c->v4v, &bind)) {
            err = GetLastError();
            goto error;
        }
    }

    c->conn_msg.dgram.addr.port = bind.ring_id.addr.port;
    c->conn_msg.dgram.addr.domain = bind.ring_id.partner;
    rc = WriteFileEx(c->v4v.v4v_handle,
                     (void *)&c->conn_msg,
                     sizeof(c->conn_msg),
                     &c->owrite,
                     write_done);
    if (rc == FALSE) {
        err = GetLastError();
        goto error;
    }

    c->due_time.QuadPart = -DUE_TIME_MS * ONE_MS_IN_HNS;
    c->timer = CreateWaitableTimer(NULL, TRUE, NULL);
    if (c->timer == NULL) {
        err = GetLastError();
        goto error;
    }

    c->priv = priv;
    c->inv_rect = inv_rect;

    return c;

error:
    uxenconsole_disp_cleanup(c);
    SetLastError(err);
    return NULL;
}

void
uxenconsole_disp_cleanup(disp_context_t ctx)
{
    struct disp_context *c = ctx;
    DWORD bytes;

    if (c) {
        // Cleanup must be called on the same thread as init was.
        assert(c->thread_id == GetCurrentThreadId());

        c->exit = TRUE;
        if (CancelIo(c->v4v.v4v_handle) ||
            (GetLastError() != ERROR_NOT_FOUND)) {
            GetOverlappedResult(c->v4v.v4v_handle,
                                &c->owrite,
                                &bytes,
                                TRUE);
            GetOverlappedResult(c->v4v.v4v_handle,
                                &c->oread,
                                &bytes,
                                TRUE);
        }
        // We need to put thread in alertable state to allow completion
        // routine to run.
        SleepEx(DUE_TIME_MS, TRUE);
        CloseHandle(c->timer);
        v4v_close(&c->v4v);
        free(c);
    }
}
