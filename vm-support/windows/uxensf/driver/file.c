/** @file
 *
 * VirtualBox Windows Guest Shared Folders
 *
 * File System Driver file routines
 */

/*
 * Copyright (C) 2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
/*
 * uXen changes:
 *
 * Copyright 2013-2015, Bromium, Inc.
 * SPDX-License-Identifier: ISC
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "vbsf.h"
#include <iprt/fs.h>

/* must be smaller than RING_SIZE + some overhead */
#define MAX_READ_SIZE 245760
#define MAX_WRITE_SIZE 245760

static NTSTATUS vbsfReadInternal(IN PRX_CONTEXT RxContext)
{
    NTSTATUS Status = STATUS_SUCCESS;

    RxCaptureFcb;
    RxCaptureFobx;

    PMRX_VBOX_DEVICE_EXTENSION pDeviceExtension = VBoxMRxGetDeviceExtension(RxContext);
    PMRX_VBOX_NETROOT_EXTENSION pNetRootExtension = VBoxMRxGetNetRootExtension(capFcb->pNetRoot);
    PMRX_VBOX_FOBX pVBoxFobx = VBoxMRxGetFileObjectExtension(capFobx);

    PLOWIO_CONTEXT LowIoContext = &RxContext->LowIoContext;

    PMDL BufferMdl = LowIoContext->ParamsFor.ReadWrite.Buffer;
    uint32_t ByteCount = LowIoContext->ParamsFor.ReadWrite.ByteCount;
    RXVBO ByteOffset = LowIoContext->ParamsFor.ReadWrite.ByteOffset;

    PVOID pbUserBuffer = RxLowIoGetBufferAddress(RxContext);

    int vboxRC;

    BOOLEAN AsyncIo = BooleanFlagOn(RxContext->Flags, RX_CONTEXT_FLAG_ASYNC_OPERATION);
    LONGLONG FileSize;

    RxGetFileSizeWithLock((PFCB)capFcb, &FileSize);

    Log(("VBOXSF: vbsfReadInternal: AsyncIo = %d, Fcb->FileSize = 0x%RX64\n",
         AsyncIo, capFcb->Header.FileSize.QuadPart));
    Log(("VBOXSF: vbsfReadInternal: UserBuffer %p, BufferMdl %p\n",
         pbUserBuffer, BufferMdl));
    Log(("VBOXSF: vbsfReadInternal: ByteCount 0x%X, ByteOffset 0x%RX64, FileSize 0x%RX64\n",
         ByteCount, ByteOffset, FileSize));

    /* @todo check if this is necessary. */
#ifdef FCB_STATE_READCACHING_ENABLED    /* Correct spelling for Vista 6001 SDK. */
    if (!FlagOn(capFcb->FcbState, FCB_STATE_READCACHING_ENABLED))
#else
    if (!FlagOn(capFcb->FcbState, FCB_STATE_READCACHEING_ENABLED))
#endif
    {
        if (ByteOffset >= FileSize)
        {
            Log(("VBOXSF: vbsfReadInternal: EOF\n"));
            return STATUS_END_OF_FILE;
        }

        if (ByteCount > FileSize - ByteOffset)
        {
            ByteCount = (ULONG)(FileSize - ByteOffset);
        }
    }

    /* @todo read 0 bytes == always success? */
    if (   BufferMdl == NULL
        || ByteCount == 0)
    {
        AssertFailed();
        return STATUS_INVALID_PARAMETER;
    }

    {
        uint32_t n_read = 0;
        uint32_t chunk, n;

        while ( n_read < ByteCount ) {
            chunk = ByteCount - n_read;
            if (chunk > MAX_READ_SIZE)
                chunk = MAX_READ_SIZE;
            n = chunk;
            vboxRC = vboxCallRead(&pDeviceExtension->hgcmClient, &pNetRootExtension->map, pVBoxFobx->hFile,
                                  ByteOffset + n_read, &n, (uint8_t *)pbUserBuffer + n_read, true /* locked */);
            n_read += n;
            Status = VBoxErrorToNTStatus(vboxRC);
            if (Status != STATUS_SUCCESS) {
                /* Nothing read. */
                n_read = 0;
                break;
            }
            if (n != chunk)
                break;
        }
        ByteCount = n_read;
    }
    RxContext->InformationToReturn = ByteCount;

    Log(("VBOXSF: vbsfReadInternal: Status = 0x%08X, ByteCount = 0x%X\n",
         Status, ByteCount));

    return Status;
}


