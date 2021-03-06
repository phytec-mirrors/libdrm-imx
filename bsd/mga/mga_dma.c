/* mga_dma.c -- DMA support for mga g200/g400
 * Created: Mon Dec 13 01:50:01 1999 by jhartmann@precisioninsight.com
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors: Rickard E. (Rik) Faith <faith@valinux.com>
 *	    Jeff Hartmann <jhartmann@valinux.com>
 *	    Keith Whitwell <keithw@valinux.com>
 *
 */

#define __NO_VERSION__
#include "drmP.h"
#include "mga_drv.h"

#include <machine/bus.h>
#include <machine/resource.h>
#include <sys/rman.h>
#include <vm/vm.h>
#include <vm/pmap.h>

#define MGA_REG(reg)		2
#define MGA_BASE(reg)		((unsigned long) \
				((drm_device_t *)dev)->maplist[MGA_REG(reg)]->handle)
#define MGA_ADDR(reg)		(MGA_BASE(reg) + reg)
#define MGA_DEREF(reg)		*(__volatile__ int *)MGA_ADDR(reg)
#define MGA_READ(reg)		MGA_DEREF(reg)
#define MGA_WRITE(reg,val) 	do { MGA_DEREF(reg) = val; } while (0)

#define PDEA_pagpxfer_enable 	     0x2

static int mga_flush_queue(drm_device_t *dev);

static unsigned long mga_alloc_page(drm_device_t *dev)
{
	unsigned long address;
   
	DRM_DEBUG("%s\n", __FUNCTION__);

	address = (unsigned long) drm_alloc(PAGE_SIZE, DRM_MEM_DMA);
	if(address == 0UL) {
		return 0;
	}
   
	return address;
}

static void mga_free_page(drm_device_t *dev, unsigned long page)
{
	DRM_DEBUG("%s\n", __FUNCTION__);

	if(page == 0UL) {
		return;
	}
	drm_free((void *) page, PAGE_SIZE, DRM_MEM_DMA);
	return;
}

static void mga_delay(void)
{
   	return;
}

void mga_flush_write_combine(void)
{
   	int xchangeDummy;
	DRM_DEBUG("%s\n", __FUNCTION__);

   	__asm__ volatile(" push %%eax ; xchg %%eax, %0 ; pop %%eax" : : "m" (xchangeDummy));
   	__asm__ volatile(" push %%eax ; push %%ebx ; push %%ecx ; push %%edx ;"
			 " movl $0,%%eax ; cpuid ; pop %%edx ; pop %%ecx ; pop %%ebx ;"
			 " pop %%eax" : /* no outputs */ :  /* no inputs */ );
}

/* These are two age tags that will never be sent to
 * the hardware */
#define MGA_BUF_USED 	0xffffffff
#define MGA_BUF_FREE	0

static int mga_freelist_init(drm_device_t *dev)
{
      	drm_device_dma_t *dma = dev->dma;
   	drm_buf_t *buf;
   	drm_mga_buf_priv_t *buf_priv;
      	drm_mga_private_t *dev_priv = (drm_mga_private_t *)dev->dev_private;
   	drm_mga_freelist_t *item;
   	int i;

	DRM_DEBUG("%s\n", __FUNCTION__);

   	dev_priv->head = drm_alloc(sizeof(drm_mga_freelist_t), DRM_MEM_DRIVER);
	if(dev_priv->head == NULL) return ENOMEM;
   	memset(dev_priv->head, 0, sizeof(drm_mga_freelist_t));
   	dev_priv->head->age = MGA_BUF_USED;
   
   	for (i = 0; i < dma->buf_count; i++) {
	   	buf = dma->buflist[ i ];
	        buf_priv = buf->dev_private;
		item = drm_alloc(sizeof(drm_mga_freelist_t),
				 DRM_MEM_DRIVER);
	   	if(item == NULL) return ENOMEM;
	   	memset(item, 0, sizeof(drm_mga_freelist_t));
	  	item->age = MGA_BUF_FREE;
	   	item->prev = dev_priv->head;
	   	item->next = dev_priv->head->next;
	   	if(dev_priv->head->next != NULL)
			dev_priv->head->next->prev = item;
	   	if(item->next == NULL) dev_priv->tail = item;
	   	item->buf = buf;
	   	buf_priv->my_freelist = item;
		buf_priv->discard = 0;
		buf_priv->dispatched = 0;
	   	dev_priv->head->next = item;
	}
   
   	return 0;
}

static void mga_freelist_cleanup(drm_device_t *dev)
{
      	drm_mga_private_t *dev_priv = (drm_mga_private_t *)dev->dev_private;
   	drm_mga_freelist_t *item;
   	drm_mga_freelist_t *prev;

	DRM_DEBUG("%s\n", __FUNCTION__);

   	item = dev_priv->head;
   	while(item) {
	   	prev = item;
	   	item = item->next;
	   	drm_free(prev, sizeof(drm_mga_freelist_t), DRM_MEM_DRIVER);
	}
   
   	dev_priv->head = dev_priv->tail = NULL;
}

