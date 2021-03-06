/*
* Copyright (c) 2017, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include <float.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/common.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_cuda_internal.h"
#include "libavutil/cuda_check.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "scale_eval.h"
#include "video.h"

#include "cuda/load_helper.h"
#include "vf_scale_cuda.h"

static const enum AVPixelFormat supported_formats[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NV12,
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_P010,
    AV_PIX_FMT_P016,
    AV_PIX_FMT_YUV444P16,
    AV_PIX_FMT_0RGB32,
    AV_PIX_FMT_0BGR32,
};

#define DIV_UP(a, b) ( ((a) + (b) - 1) / (b) )
#define BLOCKX 32
#define BLOCKY 16

#define CHECK_CU(x) FF_CUDA_CHECK_DL(ctx, s->hwctx->internal->cuda_dl, x)

enum {
    INTERP_ALGO_DEFAULT,

    INTERP_ALGO_NEAREST,
    INTERP_ALGO_BILINEAR,
    INTERP_ALGO_BICUBIC,
    INTERP_ALGO_LANCZOS,

    INTERP_ALGO_COUNT
};

typedef struct CUDAScaleContext {
    const AVClass *class;

    AVCUDADeviceContext *hwctx;

    enum AVPixelFormat in_fmt;
    enum AVPixelFormat out_fmt;

    AVBufferRef *frames_ctx;
    AVFrame     *frame;

    AVFrame *tmp_frame;
    int passthrough;

    /**
     * Output sw format. AV_PIX_FMT_NONE for no conversion.
     */
    enum AVPixelFormat format;

    char *w_expr;               ///< width  expression string
    char *h_expr;               ///< height expression string

    int force_original_aspect_ratio;
    int force_divisible_by;

    CUcontext   cu_ctx;
    CUmodule    cu_module;
    CUfunction  cu_func_uchar;
    CUfunction  cu_func_uchar2;
    CUfunction  cu_func_uchar4;
    CUfunction  cu_func_ushort;
    CUfunction  cu_func_ushort2;
    CUfunction  cu_func_ushort4;
    CUstream    cu_stream;

    CUdeviceptr srcBuffer;
    CUdeviceptr dstBuffer;
    int         tex_alignment;

    int interp_algo;
    int interp_use_linear;
    int interp_as_integer;

    float param;
} CUDAScaleContext;

static av_cold int cudascale_init(AVFilterContext *ctx)
{
    CUDAScaleContext *s = ctx->priv;

    s->format = AV_PIX_FMT_NONE;
    s->frame = av_frame_alloc();
    if (!s->frame)
        return AVERROR(ENOMEM);

    s->tmp_frame = av_frame_alloc();
    if (!s->tmp_frame)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold void cudascale_uninit(AVFilterContext *ctx)
{
    CUDAScaleContext *s = ctx->priv;

    if (s->hwctx && s->cu_module) {
        CudaFunctions *cu = s->hwctx->internal->cuda_dl;
        CUcontext dummy;

        CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
        CHECK_CU(cu->cuModuleUnload(s->cu_module));
        s->cu_module = NULL;
        CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    }

    av_frame_free(&s->frame);
    av_buffer_unref(&s->frames_ctx);
    av_frame_free(&s->tmp_frame);
}

static int cudascale_query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pixel_formats[] = {
        AV_PIX_FMT_CUDA, AV_PIX_FMT_NONE,
    };
    AVFilterFormats *pix_fmts = ff_make_format_list(pixel_formats);
    if (!pix_fmts)
        return AVERROR(ENOMEM);

    return ff_set_common_formats(ctx, pix_fmts);
}

static av_cold int init_hwframe_ctx(CUDAScaleContext *s, AVBufferRef *device_ctx, int width, int height)
{
    AVBufferRef *out_ref = NULL;
    AVHWFramesContext *out_ctx;
    int ret;

    out_ref = av_hwframe_ctx_alloc(device_ctx);
    if (!out_ref)
        return AVERROR(ENOMEM);
    out_ctx = (AVHWFramesContext*)out_ref->data;

    out_ctx->format    = AV_PIX_FMT_CUDA;
    out_ctx->sw_format = s->out_fmt;
    out_ctx->width     = FFALIGN(width,  32);
    out_ctx->height    = FFALIGN(height, 32);

    ret = av_hwframe_ctx_init(out_ref);
    if (ret < 0)
        goto fail;

    av_frame_unref(s->frame);
    ret = av_hwframe_get_buffer(out_ref, s->frame, 0);
    if (ret < 0)
        goto fail;

    s->frame->width  = width;
    s->frame->height = height;

    av_buffer_unref(&s->frames_ctx);
    s->frames_ctx = out_ref;

    return 0;
fail:
    av_buffer_unref(&out_ref);
    return ret;
}

