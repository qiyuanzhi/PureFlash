/*
 * Copyright (C) 2016 Liu Lele(liu_lele@126.com)
 *
 * This code is licensed under the GPL.
 */
#ifndef pf_dispatcher_h__
#define pf_dispatcher_h__

#include <unordered_map>
#include <libaio.h>
#include "pf_event_thread.h"
#include "pf_connection.h"
#include "pf_mempool.h"
#include "pf_volume.h"
#include "pf_message.h"
#ifdef WITH_RDMA
#include <rdma/rdma_cma.h>
#include "pf_rdma_connection.h"
#endif
#include <netdb.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

#define PF_MAX_SUBTASK_CNT 5 //1 local, 2 sync rep, 1 remote replicating, 1 rebalance
struct PfServerIocb;
class PfFlashStore;
struct lmt_entry;
struct lmt_key;

struct SubTask
{
	PfOpCode opcode;
	//NOTE: any added member should be initialized either in PfDispatcher::init_mempools, or in PfServerIocb::setup_subtask
	PfServerIocb* parent_iocb;
	uint64_t rep_id;
	uint64_t store_id;
	uint32_t task_mask;
	uint32_t rep_index; //task_mask = 1 << rep_index;
	PfMessageStatus complete_status;
	virtual void complete(PfMessageStatus comp_status);
	virtual void complete(PfMessageStatus comp_status, uint16_t meta_ver);
	virtual PfEventQueue* half_complete(PfMessageStatus comp_status);

	SubTask():opcode(PfOpCode(0)), parent_iocb(NULL), task_mask(0), rep_index(0), complete_status((PfMessageStatus)0){}
};

struct IoSubTask : public SubTask
{
#pragma warning("Use union here is beter")
	struct iovec uring_iov;
	iocb aio_cb; //aio cb to perform io
	IoSubTask* next;//used for chain waiting io
    inline void complete_read_with_zero();

	IoSubTask():next(NULL) {}
};


struct RecoverySubTask : public SubTask
{
	BufferDescriptor *recovery_bd;
	uint64_t volume_id;
	int64_t offset;
	int64_t length;
	uint32_t  snap_seq;
	uint16_t meta_ver;
	sem_t* sem;
	ObjectMemoryPool<RecoverySubTask>* owner_queue;

	RecoverySubTask() : recovery_bd(NULL), volume_id(0), offset(0), length(0), snap_seq(0), sem(NULL){}
	virtual void complete(PfMessageStatus comp_status);
	virtual void complete(PfMessageStatus comp_status, uint16_t meta_ver);
};
struct CowTask : public IoSubTask {
	off_t src_offset;
	off_t dst_offset;
	void* buf;
	int size;
	sem_t sem;
};

struct PfServerIocb
{
public:
	BufferDescriptor* cmd_bd;
	BufferDescriptor* data_bd;
	BufferDescriptor* reply_bd; //Used by dispatcher tasks

	PfConnection *conn;
	PfVolume* vol;
	PfMessageStatus complete_status;
	uint16_t  complete_meta_ver;
	uint32_t task_mask;
	uint32_t ref_count;
	int disp_index;
	BOOL is_timeout;


	SubTask* subtasks[PF_MAX_SUBTASK_CNT];
	IoSubTask io_subtasks[3];
	uint64_t received_time;

	void inline setup_subtask(PfShard* s, PfOpCode opcode)
	{
		for (int i = 0; i < s->rep_count; i++) {
			if(s->replicas[i]->status == HS_OK || s->replicas[i]->status == HS_RECOVERYING) {
				subtasks[i]->complete_status=PfMessageStatus::MSG_STATUS_SUCCESS;
				subtasks[i]->opcode = opcode;  //subtask opcode will be OP_WRITE or OP_REPLICATE_WRITE
				task_mask |= subtasks[i]->task_mask;
				add_ref();
			}
		}
	}
	void inline setup_one_subtask(PfShard* s, int rep_index, PfOpCode opcode)
	{
		subtasks[rep_index]->complete_status=PfMessageStatus::MSG_STATUS_SUCCESS;
		subtasks[rep_index]->opcode = opcode;
		task_mask |= subtasks[rep_index]->task_mask;
		add_ref();
	}

	inline void add_ref() { __sync_fetch_and_add(&ref_count, 1); }
    inline void dec_ref();

};

class PfDispatcher : public PfEventThread
{
public:
	ObjectMemoryPool<PfServerIocb> iocb_pool;
	struct disp_mem_pool mem_pool;
	std::unordered_map<uint64_t, PfVolume*> opened_volumes;
	int disp_index;

	//PfDispatcher(const std::string &name);
	int prepare_volume(PfVolume* vol);
	int dispatch_io(PfServerIocb *iocb);
	int dispatch_complete(SubTask*);
	virtual int process_event(int event_type, int arg_i, void* arg_p);

	int init(int disp_idx);
	int init_mempools(int disp_idx);

	int dispatch_write(PfServerIocb* iocb, PfVolume* vol, PfShard * s);
	int dispatch_read(PfServerIocb* iocb, PfVolume* vol, PfShard * s);
	int dispatch_rep_write(PfServerIocb* iocb, PfVolume* vol, PfShard * s);

	void set_snap_seq(int64_t volume_id, int snap_seq);
	int set_meta_ver(int64_t volume_id, int meta_ver);
	int prepare_shards(PfVolume* vol);
};

inline void PfServerIocb::dec_ref() {
    if (__sync_sub_and_fetch(&ref_count, 1) == 0) {
//    	S5LOG_DEBUG("Iocb released:%p", this);
	    complete_meta_ver=0;
	    complete_status = MSG_STATUS_SUCCESS;
	    vol = NULL;
	    is_timeout = FALSE;
	    task_mask = 0;
        conn->dispatcher->iocb_pool.free(this);
	    conn->dec_ref();
	    conn = NULL;
    }
}

inline void SubTask::complete(PfMessageStatus comp_status){
    complete_status = comp_status;
    parent_iocb->conn->dispatcher->event_queue.post_event(EVT_IO_COMPLETE, 0, this);
}
inline void SubTask::complete(PfMessageStatus comp_status, uint16_t meta_ver){
	if(meta_ver > parent_iocb->complete_meta_ver)
		parent_iocb->complete_meta_ver = meta_ver;
	complete(comp_status);
}
inline PfEventQueue* SubTask::half_complete(PfMessageStatus comp_status)
{
	complete_status = comp_status;
}
inline void IoSubTask::complete_read_with_zero() {
//    PfMessageHead* cmd = parent_iocb->cmd_bd->cmd_bd;
    BufferDescriptor* data_bd = parent_iocb->data_bd;

    memset(data_bd->buf, 0, data_bd->data_len);
    complete(PfMessageStatus::MSG_STATUS_SUCCESS);

}
#endif // pf_dispatcher_h__
