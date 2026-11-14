/* SPDX-License-Identifier: MIT */
#include "NezhaCameraGraphOutput.h"
#include <Block.h>

/* HakiOS f128f1d camera_device.c supplies the graph-pool, output block,
 * completed-frame host time and three held preview buffers sequence.
 * Keys BufferPool, PixelBufferHandler and MetadataDictionary are decoded
 * from the original 24A446 CMCaptureCore/H13ISP, not old numeric addresses. */
enum { Unavailable = -12783 };

static void drop_oldest(struct NezhaCameraGraphOutput *output)
{
    if (!output->count) return;
    CFRelease(output->held[output->oldest]);
    output->held[output->oldest] = NULL;
    output->oldest = (output->oldest + 1) % 3;
    --output->count;
}

void nezha_camera_graph_output_stop(struct NezhaCameraGraphOutput *output)
{
    if (!output) return;
    output->running = false;
    ++output->generation;
    while (output->count) drop_oldest(output);
}

void nezha_camera_graph_output_reset(struct NezhaCameraGraphOutput *output)
{
    if (!output) return;
    nezha_camera_graph_output_stop(output);
    if (output->pool) CFRelease(output->pool);
    if (output->handler) Block_release(output->handler);
    output->pool = NULL;
    output->handler = NULL;
}

CVReturn nezha_camera_graph_output_configure(struct NezhaCameraGraphOutput *output,
    CVPixelBufferPoolRef pool, NezhaCameraPixelHandler handler, bool preview)
{
    if (!output || !pool || !handler || CFGetTypeID(pool) != CVPixelBufferPoolGetTypeID())
        return kCVReturnInvalidArgument;
    NezhaCameraPixelHandler retained = Block_copy(handler);
    if (!retained) return kCVReturnAllocationFailed;
    CFRetain(pool);
    const bool running = output->running;
    nezha_camera_graph_output_reset(output);
    output->pool = pool;
    output->handler = retained;
    output->preview = preview;
    output->running = running;
    return kCVReturnSuccess;
}

CVReturn nezha_camera_graph_output_start(struct NezhaCameraGraphOutput *output)
{
    if (!output || !output->pool || !output->handler) return Unavailable;
    output->running = true;
    return kCVReturnSuccess;
}

CVReturn nezha_camera_graph_output_deliver(struct NezhaCameraGraphOutput *output,
    const struct NezhaCameraPreview *preview, const struct NezhaCameraPreviewFrame *frame,
    const uint8_t *raw, size_t raw_bytes, uint64_t timestamp, CFDictionaryRef metadata)
{
    if (!output || !preview || !frame || !raw || !timestamp || !metadata ||
        CFGetTypeID(metadata) != CFDictionaryGetTypeID()) return kCVReturnInvalidArgument;
    if (!output->running || !output->pool || !output->handler) return Unavailable;
    /* Recycle the oldest before asking the bounded graph pool for another.
     * No allocation-retry loop or private replacement buffer is needed. */
    if (output->count == 3) drop_oldest(output);
    CVPixelBufferRef pixel = NULL;
    CVReturn status = CVPixelBufferPoolCreatePixelBuffer(NULL, output->pool, &pixel);
    if (status != kCVReturnSuccess) return status;
    if (!pixel) return kCVReturnAllocationFailed;
    status = nezha_camera_preview_pixel_buffer(preview, frame, raw, raw_bytes, pixel);
    if (status != kCVReturnSuccess) {
        CFRelease(pixel);
        return status;
    }
    CFMutableDictionaryRef attached = CFDictionaryCreateMutableCopy(NULL, 0, metadata);
    if (!attached) {
        CFRelease(pixel);
        return kCVReturnAllocationFailed;
    }
    CVBufferSetAttachment(pixel, CFSTR("MetadataDictionary"), attached, kCVAttachmentMode_ShouldPropagate);
    CFRelease(attached);
    /* Keep the block alive if its callback synchronously stops/reconfigures
     * this serial output. The old frame must not enter the new generation. */
    NezhaCameraPixelHandler handler = Block_copy(output->handler);
    if (!handler) {
        CFRelease(pixel);
        return kCVReturnAllocationFailed;
    }
    const unsigned generation = output->generation;
    handler(pixel, CMClockMakeHostTimeFromSystemUnits(timestamp));
    Block_release(handler);
    if (output->preview && output->running && generation == output->generation) {
        output->held[(output->oldest + output->count) % 3] = pixel;
        ++output->count;
    } else {
        CFRelease(pixel);
    }
    return kCVReturnSuccess;
}
