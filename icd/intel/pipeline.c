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
 *
 * Authors:
 *   Courtney Goeltzenleuchter <courtney@lunarg.com>
 *   Chia-I Wu <olv@lunarg.com>
 */

#include "genhw/genhw.h"
#include "cmd.h"
#include "format.h"
#include "shader.h"
#include "pipeline_priv.h"

static uint32_t *pipeline_cmd_ptr(struct intel_pipeline *pipeline, int cmd_len)
{
    uint32_t *ptr;

    assert(pipeline->cmd_len + cmd_len < INTEL_PSO_CMD_ENTRIES);
    ptr = &pipeline->cmds[pipeline->cmd_len];
    pipeline->cmd_len += cmd_len;
    return ptr;
}

static XGL_RESULT pipeline_build_ia(struct intel_pipeline *pipeline,
                                    const struct intel_pipeline_create_info* info)
{
    pipeline->topology = info->ia.topology;
    pipeline->disable_vs_cache = info->ia.disableVertexReuse;

    if (info->ia.provokingVertex == XGL_PROVOKING_VERTEX_FIRST) {
        pipeline->provoking_vertex_tri = 0;
        pipeline->provoking_vertex_trifan = 1;
        pipeline->provoking_vertex_line = 0;
    } else {
        pipeline->provoking_vertex_tri = 2;
        pipeline->provoking_vertex_trifan = 2;
        pipeline->provoking_vertex_line = 1;
    }

    switch (info->ia.topology) {
    case XGL_TOPOLOGY_POINT_LIST:
        pipeline->prim_type = GEN6_3DPRIM_POINTLIST;
        break;
    case XGL_TOPOLOGY_LINE_LIST:
        pipeline->prim_type = GEN6_3DPRIM_LINELIST;
        break;
    case XGL_TOPOLOGY_LINE_STRIP:
        pipeline->prim_type = GEN6_3DPRIM_LINESTRIP;
        break;
    case XGL_TOPOLOGY_TRIANGLE_LIST:
        pipeline->prim_type = GEN6_3DPRIM_TRILIST;
        break;
    case XGL_TOPOLOGY_TRIANGLE_STRIP:
        pipeline->prim_type = GEN6_3DPRIM_TRISTRIP;
        break;
    case XGL_TOPOLOGY_RECT_LIST:
        /*
         * TODO: Rect lists are special in XGL, do we need to do
         * something special here?
         * XGL Guide:
         * The rectangle list is a special geometry primitive type
         * that can be used for implementing post-processing techniques
         * or efficient copy operations. There are some special limitations
         * for rectangle primitives. They cannot be clipped, must
         * be axis aligned and cannot have depth gradient.
         * Failure to comply with these restrictions results in
         * undefined rendering results.
         */
        pipeline->prim_type = GEN6_3DPRIM_RECTLIST;
        break;
    case XGL_TOPOLOGY_QUAD_LIST:
        pipeline->prim_type = GEN6_3DPRIM_QUADLIST;
        break;
    case XGL_TOPOLOGY_QUAD_STRIP:
        pipeline->prim_type = GEN6_3DPRIM_QUADSTRIP;
        break;
    case XGL_TOPOLOGY_LINE_LIST_ADJ:
        pipeline->prim_type = GEN6_3DPRIM_LINELIST_ADJ;
        break;
    case XGL_TOPOLOGY_LINE_STRIP_ADJ:
        pipeline->prim_type = GEN6_3DPRIM_LINESTRIP_ADJ;
        break;
    case XGL_TOPOLOGY_TRIANGLE_LIST_ADJ:
        pipeline->prim_type = GEN6_3DPRIM_TRILIST_ADJ;
        break;
    case XGL_TOPOLOGY_TRIANGLE_STRIP_ADJ:
        pipeline->prim_type = GEN6_3DPRIM_TRISTRIP_ADJ;
        break;
    case XGL_TOPOLOGY_PATCH:
        if (!info->tess.patchControlPoints ||
            info->tess.patchControlPoints > 32)
            return XGL_ERROR_BAD_PIPELINE_DATA;
        pipeline->prim_type = GEN7_3DPRIM_PATCHLIST_1 +
            info->tess.patchControlPoints - 1;
        break;
    default:
        return XGL_ERROR_BAD_PIPELINE_DATA;
    }

    if (info->ia.primitiveRestartEnable) {
        pipeline->primitive_restart = true;
        pipeline->primitive_restart_index = info->ia.primitiveRestartIndex;
    } else {
        pipeline->primitive_restart = false;
    }

    return XGL_SUCCESS;
}

