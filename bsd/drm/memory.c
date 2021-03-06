/* memory.c -- Memory management wrappers for DRM -*- c -*-
 * Created: Thu Feb  4 14:00:34 1999 by faith@precisioninsight.com
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
 * Authors:
 *    Rickard E. (Rik) Faith <faith@valinux.com>
 *
 */

#define __NO_VERSION__
#include "drmP.h"

#include <vm/vm.h>
#include <vm/pmap.h>
#ifdef DRM_AGP
#include <sys/agpio.h>
#endif

MALLOC_DEFINE(M_DRM, "drm", "DRM Data Structures");

typedef struct drm_mem_stats {
	const char	  *name;
	int		  succeed_count;
	int		  free_count;
	int		  fail_count;
	unsigned long	  bytes_allocated;
	unsigned long	  bytes_freed;
} drm_mem_stats_t;

#ifdef SMP
static struct simplelock  drm_mem_lock;
#endif
static unsigned long	  drm_ram_available = 0;
static unsigned long	  drm_ram_used	    = 0;
static drm_mem_stats_t	  drm_mem_stats[]   = {
	[DRM_MEM_DMA]	    = { "dmabufs"  },
	[DRM_MEM_SAREA]	    = { "sareas"   },
	[DRM_MEM_DRIVER]    = { "driver"   },
	[DRM_MEM_MAGIC]	    = { "magic"	   },
	[DRM_MEM_IOCTLS]    = { "ioctltab" },
	[DRM_MEM_MAPS]	    = { "maplist"  },
	[DRM_MEM_VMAS]	    = { "vmalist"  },
	[DRM_MEM_BUFS]	    = { "buflist"  },
	[DRM_MEM_SEGS]	    = { "seglist"  },
	[DRM_MEM_PAGES]	    = { "pagelist" },
	[DRM_MEM_FILES]	    = { "files"	   },
	[DRM_MEM_QUEUES]    = { "queues"   },
	[DRM_MEM_CMDS]	    = { "commands" },
	[DRM_MEM_MAPPINGS]  = { "mappings" },
	[DRM_MEM_BUFLISTS]  = { "buflists" },
	[DRM_MEM_AGPLISTS]  = { "agplist"  },
	[DRM_MEM_TOTALAGP]  = { "totalagp" },
	[DRM_MEM_BOUNDAGP]  = { "boundagp" },
	[DRM_MEM_CTXBITMAP] = { "ctxbitmap"},
	{ NULL, 0, }		/* Last entry must be null */
};

void drm_mem_init(void)
{
	drm_mem_stats_t *mem;
	
	for (mem = drm_mem_stats; mem->name; ++mem) {
		mem->succeed_count   = 0;
		mem->free_count	     = 0;
		mem->fail_count	     = 0;
		mem->bytes_allocated = 0;
		mem->bytes_freed     = 0;
	}
	
	drm_ram_available = 0; /* si.totalram; */
	drm_ram_used	  = 0;
}

/* drm_mem_info is called whenever a process reads /dev/drm/mem. */

static int _drm_mem_info SYSCTL_HANDLER_ARGS
{
	drm_mem_stats_t *pt;
	char buf[128];
	int error;

	DRM_SYSCTL_PRINT("		  total counts			"
		       " |    outstanding  \n");
	DRM_SYSCTL_PRINT("type	   alloc freed fail	bytes	   freed"
		       " | allocs      bytes\n\n");
	DRM_SYSCTL_PRINT("%-9.9s %5d %5d %4d %10lu	    |\n",
		       "system", 0, 0, 0, drm_ram_available);
	DRM_SYSCTL_PRINT("%-9.9s %5d %5d %4d %10lu	    |\n",
		       "locked", 0, 0, 0, drm_ram_used);
	DRM_SYSCTL_PRINT("\n");
	for (pt = drm_mem_stats; pt->name; pt++) {
		DRM_SYSCTL_PRINT("%-9.9s %5d %5d %4d %10lu %10lu | %6d %10ld\n",
			       pt->name,
			       pt->succeed_count,
			       pt->free_count,
			       pt->fail_count,
			       pt->bytes_allocated,
			       pt->bytes_freed,
			       pt->succeed_count - pt->free_count,
			       (long)pt->bytes_allocated
			       - (long)pt->bytes_freed);
	}
	SYSCTL_OUT(req, "", 1);
	
	return 0;
}