static VOID vbsfReadWorker(VOID *pv)
{
    PRX_CONTEXT RxContext = (PRX_CONTEXT)pv;

    Log(("VBOXSF: vbsfReadWorker: calling the worker\n"));

    RxContext->IoStatusBlock.Status = vbsfReadInternal(RxContext);

    Log(("VBOXSF: vbsfReadWorker: Status 0x%08X\n",
         RxContext->IoStatusBlock.Status));

    RxLowIoCompletion(RxContext);
}


NTSTATUS VBoxMRxRead(IN PRX_CONTEXT RxContext)
{
    NTSTATUS Status = RxDispatchToWorkerThread(VBoxMRxDeviceObject, DelayedWorkQueue,
                                               vbsfReadWorker,
                                               RxContext);

    Log(("VBOXSF: MRxRead: RxDispatchToWorkerThread: Status 0x%08X\n",
         Status));

    if (Status == STATUS_SUCCESS)
    {
        Status = STATUS_PENDING;
    }

    return Status;
}

static NTSTATUS vbsfWriteInternal(IN PRX_CONTEXT RxContext)
{
    NTSTATUS Status = STATUS_SUCCESS;

    RxCaptureFcb;
    RxCaptureFobx;

    PMRX_VBOX_DEVICE_EXTENSION pDeviceExtension = VBoxMRxGetDeviceExtension(RxContext);
    PMRX_VBOX_NETROOT_EXTENSION pNetRootExtension = VBoxMRxGetNetRootExtension(capFcb->pNetRoot);
    PMRX_VBOX_FOBX pVBoxFobx = VBoxMRxGetFileObjectExtension(capFobx);

    PLOWIO_CONTEXT LowIoContext  = &RxContext->LowIoContext;

    PMDL BufferMdl = LowIoContext->ParamsFor.ReadWrite.Buffer;
    uint32_t ByteCount = LowIoContext->ParamsFor.ReadWrite.ByteCount;
    RXVBO ByteOffset = LowIoContext->ParamsFor.ReadWrite.ByteOffset;

    PVOID pbUserBuffer = RxLowIoGetBufferAddress(RxContext);

    int vboxRC;

    BOOLEAN AsyncIo = BooleanFlagOn(RxContext->Flags, RX_CONTEXT_FLAG_ASYNC_OPERATION);
    LONGLONG FileSize;

    RxGetFileSizeWithLock((PFCB)capFcb, &FileSize);

    Log(("VBOXSF: vbsfWriteInternal: AsyncIo = %d, Fcb->FileSize = 0x%RX64\n",
         AsyncIo, capFcb->Header.FileSize.QuadPart));
    Log(("VBOXSF: vbsfWriteInternal: UserBuffer %p, BufferMdl %p\n",
         pbUserBuffer, BufferMdl));
    Log(("VBOXSF: vbsfWriteInternal: ByteCount is 0x%X, ByteOffset is 0x%RX64, FileSize 0x%RX64\n",
         ByteCount, ByteOffset, FileSize));

    /* @todo allow to write 0 bytes. */
    if (   BufferMdl == NULL
        || ByteCount == 0)
    {
        AssertFailed();
        return STATUS_INVALID_PARAMETER;
    }

    {
        uint32_t n_written = 0;
        uint32_t chunk, n;

        while ( n_written < ByteCount ) {
            chunk = ByteCount - n_written;
            if (chunk > MAX_WRITE_SIZE)
                chunk = MAX_WRITE_SIZE;
            n = chunk;
            vboxRC = vboxCallWrite(&pDeviceExtension->hgcmClient, &pNetRootExtension->map, pVBoxFobx->hFile,
                                  ByteOffset + n_written, &n, (uint8_t *)pbUserBuffer + n_written, true /* locked */);
            n_written += n;
            Status = VBoxErrorToNTStatus(vboxRC);
            if (Status != STATUS_SUCCESS) {
                /* Nothing written. */
                n_written = 0;
                break;
            }
            if (n != chunk)
                break;
        }
        ByteCount = n_written;
    }

    RxContext->InformationToReturn = ByteCount;

    Log(("VBOXSF: vbsfWriteInternal: Status = 0x%08X, ByteCount = 0x%X\n",
         Status, ByteCount));

    return Status;
}