/* Frees dispatch lock */
static __inline void mga_dma_quiescent(drm_device_t *dev)
{
	drm_device_dma_t  *dma      = dev->dma;
	drm_mga_private_t *dev_priv = (drm_mga_private_t *)dev->dev_private;
	drm_mga_sarea_t *sarea_priv = dev_priv->sarea_priv;
   	unsigned long end;
	int i;

	DRM_DEBUG("%s\n", __FUNCTION__);
	end = ticks + (hz*3);
    	while(1) {
		if(!test_and_set_bit(MGA_IN_DISPATCH, 
				     &dev_priv->dispatch_status)) {
			break;
		}
	   	if((signed)(end - ticks) <= 0) {
			DRM_ERROR("irqs: %d wanted %d\n", 
				  atomic_read(&dev->total_irq), 
				  atomic_read(&dma->total_lost));
			DRM_ERROR("lockup\n"); 
			goto out_nolock;
		}
		for (i = 0 ; i < 2000 ; i++) mga_delay();
	}
	end = ticks + (hz*3);
    	DRM_DEBUG("quiescent status : %x\n", MGA_READ(MGAREG_STATUS));
    	while((MGA_READ(MGAREG_STATUS) & 0x00030001) != 0x00020000) {
		if((signed)(end - ticks) <= 0) {
			DRM_ERROR("irqs: %d wanted %d\n", 
				  atomic_read(&dev->total_irq), 
				  atomic_read(&dma->total_lost));
			DRM_ERROR("lockup\n"); 
			goto out_status;
		}
		for (i = 0 ; i < 2000 ; i++) mga_delay();	  
	}
    	sarea_priv->dirty |= MGA_DMA_FLUSH;

out_status:
    	clear_bit(MGA_IN_DISPATCH, &dev_priv->dispatch_status);
out_nolock:
}

static void mga_reset_freelist(drm_device_t *dev)
{
   	drm_device_dma_t  *dma      = dev->dma;
   	drm_buf_t *buf;
   	drm_mga_buf_priv_t *buf_priv;
	int i;

   	for (i = 0; i < dma->buf_count; i++) {
	   	buf = dma->buflist[ i ];
	        buf_priv = buf->dev_private;
		buf_priv->my_freelist->age = MGA_BUF_FREE;
	}
}

/* Least recently used :
 * These operations are not atomic b/c they are protected by the 
 * hardware lock */

drm_buf_t *mga_freelist_get(drm_device_t *dev)
{
   	drm_mga_private_t *dev_priv = 
     		(drm_mga_private_t *) dev->dev_private;
	drm_mga_freelist_t *prev;
   	drm_mga_freelist_t *next;
	static int failed = 0;
	int ret, s;

	DRM_DEBUG("%s : tail->age : %d last_prim_age : %d\n", __FUNCTION__,
	       dev_priv->tail->age, dev_priv->last_prim_age);
   
	if(failed >= 1000 && dev_priv->tail->age >= dev_priv->last_prim_age) {
		DRM_DEBUG("I'm waiting on the freelist!!! %d\n", 
		       dev_priv->last_prim_age);
		s = splsofttq();
	   	set_bit(MGA_IN_GETBUF, &dev_priv->dispatch_status);
	   	for (;;) {
		   	mga_dma_schedule(dev, 0);
		   	if(!test_bit(MGA_IN_GETBUF, 
				     &dev_priv->dispatch_status)) 
				break;
		   	atomic_inc(&dev->total_sleeps);
			ret = tsleep(&dev_priv->buf_queue, PZERO|PCATCH,
				     "mgafg", 0);
			if (ret) {
				clear_bit(MGA_IN_GETBUF,
					  &dev_priv->dispatch_status);
				splx(s);
			   	goto failed_getbuf;
			}
		}
		splx(s);
	}
   
   	if(dev_priv->tail->age < dev_priv->last_prim_age) {
		prev = dev_priv->tail->prev;
	   	next = dev_priv->tail;
	   	prev->next = NULL;
	   	next->prev = next->next = NULL;
	   	dev_priv->tail = prev;
	   	next->age = MGA_BUF_USED;
		failed = 0;
	   	return next->buf;
	}

failed_getbuf:
	failed++;
   	return NULL;
}

int mga_freelist_put(drm_device_t *dev, drm_buf_t *buf)
{
      	drm_mga_private_t *dev_priv = 
     		(drm_mga_private_t *) dev->dev_private;
   	drm_mga_buf_priv_t *buf_priv = buf->dev_private;
	drm_mga_freelist_t *prev;
   	drm_mga_freelist_t *head;
   	drm_mga_freelist_t *next;

	DRM_DEBUG("%s\n", __FUNCTION__);

   	if(buf_priv->my_freelist->age == MGA_BUF_USED) {
		/* Discarded buffer, put it on the tail */
		next = buf_priv->my_freelist;
		next->age = MGA_BUF_FREE;
		prev = dev_priv->tail;
		prev->next = next;
		next->prev = prev;
		next->next = NULL;
		dev_priv->tail = next;
		DRM_DEBUG("Discarded\n");
	} else {
		/* Normally aged buffer, put it on the head + 1,
		 * as the real head is a sentinal element
		 */
		next = buf_priv->my_freelist;
		head = dev_priv->head;
		prev = head->next;
		head->next = next;
		prev->prev = next;
		next->prev = head;
		next->next = prev;
	}
   
   	return 0;
}