int drm_mem_info SYSCTL_HANDLER_ARGS
{
	int ret;
	
	simple_lock(&drm_mem_lock);
	ret = _drm_mem_info(oidp, arg1, arg2, req);
	simple_unlock(&drm_mem_lock);
	return ret;
}

void *drm_alloc(size_t size, int area)
{
	void *pt;
	
	if (!size) {
		DRM_MEM_ERROR(area, "Allocating 0 bytes\n");
		return NULL;
	}
	
	if (!(pt = malloc(size, M_DRM, M_NOWAIT))) {
		simple_lock(&drm_mem_lock);
		++drm_mem_stats[area].fail_count;
		simple_unlock(&drm_mem_lock);
		return NULL;
	}
	simple_lock(&drm_mem_lock);
	++drm_mem_stats[area].succeed_count;
	drm_mem_stats[area].bytes_allocated += size;
	simple_unlock(&drm_mem_lock);
	return pt;
}

void *drm_realloc(void *oldpt, size_t oldsize, size_t size, int area)
{
	void *pt;
	
	if (!(pt = drm_alloc(size, area))) return NULL;
	if (oldpt && oldsize) {
		memcpy(pt, oldpt, oldsize);
		drm_free(oldpt, oldsize, area);
	}
	return pt;
}

char *drm_strdup(const char *s, int area)
{
	char *pt;
	int	 length = s ? strlen(s) : 0;
	
	if (!(pt = drm_alloc(length+1, area))) return NULL;
	strcpy(pt, s);
	return pt;
}

void drm_strfree(char *s, int area)
{
	unsigned int size;
	
	if (!s) return;
	
	size = 1 + (s ? strlen(s) : 0);
	drm_free((void *)s, size, area);
}

void drm_free(void *pt, size_t size, int area)
{
	int alloc_count;
	int free_count;
	
	if (!pt) DRM_MEM_ERROR(area, "Attempt to free NULL pointer\n");
	else	 free(pt, M_DRM);
	simple_lock(&drm_mem_lock);
	drm_mem_stats[area].bytes_freed += size;
	free_count  = ++drm_mem_stats[area].free_count;
	alloc_count =	drm_mem_stats[area].succeed_count;
	simple_unlock(&drm_mem_lock);
	if (free_count > alloc_count) {
		DRM_MEM_ERROR(area, "Excess frees: %d frees, %d allocs\n",
			      free_count, alloc_count);
	}
}

unsigned long drm_alloc_pages(int order, int area)
{
	vm_offset_t address;
	unsigned long bytes	  = PAGE_SIZE << order;
	unsigned long addr;
	unsigned int  sz;
	
	simple_lock(&drm_mem_lock);
	if (drm_ram_used > +(DRM_RAM_PERCENT * drm_ram_available) / 100) {
		simple_unlock(&drm_mem_lock);
		return 0;
	}
	simple_unlock(&drm_mem_lock);
	
	address = (vm_offset_t) contigmalloc(1<<order, M_DRM, M_WAITOK, 0, ~0, 1, 0);
	if (!address) {
		simple_lock(&drm_mem_lock);
		++drm_mem_stats[area].fail_count;
		simple_unlock(&drm_mem_lock);
		return 0;
	}
	simple_lock(&drm_mem_lock);
	++drm_mem_stats[area].succeed_count;
	drm_mem_stats[area].bytes_allocated += bytes;
	drm_ram_used		            += bytes;
	simple_unlock(&drm_mem_lock);
	
	
				/* Zero outside the lock */
	memset((void *)address, 0, bytes);
	
				/* Reserve */
	for (addr = address, sz = bytes;
	     sz > 0;
	     addr += PAGE_SIZE, sz -= PAGE_SIZE) {
		/* mem_map_reserve(MAP_NR(addr));*/
	}
	
	return address;
}