static int format_is_supported(enum AVPixelFormat fmt)
{
    int i;

    for (i = 0; i < FF_ARRAY_ELEMS(supported_formats); i++)
        if (supported_formats[i] == fmt)
            return 1;
    return 0;
}

static av_cold int init_processing_chain(AVFilterContext *ctx, int in_width, int in_height,
                                         int out_width, int out_height)
{
    CUDAScaleContext *s = ctx->priv;

    AVHWFramesContext *in_frames_ctx;

    enum AVPixelFormat in_format;
    enum AVPixelFormat out_format;
    int ret;

    /* check that we have a hw context */
    if (!ctx->inputs[0]->hw_frames_ctx) {
        av_log(ctx, AV_LOG_ERROR, "No hw context provided on input\n");
        return AVERROR(EINVAL);
    }
    in_frames_ctx = (AVHWFramesContext*)ctx->inputs[0]->hw_frames_ctx->data;
    in_format     = in_frames_ctx->sw_format;
    out_format    = (s->format == AV_PIX_FMT_NONE) ? in_format : s->format;

    if (!format_is_supported(in_format)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported input format: %s\n",
               av_get_pix_fmt_name(in_format));
        return AVERROR(ENOSYS);
    }
    if (!format_is_supported(out_format)) {
        av_log(ctx, AV_LOG_ERROR, "Unsupported output format: %s\n",
               av_get_pix_fmt_name(out_format));
        return AVERROR(ENOSYS);
    }

    s->in_fmt = in_format;
    s->out_fmt = out_format;

    if (s->passthrough && in_width == out_width && in_height == out_height && in_format == out_format) {
        s->frames_ctx = av_buffer_ref(ctx->inputs[0]->hw_frames_ctx);
        if (!s->frames_ctx)
            return AVERROR(ENOMEM);
    } else {
        s->passthrough = 0;

        ret = init_hwframe_ctx(s, in_frames_ctx->device_ref, out_width, out_height);
        if (ret < 0)
            return ret;
    }

    ctx->outputs[0]->hw_frames_ctx = av_buffer_ref(s->frames_ctx);
    if (!ctx->outputs[0]->hw_frames_ctx)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold int cudascale_config_props(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = outlink->src->inputs[0];
    CUDAScaleContext *s  = ctx->priv;
    AVHWFramesContext     *frames_ctx = (AVHWFramesContext*)inlink->hw_frames_ctx->data;
    AVCUDADeviceContext *device_hwctx = frames_ctx->device_ctx->hwctx;
    CUcontext dummy, cuda_ctx = device_hwctx->cuda_ctx;
    CudaFunctions *cu = device_hwctx->internal->cuda_dl;
    char buf[64];
    int w, h;
    int ret;

    const unsigned char *scaler_ptx;
    unsigned int scaler_ptx_len;
    const char *function_infix = "";

    extern const unsigned char ff_vf_scale_cuda_ptx_data[];
    extern const unsigned int ff_vf_scale_cuda_ptx_len;
    extern const unsigned char ff_vf_scale_cuda_bicubic_ptx_data[];
    extern const unsigned int ff_vf_scale_cuda_bicubic_ptx_len;

    switch(s->interp_algo) {
    case INTERP_ALGO_NEAREST:
        scaler_ptx = ff_vf_scale_cuda_ptx_data;
        scaler_ptx_len = ff_vf_scale_cuda_ptx_len;
        function_infix = "_Nearest";
        s->interp_use_linear = 0;
        s->interp_as_integer = 1;
        break;
    case INTERP_ALGO_BILINEAR:
        scaler_ptx = ff_vf_scale_cuda_ptx_data;
        scaler_ptx_len = ff_vf_scale_cuda_ptx_len;
        function_infix = "_Bilinear";
        s->interp_use_linear = 1;
        s->interp_as_integer = 1;
        break;
    case INTERP_ALGO_DEFAULT:
    case INTERP_ALGO_BICUBIC:
        scaler_ptx = ff_vf_scale_cuda_bicubic_ptx_data;
        scaler_ptx_len = ff_vf_scale_cuda_bicubic_ptx_len;
        function_infix = "_Bicubic";
        s->interp_use_linear = 0;
        s->interp_as_integer = 0;
        break;
    case INTERP_ALGO_LANCZOS:
        scaler_ptx = ff_vf_scale_cuda_bicubic_ptx_data;
        scaler_ptx_len = ff_vf_scale_cuda_bicubic_ptx_len;
        function_infix = "_Lanczos";
        s->interp_use_linear = 0;
        s->interp_as_integer = 0;
        break;
    default:
        av_log(ctx, AV_LOG_ERROR, "Unknown interpolation algorithm\n");
        return AVERROR_BUG;
    }

    s->hwctx = device_hwctx;
    s->cu_stream = s->hwctx->stream;

    ret = CHECK_CU(cu->cuCtxPushCurrent(cuda_ctx));
    if (ret < 0)
        goto fail;

    ret = ff_cuda_load_module(ctx, device_hwctx, &s->cu_module, scaler_ptx, scaler_ptx_len);
    if (ret < 0)
        goto fail;

    snprintf(buf, sizeof(buf), "Subsample%s_uchar", function_infix);
    CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_uchar, s->cu_module, buf));
    if (ret < 0)
        goto fail;

    snprintf(buf, sizeof(buf), "Subsample%s_uchar2", function_infix);
    CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_uchar2, s->cu_module, buf));
    if (ret < 0)
        goto fail;

    snprintf(buf, sizeof(buf), "Subsample%s_uchar4", function_infix);
    CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_uchar4, s->cu_module, buf));
    if (ret < 0)
        goto fail;

    snprintf(buf, sizeof(buf), "Subsample%s_ushort", function_infix);
    CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_ushort, s->cu_module, buf));
    if (ret < 0)
        goto fail;

    snprintf(buf, sizeof(buf), "Subsample%s_ushort2", function_infix);
    CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_ushort2, s->cu_module, buf));
    if (ret < 0)
        goto fail;

    snprintf(buf, sizeof(buf), "Subsample%s_ushort4", function_infix);
    CHECK_CU(cu->cuModuleGetFunction(&s->cu_func_ushort4, s->cu_module, buf));
    if (ret < 0)
        goto fail;


    CHECK_CU(cu->cuCtxPopCurrent(&dummy));

    if ((ret = ff_scale_eval_dimensions(s,
                                        s->w_expr, s->h_expr,
                                        inlink, outlink,
                                        &w, &h)) < 0)
        goto fail;

    ff_scale_adjust_dimensions(inlink, &w, &h,
                               s->force_original_aspect_ratio, s->force_divisible_by);

    if (((int64_t)h * inlink->w) > INT_MAX  ||
        ((int64_t)w * inlink->h) > INT_MAX)
        av_log(ctx, AV_LOG_ERROR, "Rescaled value for width or height is too big.\n");

    outlink->w = w;
    outlink->h = h;

    ret = init_processing_chain(ctx, inlink->w, inlink->h, w, h);
    if (ret < 0)
        return ret;

    av_log(ctx, AV_LOG_VERBOSE, "w:%d h:%d -> w:%d h:%d%s\n",
           inlink->w, inlink->h, outlink->w, outlink->h, s->passthrough ? " (passthrough)" : "");

    if (inlink->sample_aspect_ratio.num) {
        outlink->sample_aspect_ratio = av_mul_q((AVRational){outlink->h*inlink->w,
                                                             outlink->w*inlink->h},
                                                inlink->sample_aspect_ratio);
    } else {
        outlink->sample_aspect_ratio = inlink->sample_aspect_ratio;
    }

    return 0;