static int mga_init_primary_bufs(drm_device_t *dev, drm_mga_init_t *init)
{
   	drm_mga_private_t *dev_priv = dev->dev_private;
	drm_mga_prim_buf_t *prim_buffer;
   	int i, temp, size_of_buf;
   	int offset = init->reserved_map_agpstart;

	DRM_DEBUG("%s\n", __FUNCTION__);
   	dev_priv->primary_size = ((init->primary_size + PAGE_SIZE - 1) / 
				  PAGE_SIZE) * PAGE_SIZE;
   	size_of_buf = dev_priv->primary_size / MGA_NUM_PRIM_BUFS;
	dev_priv->warp_ucode_size = init->warp_ucode_size;
   	dev_priv->prim_bufs = drm_alloc(sizeof(drm_mga_prim_buf_t *) * 
					(MGA_NUM_PRIM_BUFS + 1), 
					DRM_MEM_DRIVER);
   	if(dev_priv->prim_bufs == NULL) {
		DRM_ERROR("Unable to allocate memory for prim_buf\n");
		return ENOMEM;
	}
   	memset(dev_priv->prim_bufs, 
	       0, sizeof(drm_mga_prim_buf_t *) * (MGA_NUM_PRIM_BUFS + 1));
   
   	temp = init->warp_ucode_size + dev_priv->primary_size;
	temp = ((temp + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
	   
	dev_priv->ioremap = drm_ioremap(dev->agp->base + offset, 
					temp);
	if(dev_priv->ioremap == NULL) {
		DRM_DEBUG("Ioremap failed\n");
		return ENOMEM;
	}
   	dev_priv->wait_queue = 0;
   
   	for(i = 0; i < MGA_NUM_PRIM_BUFS; i++) {
	   	prim_buffer = drm_alloc(sizeof(drm_mga_prim_buf_t), 
					DRM_MEM_DRIVER);
	   	if(prim_buffer == NULL) return ENOMEM;
	   	memset(prim_buffer, 0, sizeof(drm_mga_prim_buf_t));
	   	prim_buffer->phys_head = offset + dev->agp->base;
	   	prim_buffer->current_dma_ptr = 
			prim_buffer->head = 
			(u_int32_t *) (dev_priv->ioremap + 
				       offset - 
				       init->reserved_map_agpstart);
	   	prim_buffer->num_dwords = 0;
	   	prim_buffer->max_dwords = size_of_buf / sizeof(u_int32_t);
	   	prim_buffer->max_dwords -= 5; /* Leave room for the softrap */
	   	prim_buffer->sec_used = 0;
	   	prim_buffer->idx = i;
		prim_buffer->prim_age = i + 1;
	   	offset = offset + size_of_buf;
	   	dev_priv->prim_bufs[i] = prim_buffer;
	}
	dev_priv->current_prim_idx = 0;
        dev_priv->next_prim = 
		dev_priv->last_prim = 
		dev_priv->current_prim =
        	dev_priv->prim_bufs[0];
	dev_priv->next_prim_age = 2;	
	dev_priv->last_prim_age = 1;
   	set_bit(MGA_BUF_IN_USE, &dev_priv->current_prim->buffer_status);
   	return 0;
}

static void mga_fire_primary(drm_device_t *dev, drm_mga_prim_buf_t *prim)
{
       	drm_mga_private_t *dev_priv = dev->dev_private;
      	drm_device_dma_t  *dma	    = dev->dma;
       	drm_mga_sarea_t *sarea_priv = dev_priv->sarea_priv;
 	int use_agp = PDEA_pagpxfer_enable;
	unsigned long end;
   	int i;
   	int next_idx;
       	PRIMLOCALS;

   	DRM_DEBUG("%s\n", __FUNCTION__);
   	dev_priv->last_prim = prim;
   
 	/* We never check for overflow, b/c there is always room */
    	PRIMPTR(prim);
   	if(num_dwords <= 0) {
		DRM_DEBUG("num_dwords == 0 when dispatched\n");
		goto out_prim_wait;
	}
 	PRIMOUTREG( MGAREG_DMAPAD, 0);
 	PRIMOUTREG( MGAREG_DMAPAD, 0);
       	PRIMOUTREG( MGAREG_DMAPAD, 0);
   	PRIMOUTREG( MGAREG_SOFTRAP, 0);
    	PRIMFINISH(prim);

	end = ticks + (hz*3);
    	if(sarea_priv->dirty & MGA_DMA_FLUSH) {
		DRM_DEBUG("Dma top flush\n");	   
		while((MGA_READ(MGAREG_STATUS) & 0x00030001) != 0x00020000) {
			if((signed)(end - ticks) <= 0) {
				DRM_ERROR("irqs: %d wanted %d\n", 
					  atomic_read(&dev->total_irq), 
					  atomic_read(&dma->total_lost));
				DRM_ERROR("lockup in fire primary "
					  "(Dma Top Flush)\n");
				goto out_prim_wait;
			}
	      
			for (i = 0 ; i < 4096 ; i++) mga_delay();
		}
		sarea_priv->dirty &= ~(MGA_DMA_FLUSH);
	} else {
		DRM_DEBUG("Status wait\n");
		while((MGA_READ(MGAREG_STATUS) & 0x00020001) != 0x00020000) {
			if((signed)(end - ticks) <= 0) {
				DRM_ERROR("irqs: %d wanted %d\n", 
					  atomic_read(&dev->total_irq), 
					  atomic_read(&dma->total_lost));
				DRM_ERROR("lockup in fire primary "
					  "(Status Wait)\n");
				goto out_prim_wait;
			}
	   
			for (i = 0 ; i < 4096 ; i++) mga_delay();
		}
	}

   	mga_flush_write_combine();
    	atomic_inc(&dev_priv->pending_bufs);
       	MGA_WRITE(MGAREG_PRIMADDRESS, phys_head | TT_GENERAL);
 	MGA_WRITE(MGAREG_PRIMEND, (phys_head + num_dwords * 4) | use_agp);
   	prim->num_dwords = 0;
	sarea_priv->last_enqueue = prim->prim_age;
    
   	next_idx = prim->idx + 1;
    	if(next_idx >= MGA_NUM_PRIM_BUFS) 
		next_idx = 0;

    	dev_priv->next_prim = dev_priv->prim_bufs[next_idx];
	return;

 out_prim_wait:
	prim->num_dwords = 0;
	prim->sec_used = 0;
	clear_bit(MGA_BUF_IN_USE, &prim->buffer_status);
   	wakeup(&dev_priv->wait_queue);
	clear_bit(MGA_BUF_SWAP_PENDING, &prim->buffer_status);
	clear_bit(MGA_IN_DISPATCH, &dev_priv->dispatch_status);
}

int mga_advance_primary(drm_device_t *dev)
{
   	drm_mga_private_t *dev_priv = dev->dev_private;
   	drm_mga_prim_buf_t *prim_buffer;
   	drm_device_dma_t  *dma      = dev->dma;
   	int		  next_prim_idx;
   	int		  ret = 0;
	int		  s;
   
   	/* This needs to reset the primary buffer if available,
	 * we should collect stats on how many times it bites
	 * it's tail */
	DRM_DEBUG("%s\n", __FUNCTION__);
   
   	next_prim_idx = dev_priv->current_prim_idx + 1;
   	if(next_prim_idx >= MGA_NUM_PRIM_BUFS)
     		next_prim_idx = 0;
   	prim_buffer = dev_priv->prim_bufs[next_prim_idx];
	set_bit(MGA_IN_WAIT, &dev_priv->dispatch_status);
   
      	/* In use is cleared in interrupt handler */
   
	s = splsofttq();
   	if(test_and_set_bit(MGA_BUF_IN_USE, &prim_buffer->buffer_status)) {
	   	for (;;) {
		   	mga_dma_schedule(dev, 0);
		   	if(!test_and_set_bit(MGA_BUF_IN_USE, 
					     &prim_buffer->buffer_status)) 
				break;
		   	atomic_inc(&dev->total_sleeps);
		   	atomic_inc(&dma->total_missed_sched);
			ret = tsleep(&dev_priv->wait_queue, PZERO|PCATCH,
				     "mgaap", 0);
			if (ret)
				break;
		}
	   	if(ret) {
			splx(s);
			return ret;
		}
	}
	clear_bit(MGA_IN_WAIT, &dev_priv->dispatch_status);
	splx(s);

   	/* This primary buffer is now free to use */
   	prim_buffer->current_dma_ptr = prim_buffer->head;
   	prim_buffer->num_dwords = 0;
   	prim_buffer->sec_used = 0;
	prim_buffer->prim_age = dev_priv->next_prim_age++;
	if(prim_buffer->prim_age == 0 || prim_buffer->prim_age == 0xffffffff) {
	   mga_flush_queue(dev);
	   mga_dma_quiescent(dev);
	   mga_reset_freelist(dev);
	   prim_buffer->prim_age = (dev_priv->next_prim_age += 2);
	}

	/* Reset all buffer status stuff */
	clear_bit(MGA_BUF_NEEDS_OVERFLOW, &prim_buffer->buffer_status);
	clear_bit(MGA_BUF_FORCE_FIRE, &prim_buffer->buffer_status);
	clear_bit(MGA_BUF_SWAP_PENDING, &prim_buffer->buffer_status);

   	dev_priv->current_prim = prim_buffer;
   	dev_priv->current_prim_idx = next_prim_idx;
   	return 0;
}

/* More dynamic performance decisions */
static __inline int mga_decide_to_fire(drm_device_t *dev)
{
   	drm_mga_private_t *dev_priv = (drm_mga_private_t *)dev->dev_private;
      	drm_device_dma_t  *dma	    = dev->dma;

   	DRM_DEBUG("%s\n", __FUNCTION__);

   	if(test_bit(MGA_BUF_FORCE_FIRE, &dev_priv->next_prim->buffer_status)) {
	   	atomic_inc(&dma->total_prio);
	   	return 1;
	}

	if (test_bit(MGA_IN_GETBUF, &dev_priv->dispatch_status) &&
	    dev_priv->next_prim->num_dwords) {
	   	atomic_inc(&dma->total_prio);
	   	return 1;
	}

	if (test_bit(MGA_IN_FLUSH, &dev_priv->dispatch_status) &&
	    dev_priv->next_prim->num_dwords) {
	   	atomic_inc(&dma->total_prio);
	   	return 1;
	}
   
   	if(atomic_read(&dev_priv->pending_bufs) <= MGA_NUM_PRIM_BUFS - 1) {
		if(test_bit(MGA_BUF_SWAP_PENDING, 
			    &dev_priv->next_prim->buffer_status)) {
			atomic_inc(&dma->total_dmas);
			return 1;
		}
	}

   	if(atomic_read(&dev_priv->pending_bufs) <= MGA_NUM_PRIM_BUFS / 2) {
		if(dev_priv->next_prim->sec_used >= MGA_DMA_BUF_NR / 8) {
			atomic_inc(&dma->total_hit);
			return 1;
		}
	}

   	if(atomic_read(&dev_priv->pending_bufs) >= MGA_NUM_PRIM_BUFS / 2) {
		if(dev_priv->next_prim->sec_used >= MGA_DMA_BUF_NR / 4) {
			atomic_inc(&dma->total_missed_free);
			return 1;
		}
	}

   	atomic_inc(&dma->total_tried);
   	return 0;
}

int mga_dma_schedule(drm_device_t *dev, int locked)
{
      	drm_mga_private_t *dev_priv = (drm_mga_private_t *)dev->dev_private;
      	drm_device_dma_t  *dma	    = dev->dma;

   	if (test_and_set_bit(0, &dev->dma_flag)) {
		atomic_inc(&dma->total_missed_dma);
		return EBUSY;
	}
   
	DRM_DEBUG("%s\n", __FUNCTION__);
	if (!dev_priv) {
		DRM_DEBUG("dev_priv is not set\n");
		return (0);
	}

   	if(test_bit(MGA_IN_FLUSH, &dev_priv->dispatch_status) || 
	   test_bit(MGA_IN_WAIT, &dev_priv->dispatch_status) ||
	   test_bit(MGA_IN_GETBUF, &dev_priv->dispatch_status)) {
		locked = 1;
	}
   
   	if (!locked && 
	    !drm_lock_take(&dev->lock.hw_lock->lock, DRM_KERNEL_CONTEXT)) {
	   	atomic_inc(&dma->total_missed_lock);
	   	clear_bit(0, &dev->dma_flag);
		DRM_DEBUG("Not locked\n");
	   	return EBUSY;
	}
   	DRM_DEBUG("I'm locked\n");

   	if(!test_and_set_bit(MGA_IN_DISPATCH, &dev_priv->dispatch_status)) {
	   	/* Fire dma buffer */
	   	if(mga_decide_to_fire(dev)) {
		   	DRM_DEBUG("idx :%d\n", dev_priv->next_prim->idx);
			clear_bit(MGA_BUF_FORCE_FIRE, 
				  &dev_priv->next_prim->buffer_status);
		   	if(dev_priv->current_prim == dev_priv->next_prim) {
				/* Schedule overflow for a later time */
				set_bit(MGA_BUF_NEEDS_OVERFLOW,
					&dev_priv->next_prim->buffer_status);
			}
		   	mga_fire_primary(dev, dev_priv->next_prim);
		} else {
			clear_bit(MGA_IN_DISPATCH, &dev_priv->dispatch_status);
		}
	} else {
		DRM_DEBUG("I can't get the dispatch lock\n");
	}
   	
	if (!locked) {
		if (drm_lock_free(dev, &dev->lock.hw_lock->lock,
				  DRM_KERNEL_CONTEXT)) {
			DRM_ERROR("\n");
		}
	}

      	if(test_bit(MGA_IN_FLUSH, &dev_priv->dispatch_status) &&
	   dev_priv->next_prim->num_dwords == 0 &&
	   atomic_read(&dev_priv->pending_bufs) == 0) {
	   	/* Everything has been processed by the hardware */
		clear_bit(MGA_IN_FLUSH, &dev_priv->dispatch_status);
	   	wakeup(&dev_priv->flush_queue);
	}

	if(test_bit(MGA_IN_GETBUF, &dev_priv->dispatch_status) &&
	   dev_priv->tail->age < dev_priv->last_prim_age) {
		clear_bit(MGA_IN_GETBUF, &dev_priv->dispatch_status);
		DRM_DEBUG("Waking up buf queue\n");
		wakeup(&dev_priv->buf_queue);
	} else if (test_bit(MGA_IN_GETBUF, &dev_priv->dispatch_status)) {
	   	DRM_DEBUG("Not waking buf_queue on %d %d\n", 
			  atomic_read(&dev->total_irq), 
			  dev_priv->last_prim_age);
	}

   	clear_bit(0, &dev->dma_flag);
	return 0;
}

static void mga_dma_service(void *arg)
{
    	drm_device_t	 *dev = (drm_device_t *)arg;
    	drm_mga_private_t *dev_priv = (drm_mga_private_t *)dev->dev_private;
    	drm_mga_prim_buf_t *last_prim_buffer;

	DRM_DEBUG("%s\n", __FUNCTION__);
    	atomic_inc(&dev->total_irq);
	if((MGA_READ(MGAREG_STATUS) & 0x00000001) != 0x00000001) return;
      	MGA_WRITE(MGAREG_ICLEAR, 0x00000001);
   	last_prim_buffer = dev_priv->last_prim;
    	last_prim_buffer->num_dwords = 0;
    	last_prim_buffer->sec_used = 0;
	dev_priv->sarea_priv->last_dispatch = 
		dev_priv->last_prim_age = last_prim_buffer->prim_age;
      	clear_bit(MGA_BUF_IN_USE, &last_prim_buffer->buffer_status);
   	wakeup(&dev_priv->wait_queue);
      	clear_bit(MGA_BUF_SWAP_PENDING, &last_prim_buffer->buffer_status);
      	clear_bit(MGA_IN_DISPATCH, &dev_priv->dispatch_status);
      	atomic_dec(&dev_priv->pending_bufs);
	taskqueue_enqueue(taskqueue_swi, &dev->task);
}

static void mga_dma_task_queue(void *device, int pending)
{
	DRM_DEBUG("%s\n", __FUNCTION__);
	mga_dma_schedule((drm_device_t *)device, 0);
}

int mga_dma_cleanup(drm_device_t *dev)
{
	DRM_DEBUG("%s\n", __FUNCTION__);

	if(dev->dev_private) {
		drm_mga_private_t *dev_priv = 
			(drm_mga_private_t *) dev->dev_private;
      
		if(dev_priv->ioremap) {
			int temp = (dev_priv->warp_ucode_size + 
				    dev_priv->primary_size + 
				    PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;

			drm_ioremapfree((void *) dev_priv->ioremap, temp);
		}
	   	if(dev_priv->real_status_page != 0UL) {
		   	mga_free_page(dev, dev_priv->real_status_page);
		}
	   	if(dev_priv->prim_bufs != NULL) {
		   	int i;
		   	for(i = 0; i < MGA_NUM_PRIM_BUFS; i++) {
			   	if(dev_priv->prim_bufs[i] != NULL) {
			     		drm_free(dev_priv->prim_bufs[i],
						 sizeof(drm_mga_prim_buf_t),
						 DRM_MEM_DRIVER);
				}
			}
		   	drm_free(dev_priv->prim_bufs, sizeof(void *) *
				 (MGA_NUM_PRIM_BUFS + 1), 
				 DRM_MEM_DRIVER);
		}
		if(dev_priv->head != NULL) {
		   	mga_freelist_cleanup(dev);
		}


		drm_free(dev->dev_private, sizeof(drm_mga_private_t), 
			 DRM_MEM_DRIVER);
		dev->dev_private = NULL;
	}

	return 0;
}

static int mga_dma_initialize(drm_device_t *dev, drm_mga_init_t *init) {
	drm_mga_private_t *dev_priv;
	drm_map_t *sarea_map = NULL;
	int i;

	DRM_DEBUG("%s\n", __FUNCTION__);

	dev_priv = drm_alloc(sizeof(drm_mga_private_t), DRM_MEM_DRIVER);
	if(dev_priv == NULL) return ENOMEM;
	dev->dev_private = (void *) dev_priv;

	memset(dev_priv, 0, sizeof(drm_mga_private_t));

	if((init->reserved_map_idx >= dev->map_count) ||
	   (init->buffer_map_idx >= dev->map_count)) {
		mga_dma_cleanup(dev);
		DRM_DEBUG("reserved_map or buffer_map are invalid\n");
		return EINVAL;
	}
   
	dev_priv->reserved_map_idx = init->reserved_map_idx;
	dev_priv->buffer_map_idx = init->buffer_map_idx;
	sarea_map = dev->maplist[0];
	dev_priv->sarea_priv = (drm_mga_sarea_t *) 
		((u_int8_t *)sarea_map->handle + 
		 init->sarea_priv_offset);

	/* Scale primary size to the next page */
	dev_priv->chipset = init->chipset;
	dev_priv->frontOffset = init->frontOffset;
	dev_priv->backOffset = init->backOffset;
	dev_priv->depthOffset = init->depthOffset;
	dev_priv->textureOffset = init->textureOffset;
	dev_priv->textureSize = init->textureSize;
	dev_priv->cpp = init->cpp;
	dev_priv->sgram = init->sgram;
	dev_priv->stride = init->stride;

	dev_priv->mAccess = init->mAccess;
   	dev_priv->flush_queue = 0;
	dev_priv->buf_queue = 0;
	dev_priv->WarpPipe = -1;

   	DRM_DEBUG("chipset: %d ucode_size: %d backOffset: %x depthOffset: %x\n",
		  dev_priv->chipset, dev_priv->warp_ucode_size, 
		  dev_priv->backOffset, dev_priv->depthOffset);
   	DRM_DEBUG("cpp: %d sgram: %d stride: %d maccess: %x\n",
		  dev_priv->cpp, dev_priv->sgram, dev_priv->stride, 
		  dev_priv->mAccess);
   
	memcpy(&dev_priv->WarpIndex, &init->WarpIndex, 
	       sizeof(drm_mga_warp_index_t) * MGA_MAX_WARP_PIPES);

   	for (i = 0 ; i < MGA_MAX_WARP_PIPES ; i++) 
		DRM_DEBUG("warp pipe %d: installed: %d phys: %lx size: %x\n",
			  i, 
			  dev_priv->WarpIndex[i].installed,
			  dev_priv->WarpIndex[i].phys_addr,
			  dev_priv->WarpIndex[i].size);

   	if(mga_init_primary_bufs(dev, init) != 0) {
		DRM_ERROR("Can not initialize primary buffers\n");
		mga_dma_cleanup(dev);
		return ENOMEM;
	}
   	dev_priv->real_status_page = mga_alloc_page(dev);
      	if(dev_priv->real_status_page == 0UL) {
		mga_dma_cleanup(dev);
		DRM_ERROR("Can not allocate status page\n");
		return ENOMEM;
	}

   	dev_priv->status_page = (void*)dev_priv->real_status_page; /* XXX wants nocache */
#if 0
   	dev_priv->status_page = 
		ioremap_nocache(virt_to_bus((void *)dev_priv->real_status_page),
				PAGE_SIZE);

   	if(dev_priv->status_page == NULL) {
		mga_dma_cleanup(dev);
		DRM_ERROR("Can not remap status page\n");
		return ENOMEM;
	}
#endif

   	/* Write status page when secend or softrap occurs */
   	MGA_WRITE(MGAREG_PRIMPTR, 
		  vtophys((void *)dev_priv->real_status_page) | 0x00000003);
      

	/* Private is now filled in, initialize the hardware */
	{
		PRIMLOCALS;
		PRIMGETPTR( dev_priv );
	   	   
		PRIMOUTREG(MGAREG_DMAPAD, 0);
		PRIMOUTREG(MGAREG_DMAPAD, 0);
		PRIMOUTREG(MGAREG_DWGSYNC, 0x0100);
		PRIMOUTREG(MGAREG_SOFTRAP, 0);
		/* Poll for the first buffer to insure that
		 * the status register will be correct
		 */
	   
		mga_flush_write_combine();
	   	MGA_WRITE(MGAREG_PRIMADDRESS, phys_head | TT_GENERAL);

		MGA_WRITE(MGAREG_PRIMEND, ((phys_head + num_dwords * 4) | 
					   PDEA_pagpxfer_enable));
	   
	   	while(MGA_READ(MGAREG_DWGSYNC) != 0x0100) ;
	}

	if(mga_freelist_init(dev) != 0) {
	   	DRM_ERROR("Could not initialize freelist\n");
	   	mga_dma_cleanup(dev);
	   	return ENOMEM;
	}
	return 0;
}

int
mga_dma_init(dev_t kdev, u_long cmd, caddr_t data, int flags, struct proc *p)
{
        drm_device_t *dev = kdev->si_drv1;
	drm_mga_init_t init;
   
   	DRM_DEBUG("%s\n", __FUNCTION__);

	init = *(drm_mga_init_t *) data;
   
	switch(init.func) {
	case MGA_INIT_DMA:
		return mga_dma_initialize(dev, &init);
	case MGA_CLEANUP_DMA:
		return mga_dma_cleanup(dev);
	}

	return EINVAL;
}

int mga_irq_install(drm_device_t *dev, int irq)
{
	int rid;
	int retcode;

	if (!irq)     return EINVAL;
	
	lockmgr(&dev->dev_lock, LK_EXCLUSIVE, 0, curproc);
	if (dev->irq) {
		lockmgr(&dev->dev_lock, LK_RELEASE, 0, curproc);
		return EBUSY;
	}
	lockmgr(&dev->dev_lock, LK_RELEASE, 0, curproc);
	
	DRM_DEBUG("install irq handler %d\n", irq);

	dev->context_flag     = 0;
	dev->interrupt_flag   = 0;
	dev->dma_flag	      = 0;
	dev->dma->next_buffer = NULL;
	dev->dma->next_queue  = NULL;
	dev->dma->this_buffer = NULL;
	TASK_INIT(&dev->task, 0, mga_dma_task_queue, dev);

				/* Before installing handler */
	MGA_WRITE(MGAREG_IEN, 0);
   				/* Install handler */
	rid = 0;
	dev->irq = bus_alloc_resource(dev->device, SYS_RES_IRQ, &rid,
				      0, ~0, 1, RF_SHAREABLE);
	if (!dev->irq)
		return ENOENT;

	retcode = bus_setup_intr(dev->device, dev->irq, INTR_TYPE_TTY,
				 mga_dma_service, dev, &dev->irqh);
	if (retcode) {
		bus_release_resource(dev->device, SYS_RES_IRQ, 0, dev->irq);
		dev->irq = 0;
		return retcode;
	}

				/* After installing handler */
   	MGA_WRITE(MGAREG_ICLEAR, 0x00000001);
	MGA_WRITE(MGAREG_IEN, 0x00000001);
	return 0;
}

int mga_irq_uninstall(drm_device_t *dev)
{
	if (!dev->irq)
		return EINVAL;
	
   	DRM_DEBUG("remove irq handler %ld\n", rman_get_start(dev->irq));
      	MGA_WRITE(MGAREG_ICLEAR, 0x00000001);
	MGA_WRITE(MGAREG_IEN, 0);

	bus_teardown_intr(dev->device, dev->irq, dev->irqh);
	bus_release_resource(dev->device, SYS_RES_IRQ, 0, dev->irq);
	dev->irq = 0;

	return 0;
}

int mga_control(dev_t kdev, u_long cmd, caddr_t data,
		int flags, struct proc *p)
{
	drm_device_t	*dev	= kdev->si_drv1;
	drm_control_t	ctl;
   
	ctl = *(drm_control_t *) data;

   	DRM_DEBUG("%s\n", __FUNCTION__);

	switch (ctl.func) {
	case DRM_INST_HANDLER:
		return mga_irq_install(dev, ctl.irq);
	case DRM_UNINST_HANDLER:
		return mga_irq_uninstall(dev);
	default:
		return EINVAL;
	}
}

static int mga_flush_queue(drm_device_t *dev)
{
  	drm_mga_private_t *dev_priv = (drm_mga_private_t *)dev->dev_private;
   	int ret = 0;
	int s;

   	DRM_DEBUG("%s\n", __FUNCTION__);

   	if(dev_priv == NULL) {
	   	return 0;
	}
   
   	if(dev_priv->next_prim->num_dwords != 0) {
		s = splsofttq();
		set_bit(MGA_IN_FLUSH, &dev_priv->dispatch_status);
   		for (;;) {
		   	mga_dma_schedule(dev, 0);
	   		if (!test_bit(MGA_IN_FLUSH, 
				      &dev_priv->dispatch_status)) 
				break;
		   	atomic_inc(&dev->total_sleeps);
			ret = tsleep(&dev_priv->flush_queue, PZERO|PCATCH,
				     "mgafq", 0);
			if (ret) {
				clear_bit(MGA_IN_FLUSH, 
					  &dev_priv->dispatch_status);
		   		break;
			}
		}
		splx(s);
	}
   	return ret;
}

/* Must be called with the lock held */
void mga_reclaim_buffers(drm_device_t *dev, pid_t pid)
{
	drm_device_dma_t *dma = dev->dma;
	int		 i;

	if (!dma) return;
      	if(dev->dev_private == NULL) return;
	if(dma->buflist == NULL) return;

	DRM_DEBUG("%s\n", __FUNCTION__);
        mga_flush_queue(dev);

	for (i = 0; i < dma->buf_count; i++) {
	   	drm_buf_t *buf = dma->buflist[ i ];
	   	drm_mga_buf_priv_t *buf_priv = buf->dev_private;

		/* Only buffers that need to get reclaimed ever 
		 * get set to free 
		 */
		if (buf->pid == pid  && buf_priv) {
			if(buf_priv->my_freelist->age == MGA_BUF_USED) 
		     		buf_priv->my_freelist->age = MGA_BUF_FREE;
		}
	}
}

int mga_lock(dev_t kdev, u_long cmd, caddr_t data,
	     int flags, struct proc *p)
{
	drm_device_t	  *dev	  = kdev->si_drv1;
	int		  ret	= 0;
	drm_lock_t	  lock;

	DRM_DEBUG("%s\n", __FUNCTION__);
	lock = *(drm_lock_t *) data;

	if (lock.context == DRM_KERNEL_CONTEXT) {
		DRM_ERROR("Process %d using kernel context %d\n",
			  p->p_pid, lock.context);
		return EINVAL;
	}
   
   	DRM_DEBUG("%d (pid %d) requests lock (0x%08x), flags = 0x%08x\n",
	       lock.context, p->p_pid, dev->lock.hw_lock->lock,
	       lock.flags);

	if (lock.context < 0) {
		return EINVAL;
	}
   
	/* Only one queue:
	 */

	if (!ret) {
		atomic_inc(&dev->lock.lock_queue);
		for (;;) {
			if (!dev->lock.hw_lock) {
				/* Device has been unregistered */
				ret = EINTR;
				break;
			}
			if (drm_lock_take(&dev->lock.hw_lock->lock,
					  lock.context)) {
				dev->lock.pid	    = p->p_pid;
				dev->lock.lock_time = ticks;
				atomic_inc(&dev->total_locks);
				break;	/* Got lock */
			}
			
				/* Contention */
			atomic_inc(&dev->total_sleeps);
			ret = tsleep(&dev->lock.lock_queue, PZERO|PCATCH,
					 "mgal2", 0);
			if (ret)
				break;
		}
		atomic_dec(&dev->lock.lock_queue);
	}
	
	if (!ret) {
		if (lock.flags & _DRM_LOCK_QUIESCENT) {
		   DRM_DEBUG("_DRM_LOCK_QUIESCENT\n");
		   mga_flush_queue(dev);
		   mga_dma_quiescent(dev);
		}
	}
   
	DRM_DEBUG("%d %s\n", lock.context, ret ? "interrupted" : "has lock");
	return ret;
}
		
int mga_flush_ioctl(dev_t kdev, u_long cmd, caddr_t data,
		    int flags, struct proc *p)
{
	drm_device_t	  *dev	  = kdev->si_drv1;
	drm_lock_t	  lock;
      	drm_mga_private_t *dev_priv = (drm_mga_private_t *)dev->dev_private;
	int		  s;

   	DRM_DEBUG("%s\n", __FUNCTION__);
	lock = *(drm_lock_t *) data;

	if(!_DRM_LOCK_IS_HELD(dev->lock.hw_lock->lock)) {
		DRM_ERROR("mga_flush_ioctl called without lock held\n");
		return EINVAL;
	}

   	if(lock.flags & _DRM_LOCK_FLUSH || lock.flags & _DRM_LOCK_FLUSH_ALL) {
		drm_mga_prim_buf_t *temp_buf =
			dev_priv->prim_bufs[dev_priv->current_prim_idx];

		if(temp_buf && temp_buf->num_dwords) {
			s = splsofttq();
			set_bit(MGA_BUF_FORCE_FIRE, &temp_buf->buffer_status);
			mga_advance_primary(dev);
			mga_dma_schedule(dev, 1);
			splx(s);
 		}
	}
   	if(lock.flags & _DRM_LOCK_QUIESCENT) {
		mga_flush_queue(dev);
		mga_dma_quiescent(dev);
	}

    	return 0;
}