void drm_free_pages(unsigned long address, int order, int area)
{
	unsigned long bytes = PAGE_SIZE << order;
	int		  alloc_count;
	int		  free_count;
	unsigned long addr;
	unsigned int  sz;
	
	if (!address) {
		DRM_MEM_ERROR(area, "Attempt to free address 0\n");
	} else {
				/* Unreserve */
		for (addr = address, sz = bytes;
		     sz > 0;
		     addr += PAGE_SIZE, sz -= PAGE_SIZE) {
			/* mem_map_unreserve(MAP_NR(addr));*/
		}
		contigfree((void *) address, bytes, M_DRM);
	}
	
	simple_lock(&drm_mem_lock);
	free_count  = ++drm_mem_stats[area].free_count;
	alloc_count =	drm_mem_stats[area].succeed_count;
	drm_mem_stats[area].bytes_freed += bytes;
	drm_ram_used			-= bytes;
	simple_unlock(&drm_mem_lock);
	if (free_count > alloc_count) {
		DRM_MEM_ERROR(area,
			      "Excess frees: %d frees, %d allocs\n",
			      free_count, alloc_count);
	}
}

void *drm_ioremap(unsigned long offset, unsigned long size)
{
	void *pt;
	
	if (!size) {
		DRM_MEM_ERROR(DRM_MEM_MAPPINGS,
			      "Mapping 0 bytes at 0x%08lx\n", offset);
		return NULL;
	}
	
	if (!(pt = pmap_mapdev(offset, size))) {
		simple_lock(&drm_mem_lock);
		++drm_mem_stats[DRM_MEM_MAPPINGS].fail_count;
		simple_unlock(&drm_mem_lock);
		return NULL;
	}
	simple_lock(&drm_mem_lock);
	++drm_mem_stats[DRM_MEM_MAPPINGS].succeed_count;
	drm_mem_stats[DRM_MEM_MAPPINGS].bytes_allocated += size;
	simple_unlock(&drm_mem_lock);
	return pt;
}

void drm_ioremapfree(void *pt, unsigned long size)
{
	int alloc_count;
	int free_count;
	
	if (!pt)
		DRM_MEM_ERROR(DRM_MEM_MAPPINGS,
			      "Attempt to free NULL pointer\n");
	else
		pmap_unmapdev((vm_offset_t) pt, size);
	
	simple_lock(&drm_mem_lock);
	drm_mem_stats[DRM_MEM_MAPPINGS].bytes_freed += size;
	free_count  = ++drm_mem_stats[DRM_MEM_MAPPINGS].free_count;
	alloc_count =	drm_mem_stats[DRM_MEM_MAPPINGS].succeed_count;
	simple_unlock(&drm_mem_lock);
	if (free_count > alloc_count) {
		DRM_MEM_ERROR(DRM_MEM_MAPPINGS,
			      "Excess frees: %d frees, %d allocs\n",
			      free_count, alloc_count);
	}
}

#ifdef DRM_AGP
void *drm_alloc_agp(int pages, u_int32_t type)
{
	device_t dev = agp_find_device();
	void *handle;

	if (!dev)
		return NULL;

	if (!pages) {
		DRM_MEM_ERROR(DRM_MEM_TOTALAGP, "Allocating 0 pages\n");
		return NULL;
	}
	
	if ((handle = agp_alloc_memory(dev, type, pages << AGP_PAGE_SHIFT))) {
		simple_lock(&drm_mem_lock);
		++drm_mem_stats[DRM_MEM_TOTALAGP].succeed_count;
		drm_mem_stats[DRM_MEM_TOTALAGP].bytes_allocated
			+= pages << PAGE_SHIFT;
		simple_unlock(&drm_mem_lock);
		return handle;
	}
	simple_lock(&drm_mem_lock);
	++drm_mem_stats[DRM_MEM_TOTALAGP].fail_count;
	simple_unlock(&drm_mem_lock);
	return NULL;
}