static XGL_RESULT pipeline_rs_state(struct intel_pipeline *pipeline,
                                    const XGL_PIPELINE_RS_STATE_CREATE_INFO* rs_state)
{
    pipeline->depthClipEnable = rs_state->depthClipEnable;
    pipeline->rasterizerDiscardEnable = rs_state->rasterizerDiscardEnable;
    pipeline->pointSize = rs_state->pointSize;
    return XGL_SUCCESS;
}

static void pipeline_destroy(struct intel_obj *obj)
{
    struct intel_pipeline *pipeline = intel_pipeline_from_obj(obj);

    pipeline_tear_shaders(pipeline);

    intel_base_destroy(&pipeline->obj.base);
}

static XGL_RESULT pipeline_validate(struct intel_pipeline *pipeline)
{
    /*
     * Validate required elements
     */
    if (!(pipeline->active_shaders & SHADER_VERTEX_FLAG)) {
        // TODO: Log debug message: Vertex Shader required.
        return XGL_ERROR_BAD_PIPELINE_DATA;
    }

    /*
     * Tessalation control and evaluation have to both have a shader defined or
     * neither should have a shader defined.
     */
    if (((pipeline->active_shaders & SHADER_TESS_CONTROL_FLAG) == 0) !=
         ((pipeline->active_shaders & SHADER_TESS_EVAL_FLAG) == 0) ) {
        // TODO: Log debug message: Both Tess control and Tess eval are required to use tessalation
        return XGL_ERROR_BAD_PIPELINE_DATA;
    }

    if ((pipeline->active_shaders & SHADER_COMPUTE_FLAG) &&
        (pipeline->active_shaders & (SHADER_VERTEX_FLAG | SHADER_TESS_CONTROL_FLAG |
                                     SHADER_TESS_EVAL_FLAG | SHADER_GEOMETRY_FLAG |
                                     SHADER_FRAGMENT_FLAG))) {
        // TODO: Log debug message: Can only specify compute shader when doing compute
        return XGL_ERROR_BAD_PIPELINE_DATA;
    }

    /*
     * XGL_TOPOLOGY_PATCH primitive topology is only valid for tessellation pipelines.
     * Mismatching primitive topology and tessellation fails graphics pipeline creation.
     */
    if (pipeline->active_shaders & (SHADER_TESS_CONTROL_FLAG | SHADER_TESS_EVAL_FLAG) &&
        (pipeline->topology != XGL_TOPOLOGY_PATCH)) {
        // TODO: Log debug message: Invalid topology used with tessalation shader.
        return XGL_ERROR_BAD_PIPELINE_DATA;
    }

    if ((pipeline->topology == XGL_TOPOLOGY_PATCH) &&
            (pipeline->active_shaders & ~(SHADER_TESS_CONTROL_FLAG | SHADER_TESS_EVAL_FLAG))) {
        // TODO: Log debug message: Cannot use TOPOLOGY_PATCH on non-tessalation shader.
        return XGL_ERROR_BAD_PIPELINE_DATA;
    }

    return XGL_SUCCESS;
}