fail:
    return ret;
}

static int call_resize_kernel(AVFilterContext *ctx, CUfunction func, int channels,
                              uint8_t *src_dptr, int src_width, int src_height, int src_pitch,
                              uint8_t *dst_dptr, int dst_width, int dst_height, int dst_pitch,
                              int pixel_size, int bit_depth)
{
    CUDAScaleContext *s = ctx->priv;
    CudaFunctions *cu = s->hwctx->internal->cuda_dl;
    CUdeviceptr dst_devptr = (CUdeviceptr)dst_dptr;
    CUtexObject tex = 0;
    void *args_uchar[] = { &tex, &dst_devptr, &dst_width, &dst_height, &dst_pitch,
                           &src_width, &src_height, &bit_depth, &s->param };
    int ret;

    CUDA_TEXTURE_DESC tex_desc = {
        .filterMode = s->interp_use_linear ?
                      CU_TR_FILTER_MODE_LINEAR :
                      CU_TR_FILTER_MODE_POINT,
        .flags = s->interp_as_integer ? CU_TRSF_READ_AS_INTEGER : 0,
    };

    CUDA_RESOURCE_DESC res_desc = {
        .resType = CU_RESOURCE_TYPE_PITCH2D,
        .res.pitch2D.format = pixel_size == 1 ?
                              CU_AD_FORMAT_UNSIGNED_INT8 :
                              CU_AD_FORMAT_UNSIGNED_INT16,
        .res.pitch2D.numChannels = channels,
        .res.pitch2D.width = src_width,
        .res.pitch2D.height = src_height,
        .res.pitch2D.pitchInBytes = src_pitch,
        .res.pitch2D.devPtr = (CUdeviceptr)src_dptr,
    };

    // Handling of channels is done via vector-types in cuda, so their size is implicitly part of the pitch
    // Same for pixel_size, which is represented via datatypes on the cuda side of things.
    dst_pitch /= channels * pixel_size;

    ret = CHECK_CU(cu->cuTexObjectCreate(&tex, &res_desc, &tex_desc, NULL));
    if (ret < 0)
        goto exit;

    ret = CHECK_CU(cu->cuLaunchKernel(func,
                                      DIV_UP(dst_width, BLOCKX), DIV_UP(dst_height, BLOCKY), 1,
                                      BLOCKX, BLOCKY, 1, 0, s->cu_stream, args_uchar, NULL));

exit:
    if (tex)
        CHECK_CU(cu->cuTexObjectDestroy(tex));

    return ret;
}