int drm_free_agp(void *handle, int pages)
{
	device_t dev = agp_find_device();
	int           alloc_count;
	int           free_count;
	int           retval = EINVAL;

	if (!dev)
		return EINVAL;

	if (!handle) {
		DRM_MEM_ERROR(DRM_MEM_TOTALAGP,
			      "Attempt to free NULL AGP handle\n");
		return retval;
	}
	
	agp_free_memory(dev, handle);
	simple_lock(&drm_mem_lock);
	free_count  = ++drm_mem_stats[DRM_MEM_TOTALAGP].free_count;
	alloc_count =   drm_mem_stats[DRM_MEM_TOTALAGP].succeed_count;
	drm_mem_stats[DRM_MEM_TOTALAGP].bytes_freed
		+= pages << PAGE_SHIFT;
	simple_unlock(&drm_mem_lock);
	if (free_count > alloc_count) {
		DRM_MEM_ERROR(DRM_MEM_TOTALAGP,
			      "Excess frees: %d frees, %d allocs\n",
			      free_count, alloc_count);
	}
	return 0;
}

int drm_bind_agp(void *handle, unsigned int start)
{
	device_t dev = agp_find_device();
	int retcode  = EINVAL;
	struct agp_memory_info info;

	DRM_DEBUG("drm_bind_agp called\n");

	if (!dev)
		return EINVAL;

	if (!handle) {
		DRM_MEM_ERROR(DRM_MEM_BOUNDAGP,
			      "Attempt to bind NULL AGP handle\n");
		return retcode;
	}

	if (!(retcode = agp_bind_memory(dev, handle,
					start << AGP_PAGE_SHIFT))) {
		simple_lock(&drm_mem_lock);
		++drm_mem_stats[DRM_MEM_BOUNDAGP].succeed_count;
		agp_memory_info(dev, handle, &info);
		drm_mem_stats[DRM_MEM_BOUNDAGP].bytes_allocated
			+= info.ami_size;
		simple_unlock(&drm_mem_lock);
		DRM_DEBUG("drm_agp.bind_memory: retcode %d\n", retcode);
		return retcode;
	}
	simple_lock(&drm_mem_lock);
	++drm_mem_stats[DRM_MEM_BOUNDAGP].fail_count;
	simple_unlock(&drm_mem_lock);
	return retcode;
}

int drm_unbind_agp(void *handle)
{
	device_t dev = agp_find_device();
	int alloc_count;
	int free_count;
	int retcode = EINVAL;
	struct agp_memory_info info;
	
	if (!dev)
		return EINVAL;

	if (!handle) {
		DRM_MEM_ERROR(DRM_MEM_BOUNDAGP,
			      "Attempt to unbind NULL AGP handle\n");
		return retcode;
	}

	
	agp_memory_info(dev, handle, &info);
	if ((retcode = agp_unbind_memory(dev, handle)))
		return retcode;
	simple_lock(&drm_mem_lock);
	free_count  = ++drm_mem_stats[DRM_MEM_BOUNDAGP].free_count;
	alloc_count = drm_mem_stats[DRM_MEM_BOUNDAGP].succeed_count;
	drm_mem_stats[DRM_MEM_BOUNDAGP].bytes_freed += info.ami_size;
	simple_unlock(&drm_mem_lock);
	if (free_count > alloc_count) {
		DRM_MEM_ERROR(DRM_MEM_BOUNDAGP,
			      "Excess frees: %d frees, %d allocs\n",
			      free_count, alloc_count);
	}
	return retcode;
}
#endif