static void pipeline_build_urb_alloc_gen6(struct intel_pipeline *pipeline,
                                          const struct intel_pipeline_create_info *info)
{
    const struct intel_gpu *gpu = pipeline->dev->gpu;
    const int urb_size = ((gpu->gt == 2) ? 64 : 32) * 1024;
    const struct intel_pipeline_shader *vs = &pipeline->vs;
    const struct intel_pipeline_shader *gs = &pipeline->gs;
    int vs_entry_size, gs_entry_size;
    int vs_size, gs_size;

    INTEL_GPU_ASSERT(gpu, 6, 6);

    vs_entry_size = ((vs->in_count >= vs->out_count) ?
        vs->in_count : vs->out_count);
    gs_entry_size = (gs) ? gs->out_count : 0;

    /* in bytes */
    vs_entry_size *= sizeof(float) * 4;
    gs_entry_size *= sizeof(float) * 4;

    if (pipeline->active_shaders & SHADER_GEOMETRY_FLAG) {
        vs_size = urb_size / 2;
        gs_size = vs_size;
    } else {
        vs_size = urb_size;
        gs_size = 0;
    }

    /* 3DSTATE_URB */
    {
        const uint8_t cmd_len = 3;
        const uint32_t dw0 = GEN6_RENDER_CMD(3D, 3DSTATE_URB) |
                             (cmd_len - 2);
        int vs_alloc_size, gs_alloc_size;
        int vs_entry_count, gs_entry_count;
        uint32_t *dw;

        /* in 1024-bit rows */
        vs_alloc_size = (vs_entry_size + 128 - 1) / 128;
        gs_alloc_size = (gs_entry_size + 128 - 1) / 128;

        /* valid range is [1, 5] */
        if (!vs_alloc_size)
            vs_alloc_size = 1;
        if (!gs_alloc_size)
            gs_alloc_size = 1;
        assert(vs_alloc_size <= 5 && gs_alloc_size <= 5);

        /* valid range is [24, 256], multiples of 4 */
        vs_entry_count = (vs_size / 128 / vs_alloc_size) & ~3;
        if (vs_entry_count > 256)
            vs_entry_count = 256;
        assert(vs_entry_count >= 24);

        /* valid range is [0, 256], multiples of 4 */
        gs_entry_count = (gs_size / 128 / gs_alloc_size) & ~3;
        if (gs_entry_count > 256)
            gs_entry_count = 256;

        dw = pipeline_cmd_ptr(pipeline, cmd_len);

        dw[0] = dw0;
        dw[1] = (vs_alloc_size - 1) << GEN6_URB_DW1_VS_ENTRY_SIZE__SHIFT |
                vs_entry_count << GEN6_URB_DW1_VS_ENTRY_COUNT__SHIFT;
        dw[2] = gs_entry_count << GEN6_URB_DW2_GS_ENTRY_COUNT__SHIFT |
                (gs_alloc_size - 1) << GEN6_URB_DW2_GS_ENTRY_SIZE__SHIFT;
    }
}