static int scalecuda_resize(AVFilterContext *ctx,
                            AVFrame *out, AVFrame *in)
{
    AVHWFramesContext *in_frames_ctx = (AVHWFramesContext*)in->hw_frames_ctx->data;
    CUDAScaleContext *s = ctx->priv;

    switch (in_frames_ctx->sw_format) {
    case AV_PIX_FMT_YUV420P:
        call_resize_kernel(ctx, s->cu_func_uchar, 1,
                           in->data[0], in->width, in->height, in->linesize[0],
                           out->data[0], out->width, out->height, out->linesize[0],
                           1, 8);
        call_resize_kernel(ctx, s->cu_func_uchar, 1,
                           in->data[1], in->width / 2, in->height / 2, in->linesize[1],
                           out->data[1], out->width / 2, out->height / 2, out->linesize[1],
                           1, 8);
        call_resize_kernel(ctx, s->cu_func_uchar, 1,
                           in->data[2], in->width / 2, in->height / 2, in->linesize[2],
                           out->data[2], out->width / 2, out->height / 2, out->linesize[2],
                           1, 8);
        break;
    case AV_PIX_FMT_YUV444P:
        call_resize_kernel(ctx, s->cu_func_uchar, 1,
                           in->data[0], in->width, in->height, in->linesize[0],
                           out->data[0], out->width, out->height, out->linesize[0],
                           1, 8);
        call_resize_kernel(ctx, s->cu_func_uchar, 1,
                           in->data[1], in->width, in->height, in->linesize[1],
                           out->data[1], out->width, out->height, out->linesize[1],
                           1, 8);
        call_resize_kernel(ctx, s->cu_func_uchar, 1,
                           in->data[2], in->width, in->height, in->linesize[2],
                           out->data[2], out->width, out->height, out->linesize[2],
                           1, 8);
        break;
    case AV_PIX_FMT_YUV444P16:
        call_resize_kernel(ctx, s->cu_func_ushort, 1,
                           in->data[0], in->width, in->height, in->linesize[0],
                           out->data[0], out->width, out->height, out->linesize[0],
                           2, 16);
        call_resize_kernel(ctx, s->cu_func_ushort, 1,
                           in->data[1], in->width, in->height, in->linesize[1],
                           out->data[1], out->width, out->height, out->linesize[1],
                           2, 16);
        call_resize_kernel(ctx, s->cu_func_ushort, 1,
                           in->data[2], in->width, in->height, in->linesize[2],
                           out->data[2], out->width, out->height, out->linesize[2],
                           2, 16);
        break;
    case AV_PIX_FMT_NV12:
        call_resize_kernel(ctx, s->cu_func_uchar, 1,
                           in->data[0], in->width, in->height, in->linesize[0],
                           out->data[0], out->width, out->height, out->linesize[0],
                           1, 8);
        call_resize_kernel(ctx, s->cu_func_uchar2, 2,
                           in->data[1], in->width / 2, in->height / 2, in->linesize[1],
                           out->data[1], out->width / 2, out->height / 2, out->linesize[1],
                           1, 8);
        break;
    case AV_PIX_FMT_P010LE:
        call_resize_kernel(ctx, s->cu_func_ushort, 1,
                           in->data[0], in->width, in->height, in->linesize[0],
                           out->data[0], out->width, out->height, out->linesize[0],
                           2, 10);
        call_resize_kernel(ctx, s->cu_func_ushort2, 2,
                           in->data[1], in->width / 2, in->height / 2, in->linesize[1],
                           out->data[1], out->width / 2, out->height / 2, out->linesize[1],
                           2, 10);
        break;
    case AV_PIX_FMT_P016LE:
        call_resize_kernel(ctx, s->cu_func_ushort, 1,
                           in->data[0], in->width, in->height, in->linesize[0],
                           out->data[0], out->width, out->height, out->linesize[0],
                           2, 16);
        call_resize_kernel(ctx, s->cu_func_ushort2, 2,
                           in->data[1], in->width / 2, in->height / 2, in->linesize[1],
                           out->data[1], out->width / 2, out->height / 2, out->linesize[1],
                           2, 16);
        break;
    case AV_PIX_FMT_0RGB32:
    case AV_PIX_FMT_0BGR32:
        call_resize_kernel(ctx, s->cu_func_uchar4, 4,
                           in->data[0], in->width, in->height, in->linesize[0],
                           out->data[0], out->width, out->height, out->linesize[0],
                           1, 8);
        break;
    default:
        return AVERROR_BUG;
    }

    return 0;
}

