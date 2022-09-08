/*
 * Copyright (C) 2016 Liu Lele(liu_lele@126.com)
 *
 * This code is licensed under the GPL.
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <malloc.h>
#include "pf_md5.h"
#include "basetype.h"
#include "pf_main.h"

MD5Stream::MD5Stream(int fd)
{
	this->fd=fd;
	buffer = NULL;
	spdk_engine = false;
	reset(0);
}

MD5Stream::MD5Stream(struct ns_entry *ns, PfspdkEngine *eng)
{
	this->nvme.ns = ns;
	this->nvme.ioengine = eng;
	buffer = NULL;
	spdk_engine = true;
	reset(0);
}

MD5Stream::~MD5Stream()
{
	if (spdk_engine)
		spdk_dma_free(buffer);
	else
		free(buffer);
}

int MD5Stream::init()
{
	int rc =0;
	if (spdk_engine)
		buffer = spdk_dma_zmalloc(LBA_LENGTH, LBA_LENGTH, NULL);
	else 
		buffer  = (char*)aligned_alloc(LBA_LENGTH, LBA_LENGTH);

	if (buffer == NULL)
	{
		S5LOG_ERROR("Failed to allocate memory for MD5Stream");
		return -ENOMEM;
	}
	return rc;
}

void MD5Stream::reset(off_t offset)
{
	base_offset=offset;
}


int MD5Stream::read(void *buf, size_t count, off_t offset)
{
	int rc;

	if (app_context.engine == SPDK) {
		if ((rc = nvme.ioengine->spdk_nvme_read(buf, count, offset)) ! = 0)
			return rc;
		return 0;
	}
	if (-1 == pread(fd, buf, count, base_offset + offset))
		return -errno;
	return 0;
}

int MD5Stream::write(void *buf, size_t count, off_t offset)
{
	int rc;

	if (spdk_engine) {
		if ((rc = nvme.ioengine->spdk_nvme_write(buf, count, offset)) ! = 0)
			return rc;
		return 0;
	}
	if (-1 == pwrite(fd, buf, count, base_offset + offset))
		return -errno;
	return 0;
}

int MD5Stream::finalize(off_t offset, int in_read)
{

	return  0;

}