static void pipeline_build_urb_alloc_gen7(struct intel_pipeline *pipeline,
                                          const struct intel_pipeline_create_info *info)
{
    const struct intel_gpu *gpu = pipeline->dev->gpu;
    const int urb_size = ((gpu->gt == 3) ? 512 :
                          (gpu->gt == 2) ? 256 : 128) * 1024;
    const struct intel_pipeline_shader *vs = &pipeline->vs;
    const struct intel_pipeline_shader *gs = &pipeline->gs;
    /* some space is reserved for PCBs */
    int urb_offset = ((gpu->gt == 3) ? 32 : 16) * 1024;
    int vs_entry_size, gs_entry_size;
    int vs_size, gs_size;

    INTEL_GPU_ASSERT(gpu, 7, 7.5);

    vs_entry_size = ((vs->in_count >= vs->out_count) ?
        vs->in_count : vs->out_count);
    gs_entry_size = (gs) ? gs->out_count : 0;

    /* in bytes */
    vs_entry_size *= sizeof(float) * 4;
    gs_entry_size *= sizeof(float) * 4;

    if (pipeline->active_shaders & SHADER_GEOMETRY_FLAG) {
        vs_size = (urb_size - urb_offset) / 2;
        gs_size = vs_size;
    } else {
        vs_size = urb_size - urb_offset;
        gs_size = 0;
    }

    /* 3DSTATE_URB_* */
    {
        const uint8_t cmd_len = 2;
        int vs_alloc_size, gs_alloc_size;
        int vs_entry_count, gs_entry_count;
        uint32_t *dw;

        /* in 512-bit rows */
        vs_alloc_size = (vs_entry_size + 64 - 1) / 64;
        gs_alloc_size = (gs_entry_size + 64 - 1) / 64;

        if (!vs_alloc_size)
            vs_alloc_size = 1;
        if (!gs_alloc_size)
            gs_alloc_size = 1;

        /* avoid performance decrease due to banking */
        if (vs_alloc_size == 5)
            vs_alloc_size = 6;

        /* in multiples of 8 */
        vs_entry_count = (vs_size / 64 / vs_alloc_size) & ~7;
        assert(vs_entry_count >= 32);

        gs_entry_count = (gs_size / 64 / gs_alloc_size) & ~7;

        if (intel_gpu_gen(gpu) >= INTEL_GEN(7.5)) {
            const int max_vs_entry_count =
                (gpu->gt >= 2) ? 1664 : 640;
            const int max_gs_entry_count =
                (gpu->gt >= 2) ? 640 : 256;
            if (vs_entry_count >= max_vs_entry_count)
                vs_entry_count = max_vs_entry_count;
            if (gs_entry_count >= max_gs_entry_count)
                gs_entry_count = max_gs_entry_count;
        } else {
            const int max_vs_entry_count =
                (gpu->gt == 2) ? 704 : 512;
            const int max_gs_entry_count =
                (gpu->gt == 2) ? 320 : 192;
            if (vs_entry_count >= max_vs_entry_count)
                vs_entry_count = max_vs_entry_count;
            if (gs_entry_count >= max_gs_entry_count)
                gs_entry_count = max_gs_entry_count;
        }

        dw = pipeline_cmd_ptr(pipeline, cmd_len*4);
        dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_URB_VS) | (cmd_len - 2);
        dw[1] = (urb_offset / 8192) << GEN7_URB_ANY_DW1_OFFSET__SHIFT |
                (vs_alloc_size - 1) << GEN7_URB_ANY_DW1_ENTRY_SIZE__SHIFT |
                vs_entry_count;

        dw += 2;
        if (gs_size)
            urb_offset += vs_size;
        dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_URB_GS) | (cmd_len - 2);
        dw[1] = (urb_offset  / 8192) << GEN7_URB_ANY_DW1_OFFSET__SHIFT |
                (gs_alloc_size - 1) << GEN7_URB_ANY_DW1_ENTRY_SIZE__SHIFT |
                gs_entry_count;

        dw += 2;
        dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_URB_HS) | (cmd_len - 2);
        dw[1] = (urb_offset / 8192)  << GEN7_URB_ANY_DW1_OFFSET__SHIFT;

        dw += 2;
        dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_URB_DS) | (cmd_len - 2);
        dw[1] = (urb_offset / 8192)  << GEN7_URB_ANY_DW1_OFFSET__SHIFT;
    }
}