static int cudascale_scale(AVFilterContext *ctx, AVFrame *out, AVFrame *in)
{
    CUDAScaleContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    AVFrame *src = in;
    int ret;

    ret = scalecuda_resize(ctx, s->frame, src);
    if (ret < 0)
        return ret;

    src = s->frame;
    ret = av_hwframe_get_buffer(src->hw_frames_ctx, s->tmp_frame, 0);
    if (ret < 0)
        return ret;

    av_frame_move_ref(out, s->frame);
    av_frame_move_ref(s->frame, s->tmp_frame);

    s->frame->width  = outlink->w;
    s->frame->height = outlink->h;

    ret = av_frame_copy_props(out, in);
    if (ret < 0)
        return ret;

    return 0;
}

static int cudascale_filter_frame(AVFilterLink *link, AVFrame *in)
{
    AVFilterContext       *ctx = link->dst;
    CUDAScaleContext        *s = ctx->priv;
    AVFilterLink      *outlink = ctx->outputs[0];
    CudaFunctions          *cu = s->hwctx->internal->cuda_dl;

    AVFrame *out = NULL;
    CUcontext dummy;
    int ret = 0;

    if (s->passthrough)
        return ff_filter_frame(outlink, in);

    out = av_frame_alloc();
    if (!out) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    ret = CHECK_CU(cu->cuCtxPushCurrent(s->hwctx->cuda_ctx));
    if (ret < 0)
        goto fail;

    ret = cudascale_scale(ctx, out, in);

    CHECK_CU(cu->cuCtxPopCurrent(&dummy));
    if (ret < 0)
        goto fail;

    av_reduce(&out->sample_aspect_ratio.num, &out->sample_aspect_ratio.den,
              (int64_t)in->sample_aspect_ratio.num * outlink->h * link->w,
              (int64_t)in->sample_aspect_ratio.den * outlink->w * link->h,
              INT_MAX);

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
fail:
    av_frame_free(&in);
    av_frame_free(&out);
    return ret;
}

