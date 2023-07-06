#ifndef pf_spdk_ring_h__
#define pf_spdk_ring_h__

#include "pf_event_queue.h"

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/likely.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
#include "spdk/util.h"
#include "spdk/fd_group.h"


struct pf_spdk_msg {
    struct S5Event event;
	SLIST_ENTRY(pf_spdk_msg)	link;
};



class PfSpdkQueue
{
public:
	char name[32];

    struct spdk_ring *messages;
    SLIST_HEAD(, pf_spdk_msg)  msg_cache;
	struct spdk_mempool * msg_mempool;
    int cahce_cnt;
	int event_fd;

	PfSpdkQueue();
	~PfSpdkQueue();

    int init(const char* name, int size, enum spdk_ring_type mode);
    void destroy();
    int post_event(int type, int arg_i, void* arg_p);
	int get_events(int max_events, void **msgs);
	int put_event(void *msg);
	int sync_invoke(std::function<int()> f);
};


#endif