static VOID vbsfWriteWorker(VOID *pv)
{
    PRX_CONTEXT RxContext = (PRX_CONTEXT)pv;

    Log(("VBOXSF: vbsfWriteWorker: calling the worker\n"));

    RxContext->IoStatusBlock.Status = vbsfWriteInternal(RxContext);

    Log(("VBOXSF: vbsfWriteWorker: Status 0x%08X\n",
         RxContext->IoStatusBlock.Status));

    RxLowIoCompletion(RxContext);
}


NTSTATUS VBoxMRxWrite(IN PRX_CONTEXT RxContext)
{
    NTSTATUS Status = RxDispatchToWorkerThread(VBoxMRxDeviceObject, DelayedWorkQueue,
                                               vbsfWriteWorker,
                                               RxContext);

    Log(("VBOXSF: MRxWrite: RxDispatchToWorkerThread: Status 0x%08X\n",
         Status));

    if (Status == STATUS_SUCCESS)
    {
        Status = STATUS_PENDING;
    }

    return Status;
}


NTSTATUS VBoxMRxLocks(IN PRX_CONTEXT RxContext)
{
    NTSTATUS Status = STATUS_SUCCESS;

    RxCaptureFcb;
    RxCaptureFobx;

    PMRX_VBOX_DEVICE_EXTENSION pDeviceExtension = VBoxMRxGetDeviceExtension(RxContext);
    PMRX_VBOX_NETROOT_EXTENSION pNetRootExtension = VBoxMRxGetNetRootExtension(capFcb->pNetRoot);
    PMRX_VBOX_FOBX pVBoxFobx = VBoxMRxGetFileObjectExtension(capFobx);

    PLOWIO_CONTEXT LowIoContext = &RxContext->LowIoContext;
    uint32_t fu32Lock = 0;
    int vboxRC;

    Log(("VBOXSF: MRxLocks: Operation %d\n",
         LowIoContext->Operation));

    switch (LowIoContext->Operation)
    {
        default:
            AssertMsgFailed(("VBOXSF: MRxLocks: Unsupported lock/unlock type %d detected!\n",
                             LowIoContext->Operation));
            return STATUS_NOT_IMPLEMENTED;

        case LOWIO_OP_UNLOCK_MULTIPLE:
            /* @todo Remove multiple locks listed in LowIoContext.ParamsFor.Locks.LockList. */
            Log(("VBOXSF: MRxLocks: Unsupported LOWIO_OP_UNLOCK_MULTIPLE!\n",
                 LowIoContext->Operation));
            return STATUS_NOT_IMPLEMENTED;

        case LOWIO_OP_SHAREDLOCK:
            fu32Lock = SHFL_LOCK_SHARED | SHFL_LOCK_PARTIAL;
            break;

        case LOWIO_OP_EXCLUSIVELOCK:
            fu32Lock = SHFL_LOCK_EXCLUSIVE | SHFL_LOCK_PARTIAL;
            break;

        case LOWIO_OP_UNLOCK:
            fu32Lock = SHFL_LOCK_CANCEL | SHFL_LOCK_PARTIAL;
            break;
    }

    if (LowIoContext->ParamsFor.Locks.Flags & LOWIO_LOCKSFLAG_FAIL_IMMEDIATELY)
    {
        fu32Lock |= SHFL_LOCK_NOWAIT;
    }
    else
    {
        fu32Lock |= SHFL_LOCK_WAIT;
    }

    vboxRC = vboxCallLock(&pDeviceExtension->hgcmClient, &pNetRootExtension->map, pVBoxFobx->hFile,
                          LowIoContext->ParamsFor.Locks.ByteOffset, LowIoContext->ParamsFor.Locks.Length, fu32Lock);

    Status = VBoxErrorToNTStatus(vboxRC);

    Log(("VBOXSF: MRxLocks: Returned 0x%08X\n",
         Status));
    return Status;
}