static AVFrame *cudascale_get_video_buffer(AVFilterLink *inlink, int w, int h)
{
    CUDAScaleContext *s = inlink->dst->priv;

    return s->passthrough ?
        ff_null_get_video_buffer   (inlink, w, h) :
        ff_default_get_video_buffer(inlink, w, h);
}

#define OFFSET(x) offsetof(CUDAScaleContext, x)
#define FLAGS (AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM)
static const AVOption options[] = {
    { "w", "Output video width",  OFFSET(w_expr), AV_OPT_TYPE_STRING, { .str = "iw" }, .flags = FLAGS },
    { "h", "Output video height", OFFSET(h_expr), AV_OPT_TYPE_STRING, { .str = "ih" }, .flags = FLAGS },
    { "interp_algo", "Interpolation algorithm used for resizing", OFFSET(interp_algo), AV_OPT_TYPE_INT, { .i64 = INTERP_ALGO_DEFAULT }, 0, INTERP_ALGO_COUNT - 1, FLAGS, "interp_algo" },
        { "nearest",  "nearest neighbour", 0, AV_OPT_TYPE_CONST, { .i64 = INTERP_ALGO_NEAREST }, 0, 0, FLAGS, "interp_algo" },
        { "bilinear", "bilinear", 0, AV_OPT_TYPE_CONST, { .i64 = INTERP_ALGO_BILINEAR }, 0, 0, FLAGS, "interp_algo" },
        { "bicubic",  "bicubic",  0, AV_OPT_TYPE_CONST, { .i64 = INTERP_ALGO_BICUBIC  }, 0, 0, FLAGS, "interp_algo" },
        { "lanczos",  "lanczos",  0, AV_OPT_TYPE_CONST, { .i64 = INTERP_ALGO_LANCZOS  }, 0, 0, FLAGS, "interp_algo" },
    { "passthrough", "Do not process frames at all if parameters match", OFFSET(passthrough), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, FLAGS },
    { "param", "Algorithm-Specific parameter", OFFSET(param), AV_OPT_TYPE_FLOAT, { .dbl = SCALE_CUDA_PARAM_DEFAULT }, -FLT_MAX, FLT_MAX, FLAGS },
    { "force_original_aspect_ratio", "decrease or increase w/h if necessary to keep the original AR", OFFSET(force_original_aspect_ratio), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 2, FLAGS, "force_oar" },
        { "disable",  NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 0 }, 0, 0, FLAGS, "force_oar" },
        { "decrease", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 1 }, 0, 0, FLAGS, "force_oar" },
        { "increase", NULL, 0, AV_OPT_TYPE_CONST, {.i64 = 2 }, 0, 0, FLAGS, "force_oar" },
    { "force_divisible_by", "enforce that the output resolution is divisible by a defined integer when force_original_aspect_ratio is used", OFFSET(force_divisible_by), AV_OPT_TYPE_INT, { .i64 = 1 }, 1, 256, FLAGS },
    { NULL },
};

static const AVClass cudascale_class = {
    .class_name = "cudascale",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVFilterPad cudascale_inputs[] = {
    {
        .name        = "default",
        .type        = AVMEDIA_TYPE_VIDEO,
        .filter_frame = cudascale_filter_frame,
        .get_video_buffer = cudascale_get_video_buffer,
    },
    { NULL }
};

static const AVFilterPad cudascale_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = cudascale_config_props,
    },
    { NULL }
};

const AVFilter ff_vf_scale_cuda = {
    .name      = "scale_cuda",
    .description = NULL_IF_CONFIG_SMALL("GPU accelerated video resizer"),

    .init          = cudascale_init,
    .uninit        = cudascale_uninit,
    .query_formats = cudascale_query_formats,

    .priv_size = sizeof(CUDAScaleContext),
    .priv_class = &cudascale_class,

    .inputs    = cudascale_inputs,
    .outputs   = cudascale_outputs,

    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE,
};