static void pipeline_build_push_const_alloc_gen7(struct intel_pipeline *pipeline,
                                                 const struct intel_pipeline_create_info *info)
{
    const uint8_t cmd_len = 2;
    uint32_t offset = 0;
    uint32_t size = 8192;
    uint32_t *dw;
    int end;

    INTEL_GPU_ASSERT(pipeline->dev->gpu, 7, 7.5);

    /*
    * From the Ivy Bridge PRM, volume 2 part 1, page 68:
    *
    *     "(A table that says the maximum size of each constant buffer is
    *      16KB")
    *
    * From the Ivy Bridge PRM, volume 2 part 1, page 115:
    *
    *     "The sum of the Constant Buffer Offset and the Constant Buffer Size
    *      may not exceed the maximum value of the Constant Buffer Size."
    *
    * Thus, the valid range of buffer end is [0KB, 16KB].
    */
    end = (offset + size) / 1024;
    if (end > 16) {
        assert(!"invalid constant buffer end");
        end = 16;
    }

    /* the valid range of buffer offset is [0KB, 15KB] */
    offset = (offset + 1023) / 1024;
    if (offset > 15) {
        assert(!"invalid constant buffer offset");
        offset = 15;
    }

    if (offset > end) {
        assert(!size);
        offset = end;
    }

    /* the valid range of buffer size is [0KB, 15KB] */
    size = end - offset;
    if (size > 15) {
        assert(!"invalid constant buffer size");
        size = 15;
    }

    dw = pipeline_cmd_ptr(pipeline, cmd_len * 5);
    dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_PUSH_CONSTANT_ALLOC_VS) | (cmd_len - 2);
    dw[1] = offset << GEN7_PCB_ALLOC_ANY_DW1_OFFSET__SHIFT |
                      size << GEN7_PCB_ALLOC_ANY_DW1_SIZE__SHIFT;

    dw += 2;
    dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_PUSH_CONSTANT_ALLOC_PS) | (cmd_len - 2);
    dw[1] = size << GEN7_PCB_ALLOC_ANY_DW1_OFFSET__SHIFT |
                    size << GEN7_PCB_ALLOC_ANY_DW1_SIZE__SHIFT;

    dw += 2;
    dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_PUSH_CONSTANT_ALLOC_HS) | (cmd_len - 2);
    dw[1] = 0 << GEN7_PCB_ALLOC_ANY_DW1_OFFSET__SHIFT |
                 0 << GEN7_PCB_ALLOC_ANY_DW1_SIZE__SHIFT;

    dw += 2;
    dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_PUSH_CONSTANT_ALLOC_DS) | (cmd_len - 2);
    dw[1] = 0 << GEN7_PCB_ALLOC_ANY_DW1_OFFSET__SHIFT |
                 0 << GEN7_PCB_ALLOC_ANY_DW1_SIZE__SHIFT;

    dw += 2;
    dw[0] = GEN7_RENDER_CMD(3D, 3DSTATE_PUSH_CONSTANT_ALLOC_GS) | (cmd_len - 2);
    dw[1] = 0 << GEN7_PCB_ALLOC_ANY_DW1_OFFSET__SHIFT |
                 0 << GEN7_PCB_ALLOC_ANY_DW1_SIZE__SHIFT;

    // gen7_wa_pipe_control_cs_stall(p, true, true);
    // looks equivalent to: gen6_wa_wm_multisample_flush - this does more
    // than the documentation seems to imply
}

static void pipeline_build_vertex_elements(struct intel_pipeline *pipeline,
                                           const struct intel_pipeline_create_info *info)
{
    const struct intel_pipeline_shader *vs = &pipeline->vs;
    uint8_t cmd_len;
    uint32_t *dw;
    XGL_UINT i;
    int comps[4];

    INTEL_GPU_ASSERT(pipeline->dev->gpu, 6, 7.5);

    cmd_len = 1 + 2 * info->vi.attributeCount;
    if (vs->uses & (INTEL_SHADER_USE_VID | INTEL_SHADER_USE_IID))
        cmd_len += 2;

    if (cmd_len == 1)
        return;

    dw = pipeline_cmd_ptr(pipeline, cmd_len);

    dw[0] = GEN6_RENDER_CMD(3D, 3DSTATE_VERTEX_ELEMENTS) |
            (cmd_len - 2);
    dw++;

    /* VERTEX_ELEMENT_STATE */
    for (i = 0; i < info->vi.attributeCount; i++) {
        const XGL_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION *attr =
            &info->vi.pVertexAttributeDescriptions[i];
        const int format =
            intel_format_translate_color(pipeline->dev->gpu, attr->format);

        comps[0] = GEN6_VFCOMP_STORE_0;
        comps[1] = GEN6_VFCOMP_STORE_0;
        comps[2] = GEN6_VFCOMP_STORE_0;
        comps[3] = icd_format_is_int(attr->format) ?
            GEN6_VFCOMP_STORE_1_INT : GEN6_VFCOMP_STORE_1_FP;

        switch (icd_format_get_channel_count(attr->format)) {
        case 4: comps[3] = GEN6_VFCOMP_STORE_SRC; /* fall through */
        case 3: comps[2] = GEN6_VFCOMP_STORE_SRC; /* fall through */
        case 2: comps[1] = GEN6_VFCOMP_STORE_SRC; /* fall through */
        case 1: comps[0] = GEN6_VFCOMP_STORE_SRC; break;
        default:
            break;
        }

        assert(attr->offsetInBytes <= 2047);

        dw[0] = attr->binding << GEN6_VE_STATE_DW0_VB_INDEX__SHIFT |
                GEN6_VE_STATE_DW0_VALID |
                format << GEN6_VE_STATE_DW0_FORMAT__SHIFT |
                attr->offsetInBytes;

        dw[1] = comps[0] << GEN6_VE_STATE_DW1_COMP0__SHIFT |
                comps[1] << GEN6_VE_STATE_DW1_COMP1__SHIFT |
                comps[2] << GEN6_VE_STATE_DW1_COMP2__SHIFT |
                comps[3] << GEN6_VE_STATE_DW1_COMP3__SHIFT;

        dw += 2;
    }

    if (vs->uses & (INTEL_SHADER_USE_VID | INTEL_SHADER_USE_IID)) {
        comps[0] = (vs->uses & INTEL_SHADER_USE_VID) ?
            GEN6_VFCOMP_STORE_VID : GEN6_VFCOMP_STORE_0;
        comps[1] = (vs->uses & INTEL_SHADER_USE_IID) ?
            GEN6_VFCOMP_STORE_IID : GEN6_VFCOMP_NOSTORE;
        comps[2] = GEN6_VFCOMP_NOSTORE;
        comps[3] = GEN6_VFCOMP_NOSTORE;

        dw[0] = GEN6_VE_STATE_DW0_VALID;
        dw[1] = comps[0] << GEN6_VE_STATE_DW1_COMP0__SHIFT |
                comps[1] << GEN6_VE_STATE_DW1_COMP1__SHIFT |
                comps[2] << GEN6_VE_STATE_DW1_COMP2__SHIFT |
                comps[3] << GEN6_VE_STATE_DW1_COMP3__SHIFT;

        dw += 2;
    }
}

