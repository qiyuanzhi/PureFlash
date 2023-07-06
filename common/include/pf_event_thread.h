#ifndef pf_event_thread_h__
#define pf_event_thread_h__
#include <functional>
#include "pf_event_queue.h"
#include "pf_spdk_ring.h"

class PfEventThread
{
public:
	#if 0
	PfEventQueue event_queue;
	#else
	PfSpdkQueue event_queue;
	#endif
	pthread_t tid;
	char name[32];
	int (*func_priv)(int *, void *);
	void *arg_v;


	bool inited;
	int init(const char* name, int queue_depth);
	PfEventThread();
	void destroy();
	virtual ~PfEventThread();
	virtual int process_event(int event_type, int arg_i, void* arg_p) = 0;
	int start();
	void stop();
	static void *thread_proc(void* arg);

	int sync_invoke(std::function<int(void)> _f);

};
#endif // pf_event_thread_h__
