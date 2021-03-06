/*
 * Copyright 2014, 2015 Red Hat.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "virgl_context.h"
#include "virgl_resource.h"
#include "virgl_screen.h"

static void *virgl_buffer_transfer_map(struct pipe_context *ctx,
                                       struct pipe_resource *resource,
                                       unsigned level,
                                       unsigned usage,
                                       const struct pipe_box *box,
                                       struct pipe_transfer **transfer)
{
   struct virgl_context *vctx = virgl_context(ctx);
   struct virgl_screen *vs = virgl_screen(ctx->screen);
   struct virgl_resource *vbuf = virgl_resource(resource);
   struct virgl_transfer *trans;
   bool readback;
   bool flush = false;

   trans = virgl_resource_create_transfer(&vctx->transfer_pool, resource,
                                          &vbuf->metadata, level, usage, box);

   flush = virgl_res_needs_flush(vctx, trans);
   if (flush)
      ctx->flush(ctx, NULL, 0);

   readback = virgl_res_needs_readback(vctx, vbuf, usage, 0);
   if (readback)
      vs->vws->transfer_get(vs->vws, vbuf->hw_res, box, trans->base.stride,
                            trans->l_stride, trans->offset, level);

   /* XXX Consider
    *
    *   glBufferSubData(GL_ARRAY_BUFFER, 0, 12, data1);
    *   glDrawArrays(..);
    *   glFlush();
    *   glBufferSubData(GL_ARRAY_BUFFER, 0, 12, data2)
    *
    * readback and flush are both false in the second glBufferSubData call.
    * The draw call might end up seeing data2.
    */
   if (readback || flush)
      vs->vws->resource_wait(vs->vws, vbuf->hw_res);

   trans->hw_res_map = vs->vws->resource_map(vs->vws, vbuf->hw_res);
   if (!trans->hw_res_map) {
      virgl_resource_destroy_transfer(&vctx->transfer_pool, trans);
      return NULL;
   }

   *transfer = &trans->base;
   return trans->hw_res_map + trans->offset;
}

static void virgl_buffer_transfer_unmap(struct pipe_context *ctx,
                                        struct pipe_transfer *transfer)
{
   struct virgl_context *vctx = virgl_context(ctx);
   struct virgl_transfer *trans = virgl_transfer(transfer);

   if (trans->base.usage & PIPE_TRANSFER_WRITE) {
      if (transfer->usage & PIPE_TRANSFER_FLUSH_EXPLICIT) {
         if (trans->range.end <= trans->range.start) {
            virgl_resource_destroy_transfer(&vctx->transfer_pool, trans);
            return;
         }

         transfer->box.x += trans->range.start;
         transfer->box.width = trans->range.end - trans->range.start;
         trans->offset = transfer->box.x;
      }

      virgl_transfer_queue_unmap(&vctx->queue, trans);
   } else
      virgl_resource_destroy_transfer(&vctx->transfer_pool, trans);
}

static void virgl_buffer_transfer_flush_region(struct pipe_context *ctx,
                                               struct pipe_transfer *transfer,
                                               const struct pipe_box *box)
{
   struct virgl_transfer *trans = virgl_transfer(transfer);

   /*
    * FIXME: This is not optimal.  For example,
    *
    * glMapBufferRange(.., 0, 100, GL_MAP_FLUSH_EXPLICIT_BIT)
    * glFlushMappedBufferRange(.., 25, 30)
    * glFlushMappedBufferRange(.., 65, 70)
    *
    * We'll end up flushing 25 --> 70.
    */
   util_range_add(&trans->range, box->x, box->x + box->width);
}

static const struct u_resource_vtbl virgl_buffer_vtbl =
{
   u_default_resource_get_handle,            /* get_handle */
   virgl_resource_destroy,                   /* resource_destroy */
   virgl_buffer_transfer_map,                /* transfer_map */
   virgl_buffer_transfer_flush_region,       /* transfer_flush_region */
   virgl_buffer_transfer_unmap,              /* transfer_unmap */
};

void virgl_buffer_init(struct virgl_resource *res)
{
   res->u.vtbl = &virgl_buffer_vtbl;
}