static void pipeline_build_gs(struct intel_pipeline *pipeline,
                              const struct intel_pipeline_create_info *info)
{
    // gen7_emit_3DSTATE_GS done by cmd_pipeline
}

static void pipeline_build_hs(struct intel_pipeline *pipeline,
                              const struct intel_pipeline_create_info *info)
{
    const uint8_t cmd_len = 7;
    const uint32_t dw0 = GEN7_RENDER_CMD(3D, 3DSTATE_HS) | (cmd_len - 2);
    uint32_t *dw;

    INTEL_GPU_ASSERT(pipeline->dev->gpu, 7, 7.5);

    dw = pipeline_cmd_ptr(pipeline, cmd_len);
    dw[0] = dw0;
    dw[1] = 0;
    dw[2] = 0;
    dw[3] = 0;
    dw[4] = 0;
    dw[5] = 0;
    dw[6] = 0;
}

static void pipeline_build_te(struct intel_pipeline *pipeline,
                              const struct intel_pipeline_create_info *info)
{
    const uint8_t cmd_len = 4;
    const uint32_t dw0 = GEN7_RENDER_CMD(3D, 3DSTATE_TE) | (cmd_len - 2);
    uint32_t *dw;

    INTEL_GPU_ASSERT(pipeline->dev->gpu, 7, 7.5);

    dw = pipeline_cmd_ptr(pipeline, cmd_len);
    dw[0] = dw0;
    dw[1] = 0;
    dw[2] = 0;
    dw[3] = 0;
}

static void pipeline_build_ds(struct intel_pipeline *pipeline,
                              const struct intel_pipeline_create_info *info)
{
    const uint8_t cmd_len = 6;
    const uint32_t dw0 = GEN7_RENDER_CMD(3D, 3DSTATE_DS) | (cmd_len - 2);
    uint32_t *dw;

    INTEL_GPU_ASSERT(pipeline->dev->gpu, 7, 7.5);

    dw = pipeline_cmd_ptr(pipeline, cmd_len);
    dw[0] = dw0;
    dw[1] = 0;
    dw[2] = 0;
    dw[3] = 0;
    dw[4] = 0;
    dw[5] = 0;
}

