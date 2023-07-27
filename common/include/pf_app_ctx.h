#ifndef pf_app_ctx_h__
#define pf_app_ctx_h__
#include <string>

#include "pf_buffer.h"
#include "pf_mempool.h"

#include "pf_client_api.h"

enum {
	AIO,
	IO_URING,
	SPDK,
};

class BufferDescriptor;
class PfIoDesc
{
public:
	BufferDescriptor* io_cmd;
	BufferDescriptor* io_data;
	BufferDescriptor* io_reply;
};
class PfAppCtx
{
public:
	int io_desc_count;
	ObjectMemoryPool<PfIoDesc> iod_pool;

	//BufferPool cmd_pool;
	//BufferPool data_pool;
	//BufferPool reply_pool;
	//BufferPool handshake_pool;

	//ObjectMemoryPool<PfMessageHead> cmd_pool;
	//ObjectMemoryPool<byte[64 << 10]> data_pool;
	//ObjectMemoryPool<pf_io_reply> reply_pool;
	//ObjectMemoryPool<pf_handshake_msg> handshake_pool;

	std::string conf_file_name;
	conf_file_t conf;
	int engine;
	PfAppCtx():engine(AIO){}
};

extern PfAppCtx* g_app_ctx;
#endif // pf_app_ctx_h__
