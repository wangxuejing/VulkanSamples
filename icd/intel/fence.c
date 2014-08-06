/*
 * XGL
 *
 * Copyright (C) 2014 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "kmd/winsys.h"
#include "dev.h"
#include "fence.h"

static void fence_destroy_callback(struct intel_obj *obj)
{
    struct intel_fence *fence = intel_fence_from_obj(obj);

    intel_fence_destroy(fence);
}

XGL_RESULT intel_fence_create(struct intel_dev *dev,
                              const XGL_FENCE_CREATE_INFO *info,
                              struct intel_fence **fence_ret)
{
    struct intel_fence *fence;

    fence = icd_alloc(sizeof(*fence), 0, XGL_SYSTEM_ALLOC_API_OBJECT);
    if (!fence)
        return XGL_ERROR_OUT_OF_MEMORY;

    memset(fence, 0, sizeof(*fence));

    fence->obj.destroy = fence_destroy_callback;

    fence->obj.base.dispatch = dev->base.dispatch;
    if (dev->base.dbg) {
        fence->obj.base.dbg =
            intel_base_dbg_create(XGL_DBG_OBJECT_FENCE, info, sizeof(*info));
        if (!fence->obj.base.dbg) {
            icd_free(fence);
            return XGL_ERROR_OUT_OF_MEMORY;
        }
    }

    *fence_ret = fence;

    return XGL_SUCCESS;
}

void intel_fence_destroy(struct intel_fence *fence)
{
    if (fence->submitted_bo)
        intel_bo_unreference(fence->submitted_bo);

    if (fence->obj.base.dbg)
        intel_base_dbg_destroy(fence->obj.base.dbg);

    icd_free(fence);
}

XGL_RESULT intel_fence_get_status(struct intel_fence *fence)
{
    if (!fence->submitted_bo)
        return XGL_ERROR_UNAVAILABLE;

    return (intel_bo_is_busy(fence->submitted_bo)) ?
        XGL_NOT_READY : XGL_SUCCESS;
}

XGL_RESULT intel_fence_wait(struct intel_fence *fence, int64_t timeout_ns)
{
    int err;

    if (!fence->submitted_bo)
        return XGL_ERROR_UNAVAILABLE;

    err = intel_bo_wait(fence->submitted_bo, timeout_ns);

    return (err) ? XGL_NOT_READY : XGL_SUCCESS;
}

XGL_RESULT XGLAPI intelCreateFence(
    XGL_DEVICE                                  device,
    const XGL_FENCE_CREATE_INFO*                pCreateInfo,
    XGL_FENCE*                                  pFence)
{
    struct intel_dev *dev = intel_dev(device);

    return intel_fence_create(dev, pCreateInfo,
            (struct intel_fence **) pFence);
}

XGL_RESULT XGLAPI intelGetFenceStatus(
    XGL_FENCE                                   fence_)
{
    struct intel_fence *fence = intel_fence(fence_);

    return intel_fence_get_status(fence);
}

XGL_RESULT XGLAPI intelWaitForFences(
    XGL_DEVICE                                  device,
    XGL_UINT                                    fenceCount,
    const XGL_FENCE*                            pFences,
    XGL_BOOL                                    waitAll,
    XGL_UINT64                                  timeout)
{
    XGL_RESULT ret = XGL_SUCCESS;
    XGL_UINT i;

    for (i = 0; i < fenceCount; i++) {
        struct intel_fence *fence = intel_fence(pFences[i]);
        int64_t ns;
        XGL_RESULT r;

        if (timeout <= (XGL_UINT64) (INT64_MAX / 1000 / 1000 / 1000))
            ns = timeout * 1000 * 1000 * 1000;
        else
            ns = -1;

        r = intel_fence_wait(fence, ns);

        if (!waitAll && r == XGL_SUCCESS)
            return XGL_SUCCESS;

        if (r != XGL_SUCCESS)
            ret = r;
    }

    return ret;
}