static XGL_RESULT pipeline_build_all(struct intel_pipeline *pipeline,
                                     const struct intel_pipeline_create_info *info)
{
    XGL_RESULT ret;

    ret = pipeline_build_shaders(pipeline, info);
    if (ret != XGL_SUCCESS)
        return ret;

    if (info->vi.bindingCount > ARRAY_SIZE(pipeline->vb) ||
        info->vi.attributeCount > ARRAY_SIZE(pipeline->vb))
        return XGL_ERROR_BAD_PIPELINE_DATA;

    pipeline->vb_count = info->vi.bindingCount;
    memcpy(pipeline->vb, info->vi.pVertexBindingDescriptions,
            sizeof(pipeline->vb[0]) * pipeline->vb_count);

    pipeline_build_vertex_elements(pipeline, info);

    if (intel_gpu_gen(pipeline->dev->gpu) >= INTEL_GEN(7)) {
        pipeline_build_urb_alloc_gen7(pipeline, info);
        pipeline_build_push_const_alloc_gen7(pipeline, info);
        pipeline_build_gs(pipeline, info);
        pipeline_build_hs(pipeline, info);
        pipeline_build_te(pipeline, info);
        pipeline_build_ds(pipeline, info);

        pipeline->wa_flags = INTEL_CMD_WA_GEN6_PRE_DEPTH_STALL_WRITE |
                             INTEL_CMD_WA_GEN6_PRE_COMMAND_SCOREBOARD_STALL |
                             INTEL_CMD_WA_GEN7_PRE_VS_DEPTH_STALL_WRITE |
                             INTEL_CMD_WA_GEN7_POST_COMMAND_CS_STALL |
                             INTEL_CMD_WA_GEN7_POST_COMMAND_DEPTH_STALL;
    } else {
        pipeline_build_urb_alloc_gen6(pipeline, info);

        pipeline->wa_flags = INTEL_CMD_WA_GEN6_PRE_DEPTH_STALL_WRITE |
                             INTEL_CMD_WA_GEN6_PRE_COMMAND_SCOREBOARD_STALL;
    }

    ret = pipeline_build_ia(pipeline, info);

    if (ret == XGL_SUCCESS)
        ret = pipeline_rs_state(pipeline, &info->rs);

    if (ret == XGL_SUCCESS) {
        pipeline->db_format = info->db.format;
        pipeline->cb_state = info->cb;
        pipeline->tess_state = info->tess;
    }

    return ret;
}

struct intel_pipeline_create_info_header {
    XGL_STRUCTURE_TYPE struct_type;
    const struct intel_pipeline_create_info_header *next;
};

static XGL_RESULT pipeline_create_info_init(struct intel_pipeline_create_info *info,
                                            const struct intel_pipeline_create_info_header *header)
{
    memset(info, 0, sizeof(*info));

    while (header) {
        const void *src = (const void *) header;
        XGL_SIZE size;
        void *dst;

        switch (header->struct_type) {
        case XGL_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO:
            size = sizeof(info->graphics);
            dst = &info->graphics;
            break;
        case XGL_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_CREATE_INFO:
            size = sizeof(info->vi);
            dst = &info->vi;
            break;
        case XGL_STRUCTURE_TYPE_PIPELINE_IA_STATE_CREATE_INFO:
            size = sizeof(info->ia);
            dst = &info->ia;
            break;
        case XGL_STRUCTURE_TYPE_PIPELINE_DB_STATE_CREATE_INFO:
            size = sizeof(info->db);
            dst = &info->db;
            break;
        case XGL_STRUCTURE_TYPE_PIPELINE_CB_STATE_CREATE_INFO:
            size = sizeof(info->cb);
            dst = &info->cb;
            break;
        case XGL_STRUCTURE_TYPE_PIPELINE_RS_STATE_CREATE_INFO:
            size = sizeof(info->rs);
            dst = &info->rs;
            break;
        case XGL_STRUCTURE_TYPE_PIPELINE_TESS_STATE_CREATE_INFO:
            size = sizeof(info->tess);
            dst = &info->tess;
            break;
        case XGL_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO:
            {
                const XGL_PIPELINE_SHADER *shader =
                    (const XGL_PIPELINE_SHADER *) (header + 1);

                src = (const void *) shader;
                size = sizeof(*shader);

                switch (shader->stage) {
                case XGL_SHADER_STAGE_VERTEX:
                    dst = &info->vs;
                    break;
                case XGL_SHADER_STAGE_TESS_CONTROL:
                    dst = &info->tcs;
                    break;
                case XGL_SHADER_STAGE_TESS_EVALUATION:
                    dst = &info->tes;
                    break;
                case XGL_SHADER_STAGE_GEOMETRY:
                    dst = &info->gs;
                    break;
                case XGL_SHADER_STAGE_FRAGMENT:
                    dst = &info->fs;
                    break;
                default:
                    return XGL_ERROR_BAD_PIPELINE_DATA;
                    break;
                }
            }
            break;
        case XGL_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO:
            size = sizeof(info->compute);
            dst = &info->compute;
            break;
        default:
            return XGL_ERROR_BAD_PIPELINE_DATA;
            break;
        }

        memcpy(dst, src, size);

        header = header->next;
    }

    return XGL_SUCCESS;
}