NTSTATUS VBoxMRxCompleteBufferingStateChangeRequest(IN OUT PRX_CONTEXT RxContext,
                                                    IN OUT PMRX_SRV_OPEN SrvOpen,
                                                    IN PVOID pContext)
{
    Log(("VBOXSF: MRxCompleteBufferingStateChangeRequest: not implemented\n"));
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS VBoxMRxFlush (IN PRX_CONTEXT RxContext)
{
    NTSTATUS Status = STATUS_SUCCESS;

    RxCaptureFcb;
    RxCaptureFobx;

    PMRX_VBOX_DEVICE_EXTENSION pDeviceExtension = VBoxMRxGetDeviceExtension(RxContext);
    PMRX_VBOX_NETROOT_EXTENSION pNetRootExtension = VBoxMRxGetNetRootExtension(capFcb->pNetRoot);
    PMRX_VBOX_FOBX pVBoxFobx = VBoxMRxGetFileObjectExtension(capFobx);

    int vboxRC;

    Log(("VBOXSF: MRxFlush\n"));

    /* Do the actual flushing of file buffers */
    vboxRC = vboxCallFlush(&pDeviceExtension->hgcmClient, &pNetRootExtension->map, pVBoxFobx->hFile);

    Status = VBoxErrorToNTStatus(vboxRC);

    Log(("VBOXSF: MRxFlush: Returned 0x%08X\n", 
         Status));
    return Status;
}

NTSTATUS vbsfSetEndOfFile(IN OUT struct _RX_CONTEXT * RxContext,
                          IN OUT PLARGE_INTEGER pNewFileSize,
                          OUT PLARGE_INTEGER pNewAllocationSize)
{
    NTSTATUS Status = STATUS_SUCCESS;

    RxCaptureFcb;
    RxCaptureFobx;

    PMRX_VBOX_DEVICE_EXTENSION pDeviceExtension = VBoxMRxGetDeviceExtension(RxContext);
    PMRX_VBOX_NETROOT_EXTENSION pNetRootExtension = VBoxMRxGetNetRootExtension(capFcb->pNetRoot);
    PMRX_VBOX_FOBX pVBoxFobx = VBoxMRxGetFileObjectExtension(capFobx);

    PSHFLFSOBJINFO pObjInfo;
    uint32_t cbBuffer;
    int vboxRC;

    Log(("VBOXSF: vbsfSetEndOfFile: New size = %RX64 (%p), pNewAllocationSize = %p\n",
         pNewFileSize->QuadPart, pNewFileSize, pNewAllocationSize));

    Assert(pVBoxFobx && pNetRootExtension && pDeviceExtension);

    cbBuffer = sizeof(SHFLFSOBJINFO);
    pObjInfo = (SHFLFSOBJINFO *)vbsfAllocNonPagedMem(cbBuffer);
    if (pObjInfo == NULL)
    {
        AssertFailed();
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(pObjInfo, cbBuffer);
    pObjInfo->cbObject = pNewFileSize->QuadPart;

    vboxRC = vboxCallFSInfo(&pDeviceExtension->hgcmClient, &pNetRootExtension->map, pVBoxFobx->hFile,
                            SHFL_INFO_SET | SHFL_INFO_SIZE, &cbBuffer, (PSHFLDIRINFO)pObjInfo);

    Log(("VBOXSF: vbsfSetEndOfFile: vboxCallFSInfo returned %Rrc\n",
         vboxRC));

    Status = VBoxErrorToNTStatus(vboxRC);
    if (Status == STATUS_SUCCESS)
    {
        Log(("VBOXSF: vbsfSetEndOfFile: vboxCallFSInfo new allocation size = %RX64\n",
             pObjInfo->cbAllocated));

        /* Return new allocation size */
        pNewAllocationSize->QuadPart = pObjInfo->cbAllocated;
    }

    if (pObjInfo)
    {
        vbsfFreeNonPagedMem(pObjInfo);
    }

    Log(("VBOXSF: vbsfSetEndOfFile: Returned 0x%08X\n",
         Status));
    return Status;
}

NTSTATUS VBoxMRxExtendStub(IN OUT struct _RX_CONTEXT * RxContext,
                           IN OUT PLARGE_INTEGER pNewFileSize,
                           OUT PLARGE_INTEGER pNewAllocationSize)
{
    /* Note: On Windows hosts vbsfSetEndOfFile returns ACCESS_DENIED if the file has been
     *       opened in APPEND mode. Writes to a file will extend it anyway, therefore it is
     *       better to not call the host at all and tell the caller that the file was extended.
     */
    Log(("VBOXSF: MRxExtendStub: new size = %RX64\n",
         pNewFileSize->QuadPart));

    pNewAllocationSize->QuadPart = pNewFileSize->QuadPart;

    return STATUS_SUCCESS;
}