static XGL_RESULT graphics_pipeline_create(struct intel_dev *dev,
                                           const XGL_GRAPHICS_PIPELINE_CREATE_INFO *info_,
                                           struct intel_pipeline **pipeline_ret)
{
    struct intel_pipeline_create_info info;
    struct intel_pipeline *pipeline;
    XGL_RESULT ret;

    ret = pipeline_create_info_init(&info,
            (const struct intel_pipeline_create_info_header *) info_);
    if (ret != XGL_SUCCESS)
        return ret;

    pipeline = (struct intel_pipeline *)
        intel_base_create(dev, sizeof(*pipeline), dev->base.dbg,
                XGL_DBG_OBJECT_GRAPHICS_PIPELINE, info_, 0);
    if (!pipeline)
        return XGL_ERROR_OUT_OF_MEMORY;

    pipeline->dev = dev;
    pipeline->obj.destroy = pipeline_destroy;

    ret = pipeline_build_all(pipeline, &info);
    if (ret == XGL_SUCCESS)
        ret = pipeline_validate(pipeline);
    if (ret != XGL_SUCCESS) {
        pipeline_destroy(&pipeline->obj);
        return ret;
    }

    *pipeline_ret = pipeline;

    return XGL_SUCCESS;
}

XGL_RESULT XGLAPI intelCreateGraphicsPipeline(
    XGL_DEVICE                                  device,
    const XGL_GRAPHICS_PIPELINE_CREATE_INFO*    pCreateInfo,
    XGL_PIPELINE*                               pPipeline)
{
    struct intel_dev *dev = intel_dev(device);

    return graphics_pipeline_create(dev, pCreateInfo,
            (struct intel_pipeline **) pPipeline);
}

XGL_RESULT XGLAPI intelCreateComputePipeline(
    XGL_DEVICE                                  device,
    const XGL_COMPUTE_PIPELINE_CREATE_INFO*     pCreateInfo,
    XGL_PIPELINE*                               pPipeline)
{
    return XGL_ERROR_UNAVAILABLE;
}

XGL_RESULT XGLAPI intelStorePipeline(
    XGL_PIPELINE                                pipeline,
    XGL_SIZE*                                   pDataSize,
    XGL_VOID*                                   pData)
{
    return XGL_ERROR_UNAVAILABLE;
}

XGL_RESULT XGLAPI intelLoadPipeline(
    XGL_DEVICE                                  device,
    XGL_SIZE                                    dataSize,
    const XGL_VOID*                             pData,
    XGL_PIPELINE*                               pPipeline)
{
    return XGL_ERROR_UNAVAILABLE;
}

XGL_RESULT XGLAPI intelCreatePipelineDelta(
    XGL_DEVICE                                  device,
    XGL_PIPELINE                                p1,
    XGL_PIPELINE                                p2,
    XGL_PIPELINE_DELTA*                         delta)
{
    return XGL_ERROR_UNAVAILABLE;
}
