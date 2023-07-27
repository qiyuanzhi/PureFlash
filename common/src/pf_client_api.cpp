#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <zookeeper.h>
#include <curl/curl.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <string>
#include <vector>

#include "pf_connection_pool.h"
#include "pf_connection.h"
#include "pf_client_priv.h"
#include <nlohmann/json.hpp>
#include <pf_tcp_connection.h>
#include "pf_message.h"
#include "pf_poller.h"
#include "pf_buffer.h"
#include "pf_client_api.h"
#include "pf_app_ctx.h"

using namespace std;
using nlohmann::json;
size_t iov_from_buf(const struct iovec *iov, unsigned int iov_cnt, const void *buf, size_t bytes);

static const char* pf_lib_ver = "S5 client version:0x00010000";

PfAppCtx client_app_context;

#define CLIENT_TIMEOUT_CHECK_INTERVAL 1 //seconds
const char* default_cfg_file = "/etc/pureflash/pf.conf";
void from_json(const json& j, GeneralReply& reply)
{
	j.at("op").get_to(reply.op);
	j.at("ret_code").get_to(reply.ret_code);
	if (reply.ret_code != 0) {
		if (j.contains("reason"))
			j.at("reason").get_to(reply.reason);
	}
}

void from_json(const json& j, PfClientShardInfo& p) {
	j.at("index").get_to(p.index);
	j.at("store_ips").get_to(p.store_ips);
	j.at("status").get_to(p.status);

}
void from_json(const json& j, PfClientVolume& p) {
	j.at("status").get_to(p.status);
	j.at("volume_name").get_to(p.volume_name);
	j.at("volume_size").get_to(p.volume_size);
	j.at("volume_id").get_to(p.volume_id);
	j.at("shard_count").get_to(p.shard_count);
	j.at("rep_count").get_to(p.rep_count);
	j.at("meta_ver").get_to(p.meta_ver);
	j.at("snap_seq").get_to(p.snap_seq);
	j.at("shards").get_to(p.shards);
}
void from_json(const json& j, PfClientVolumeInfo& p) {
	string temp;
	j.at("status").get_to(temp);
	strcpy(p.status, temp.c_str());
	j.at("name").get_to(temp);
	strcpy(p.volume_name, temp.c_str());
	j.at("size").get_to(p.volume_size);
	j.at("id").get_to(p.volume_id);
	j.at("rep_count").get_to(p.rep_count);
	j.at("meta_ver").get_to(p.meta_ver);
	j.at("snap_seq").get_to(p.snap_seq);
}
void from_json(const json&j, ListVolumeReply &reply)
{
	j.at("volumes").get_to(reply.volumes);
	j.at("ret_code").get_to(reply.ret_code);
	if(reply.ret_code != 0)
		j.at("reason").get_to(reply.reason);
}

#define MAX_URL_LEN 2048

#define VOLUME_MONITOR_INTERVAL 30 //seconds

#define ZK_TIMEOUT 5000


/**
 * Thread model in client
 * 1. User thread(s), user thread will call qfa_open_volume, qfa_aio
 * 2. volume_proc thread, the major work thread to process volume works
 * 3. rdma_cm thread, handle connection establish and close
 * 4. rdma completion thread, wait on rdam completion channel and call rdma completion
 * 5. timeout check thread
 */
typedef struct curl_memory {
	char *memory;
	size_t size;
}curl_memory_t;


struct PfClientVolume* _pf_open_volume(const char* volume_name, const char* cfg_filename, const char* snap_name,
                                      int lib_ver, bool is_aof)
{
	int rc = 0;
	if (lib_ver != S5_LIB_VER)
	{
		S5LOG_ERROR("Caller lib version:%d mismatch lib:%d", lib_ver, S5_LIB_VER);
		return NULL;
	}
	S5LOG_INFO("Opening volume %s@%s", volume_name,
		(snap_name == NULL || strlen(snap_name) == 0) ? "HEAD" : snap_name);
	try
	{
		Cleaner _clean;
		PfClientVolume* volume = new PfClientVolume;
		if (volume == NULL)
		{
			S5LOG_ERROR("alloca memory for volume failed!");
			return NULL;
		}
		_clean.push_back([volume]() { delete volume; });
		//other calls
		volume->volume_name = volume_name;
		if(cfg_filename == NULL)
			cfg_filename = default_cfg_file;
		volume->cfg_file = cfg_filename;
		if(snap_name)
			volume->snap_name = snap_name;

		rc = volume->do_open(false, is_aof);
		if (rc)	{
			return NULL;
		}


		S5LOG_INFO("Succeeded open volume %s@%s(0x%lx), meta_ver=%d, io_depth=%d", volume->volume_name.c_str(),
			volume->snap_seq == -1 ? "HEAD" : volume->snap_name.c_str(), volume->volume_id, volume->meta_ver, volume->io_depth);

		_clean.cancel_all();
		return volume;
	}
	catch (std::exception& e)
	{
		S5LOG_ERROR("Exception in open volume:%s", e.what());
	}
	return NULL;
}


struct PfClientVolume* pf_open_volume(const char* volume_name, const char* cfg_filename, const char* snap_name,	int lib_ver)
{
	return _pf_open_volume(volume_name, cfg_filename, snap_name, lib_ver, false);
}

int pf_query_volume_info(const char* volume_name, const char* cfg_filename, const char* snap_name,
                                       int lib_ver, struct PfClientVolumeInfo* volume)
{
	int rc = 0;
	if (lib_ver != S5_LIB_VER)
	{
		S5LOG_ERROR("Caller lib version:%d mismatch lib:%d", lib_ver, S5_LIB_VER);
		return -EINVAL;
	}
	S5LOG_INFO("Opening volume %s@%s", volume_name,
	           (snap_name == NULL || strlen(snap_name) == 0) ? "HEAD" : snap_name);
	try
	{
		Cleaner _clean;
		ListVolumeReply reply;

		if(cfg_filename == NULL)
			cfg_filename = default_cfg_file;
		conf_file_t cfg = conf_open(cfg_filename);
		if(cfg == NULL)
		{
			S5LOG_ERROR("Failed open config file:%s, rc:%d", cfg_filename, -errno);
			return -errno;
		}
		DeferCall _cfg_r([cfg]() { conf_close(cfg); });

		char* esc_vol_name = curl_easy_escape(NULL, volume_name, 0);
		if (!esc_vol_name)
		{
			S5LOG_ERROR("Curl easy escape failed. ENOMEM");
			return -ENOMEM;
		}
		DeferCall _1([esc_vol_name]() { curl_free(esc_vol_name); });
		std::string query = format_string("op=list_volume&name=%s", esc_vol_name);
		char* esc_snap_name = NULL;
		if(snap_name != NULL) {
			curl_easy_escape(NULL, snap_name, 0);
			if (!esc_snap_name) {
				S5LOG_ERROR("Curl easy escape failed. ENOMEM");
				return -ENOMEM;
			}
			query += format_string("&snap_name=%s", esc_snap_name);
			curl_free(esc_snap_name);
		}

		rc = query_conductor(cfg, query, reply);
		if (rc != 0)
		{
			S5LOG_ERROR("Failed query conductor, rc:%d", rc);
			return rc;
		}

		if(reply.volumes.size() < 1) {
			S5LOG_ERROR("Volume:%s not exists or can't be open", volume_name);
			return -ENOENT;
		}
		*volume = reply.volumes[0];
		S5LOG_INFO("Succeeded query volume %s@%s(0x%lx), meta_ver=%d", volume->volume_name,
		           snap_name == NULL ? "HEAD" : volume->snap_name, volume->volume_id, volume->meta_ver);

		_clean.cancel_all();
		return 0;
	}
	catch (std::exception& e)
	{
		S5LOG_ERROR("Exception in open volume:%s", e.what());
	}
	return -1;
}

static int client_on_tcp_network_done(BufferDescriptor* bd, WcStatus complete_status, PfConnection* _conn, void* cbk_data)
{
	PfTcpConnection* conn = (PfTcpConnection*)_conn;

	if(complete_status == WcStatus::TCP_WC_SUCCESS) {

		if(bd->data_len == sizeof(PfMessageHead)) {
			if(bd->cmd_bd->opcode == PfOpCode::S5_OP_WRITE) {
				//message head sent complete, continue to send data for write OP
				PfClientIocb* iocb = bd->client_iocb;
				conn->add_ref(); //for start send data
				iocb->data_bd->conn = conn;
				conn->start_send(iocb->data_bd); //on client side, use user's buffer
			}
			return 1;
		} else if(bd->data_len == sizeof(PfMessageReply) ) {
			//assert(bd->reply_bd->command_id<32);
			PfClientIocb* iocb = conn->volume->pick_iocb(bd->reply_bd->command_id, bd->reply_bd->command_seq);
			//In io timeout case, we just ignore this completion

			if (unlikely(iocb == NULL)) {
				//this should never happen, since whenever an IO timeout, the connection will be closed
				S5LOG_FATAL("Previous IO cid:%d back but timeout!", bd->reply_bd->command_id);
				return 1;
			}

			iocb->reply_bd = bd;
			bd->client_iocb = iocb;
			if(iocb != NULL && iocb->cmd_bd->cmd_bd->opcode == PfOpCode::S5_OP_READ) {
				conn->add_ref(); //for start recv data
				iocb->data_bd->conn = conn;
				//S5LOG_DEBUG("To recv %d bytes data payload", iocb->data_bd->data_len);
				conn->start_recv(iocb->data_bd); //on client side, use use's buffer
				return 1;
			}
			return conn->volume->event_queue->post_event(EVT_IO_COMPLETE, complete_status, bd);
		}else{
			PfClientIocb* iocb = bd->client_iocb;
			if(iocb->cmd_bd->cmd_bd->opcode == PfOpCode::S5_OP_READ){ //case c
				//for READ, need to complete IO
				PfClientIocb* iocb = bd->client_iocb;
				return conn->volume->event_queue->post_event(EVT_IO_COMPLETE, complete_status, bd);

			}
			return 0;
		}
	}

	S5LOG_ERROR("Unexpected status:%d, data_len=%d", complete_status, bd->data_len);
	return -1;
}

static void client_on_tcp_close(PfConnection* c)
{
	//c->dec_ref(); //Don't dec_ref here, only dec_ref when connection removed from pool

}

int PfClientVolume::do_open(bool reopen, bool is_aof)
{
	int rc = 0;
	conf_file_t cfg = conf_open(cfg_file.c_str());
	if(cfg == NULL)
	{
		S5LOG_ERROR("Failed open config file:%s", cfg_file.c_str());
		return -errno;
	}
	DeferCall _cfg_r([cfg]() { conf_close(cfg); });
	io_depth = conf_get_int(cfg, "client", "io_depth", 32, FALSE);
	io_timeout = conf_get_int(cfg, "client", "io_timeout", 30, FALSE);

	char* esc_vol_name = curl_easy_escape(NULL, volume_name.c_str(), 0);
	if (!esc_vol_name)
	{
		S5LOG_ERROR("Curl easy escape failed.");
		return -ENOMEM;
	}
	DeferCall _1([esc_vol_name]() { curl_free(esc_vol_name); });
	char* esc_snap_name = curl_easy_escape(NULL, snap_name.c_str(), 0);
	if (!esc_snap_name)
	{
		S5LOG_ERROR("Curl easy escape failed.");
		return -ENOMEM;
	}
	DeferCall _2([esc_snap_name]() { curl_free(esc_snap_name); });
	const char* op = is_aof ? "open_aof" : "open_volume";
	std::string query = format_string("op=%s&volume_name=%s&snapshot_name=%s", op, esc_vol_name, esc_snap_name);
	rc = query_conductor(cfg, query, *this);
	if (rc != 0)
		return rc;

	for(int i=0;i<shards.size();i++){
		shards[i].parsed_store_ips = split_string(shards[i].store_ips, ',');
	}
	if(reopen) {
		state = VOLUME_OPENED;
		open_time = now_time_usec();
		return 0;
	}
	Cleaner clean;
	tcp_poller = new PfPoller();
	if(tcp_poller == NULL) {
		S5LOG_ERROR("No memory to alloc poller");
		return -ENOMEM;
	}
	clean.push_back([this](){delete tcp_poller; tcp_poller = NULL;});
	//TODO: max_fd_count in poller->init should depend on how many server this volume layed on
	rc = tcp_poller->init(volume_name.c_str(), 128);
	if(rc != 0) {
		S5LOG_ERROR("tcp_poller init failed, rc:%d", rc);
		return rc;
	}
	conn_pool = new PfConnectionPool();
	if (conn_pool == NULL) {
		S5LOG_ERROR("No memory to alloc connection pool");
		return -ENOMEM;
	}
	clean.push_back([this]{delete conn_pool; conn_pool=NULL;});
	conn_pool->init((int) shards.size() * 2, tcp_poller, this, this->volume_id, io_depth, TCP_TYPE, client_on_tcp_network_done, client_on_tcp_close);
	rc = data_pool.init(PF_MAX_IO_SIZE, io_depth);
	if(rc != 0){
		S5LOG_ERROR("Failed to init data_pool, rc:%d", rc);
		return rc;
	}
	clean.push_back([this](){data_pool.destroy();});

	rc = cmd_pool.init(sizeof(PfMessageHead), io_depth);
	if(rc != 0){
		S5LOG_ERROR("Failed to init cmd_pool, rc:%d", rc);
		return rc;
	}
	clean.push_back([this](){cmd_pool.destroy();});

	rc = reply_pool.init(sizeof(PfMessageReply), io_depth);
	if(rc != 0){
		S5LOG_ERROR("Failed to init reply_pool, rc:%d", rc);
		return rc;
	}
	clean.push_back([this](){reply_pool.destroy();});

	rc = iocb_pool.init(io_depth);
	if(rc != 0){
		S5LOG_ERROR("Failed to init iocb_pool, rc:%d", rc);
		return rc;
	}
	for(int i=0;i<io_depth;i++)
	{
		PfClientIocb* io = iocb_pool.alloc();
		io->cmd_bd = cmd_pool.alloc();
		io->cmd_bd->cmd_bd->command_id = (uint16_t)i;
		io->cmd_bd->data_len = io->cmd_bd->buf_capacity;
		io->cmd_bd->client_iocb = io;
		io->data_bd = data_pool.alloc();
		io->data_bd->client_iocb = io;
		io->reply_bd = NULL;
		BufferDescriptor* rbd = reply_pool.alloc();
		rbd->data_len = rbd->buf_capacity;
		rbd->client_iocb = NULL;
		reply_pool.free(rbd);
		iocb_pool.free(io);
	}
	timeout_thread = std::thread([this] (){
		timeout_check_proc();
	});

	vol_proc = new PfVolumeEventProc(this);
	vol_proc->init("vol_proc", io_depth);
	vol_proc->start();
	event_queue = vol_proc->event_queue; //keep for quick reference
	state = VOLUME_OPENED;
	clean.cancel_all();
	open_time = now_time_usec();
	return 0;
}

static inline int cmp(const void *a, const void *b)
{
	return strcmp(*(char * const *)a, *(char * const *)b);
}

string get_master_conductor_ip(const char *zk_host, const char* cluster_name)
{
    struct String_vector condutors = {0};
    char **str = NULL;
    zoo_set_debug_level(ZOO_LOG_LEVEL_WARN);
    zhandle_t *zkhandle = zookeeper_init(zk_host, NULL, ZK_TIMEOUT, NULL, NULL, 0);
    if (zkhandle == NULL)
    {
        throw std::runtime_error("Error zookeeper_init");
    }
	DeferCall _r_z([zkhandle]() { zookeeper_close(zkhandle); });

	int state;
    for(int i=0; i<100; i++)
    {
		state = zoo_state(zkhandle);
        if (state == ZOO_CONNECTED_STATE)
            break;
		S5LOG_INFO("Connecting to zk server %s, state:%d ...", zk_host, state);
        usleep(300000);
    }
	if (state != ZOO_CONNECTED_STATE)
	{
		throw std::runtime_error("Error when connecting to zookeeper servers...");
	}

	int rc = 0;
	//const char* zk_root = "/pureflash/" + cluster_name;
	string zk_root = format_string("/pureflash/%s/conductors", cluster_name);
	rc = zoo_get_children(zkhandle, zk_root.c_str(), 0, &condutors);
	if (ZOK != rc || condutors.count == 0)
    {
		throw std::runtime_error(format_string("Error when get S5 conductor from zk, rc:%d, conductor count:%d", rc, condutors.count));
    }
	DeferCall _r_c([&condutors]() {deallocate_String_vector(&condutors); });
    str = (char **)condutors.data;
    qsort(str, condutors.count, sizeof(char *), cmp);

	char leader_path[256];
    int len = snprintf(leader_path, sizeof(leader_path), "%s/%s", zk_root.c_str(), str[0]);
    if (len >= sizeof(leader_path) || len < 0)
    {
		throw std::runtime_error(format_string("Cluster name is too long, max length is:%d", sizeof(leader_path)));
    }

	char ip_str[256];
	len = sizeof(ip_str);
	rc = zoo_get(zkhandle, leader_path, 0, ip_str, &len, 0);
    if (ZOK != rc)
    {
		throw std::runtime_error(format_string("Error when get pfconductor leader data:%d", rc));
    }
    if(len == 0 || len >= sizeof(ip_str))
	{
		throw std::runtime_error(format_string("Error when get pfconductor leader data, invalid len:%d", len));
	}
    ip_str[len] = 0;
    S5LOG_INFO("Get S5 conductor IP:%s", ip_str);
    return std::string(ip_str);
}

static size_t write_mem_callback(void *contents, size_t size, size_t nmemb, void *buf)
{
	size_t realsize = size * nmemb;
	curl_memory_t *mem = (curl_memory_t *)buf;

	mem->memory = (char*)realloc(mem->memory, mem->size + realsize + 1);
	if (mem->memory == NULL)
	{
		S5LOG_ERROR("not enough memory (realloc returned NULL)\n");
		return 0;
	}

	memcpy(mem->memory + mem->size, contents, realsize);
	mem->size += realsize;
	((char*)mem->memory)[mem->size] = '\0';

	return realsize;
}

void* pf_http_get(std::string& url, int timeout_sec, int retry_times)
{
	CURLcode res;
	CURL *curl = NULL;
	curl_memory_t curl_buf;
	curl = curl_easy_init();
	if (curl == NULL)
	{
		throw std::runtime_error("curl_easy_init failed.");
	}
	DeferCall _r_curl([curl]() { curl_easy_cleanup(curl); });

	curl_buf.memory = (char *)malloc(1 << 20);
	if (curl_buf.memory == NULL)
		throw std::runtime_error("Failed alloc curl buffer");
	curl_buf.size = 0;


	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_mem_callback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&curl_buf);
	//curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	// Set timeout.
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);

	for (int i = 0; i < retry_times; i++)
	{
		S5LOG_DEBUG("Query %s ...", url.c_str());
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		res = curl_easy_perform(curl);
		if (res == CURLE_OK)
		{
			long http_code = 0;
			curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
			curl_buf.memory[curl_buf.size] = 0;
			S5LOG_DEBUG("HTTP return:%d content:%*.s", http_code, curl_buf.size, curl_buf.memory);
			if (http_code == 200)
			{
				return curl_buf.memory;
			}
			else
			{
				S5LOG_ERROR("Server returns error, http code:%d, content:%s", http_code, curl_buf.memory);
				return NULL;
			}

		}
		if (i < retry_times - 1)
		{
			S5LOG_ERROR("Failed query %s, will retry", url.c_str());
			sleep(DEFAULT_HTTP_QUERY_INTERVAL);
		}
	}

	free(curl_buf.memory);
	return NULL;
}



void PfClientVolume::client_do_complete(int wc_status, BufferDescriptor* wr_bd)
{
	const static int ms1 = 1000;
	if (unlikely(wc_status != TCP_WC_SUCCESS))
	{
		S5LOG_INFO("Op complete unsuccessful opcode:%d, status:%s", wr_bd->wr_op, WcStatusToStr(WcStatus(wc_status)));
		PfConnection* conn = wr_bd->conn;
		reply_pool.free(wr_bd);
		conn->close();
		//IO not completed, it will be resend by timeout
		return;
	}

    if (wr_bd->wr_op == TCP_WR_RECV)
    {
		PfConnection* conn = wr_bd->conn;
		PfClientVolume* vol = conn->volume;
	    PfClientIocb* io = wr_bd->client_iocb;
		PfMessageReply* reply = io->reply_bd->reply_bd;

			io->reply_time = now_time_usec();
			PfMessageStatus s = (PfMessageStatus)reply->status;
			if (unlikely(s & (MSG_STATUS_REOPEN)))
			{
				S5LOG_WARN( "Get reopen from store, conn:%s status code:0x%x, req meta_ver:%d store meta_ver:%d",
					conn->connection_info.c_str(), s, io->cmd_bd->cmd_bd->meta_ver, reply->meta_ver);
				if (vol->meta_ver < reply->meta_ver)
				{
					S5LOG_WARN("client meta_ver is:%d, store meta_ver is:%d. reopen volume", vol->meta_ver, reply->meta_ver);
					vol->event_queue->post_event(EVT_REOPEN_VOLUME, reply->meta_ver, (void *)(now_time_usec()));
				}
				io->conn->dec_ref();
				io->conn=NULL;
				vol->event_queue->post_event(EVT_IO_REQ, 0, io);
				reply_pool.free(wr_bd);
				return;
			}


			PfMessageHead* io_cmd = io->cmd_bd->cmd_bd;
			//On client side, we rely on the io timeout mechnism to release time connection
			//Here we just release the io task
			if (unlikely(io_cmd->opcode == S5_OP_HEARTBEAT))
			{
				__sync_fetch_and_sub(&conn->inflying_heartbeat, 1);
				io->sent_time = 0;
				io->conn->dec_ref();
				io->conn = NULL;
				free_iocb(io);
				reply_pool.free(wr_bd);
				return;
			}

			void* arg = io->ulp_arg;
			ulp_io_handler h = io->ulp_handler;
			uint64_t io_end_time = now_time_usec();
			uint64_t io_elapse_time = (io_end_time - io->submit_time) / ms1;

			if (io_elapse_time > 2000)
			{
				S5LOG_WARN("SLOW IO, shard id:%d, command_id:%d, vol:%s, since submit:%dms since send:%dms",
						   io_cmd->offset >> SHARD_SIZE_ORDER,
						   io_cmd->command_id,
						   vol->volume_name.c_str(),
						   io_elapse_time,
						   (io_end_time-io->sent_time)/ms1
					);
			}


	        reply_pool.free(io->reply_bd);
	        io->reply_bd = NULL;
	        io->sent_time=0;
	        io->conn->dec_ref();
	        io->conn=NULL;
	        if(io->cmd_bd->cmd_bd->opcode == S5_OP_READ)  {
	        	if(io->user_iov_cnt)
					iov_from_buf(io->user_iov, io->user_iov_cnt, io->data_bd->buf, io->data_bd->data_len);
		        else
		        	memcpy(io->user_buf, io->data_bd->buf, io->data_bd->data_len);
	        }
			free_iocb(io);
			h(arg, s);

    }
    else if(wr_bd->wr_op != TCP_WR_SEND)
    {
       S5LOG_ERROR("Unexpected completion, op:%d", wr_bd->wr_op);
    }
}


void pf_close_volume(PfClientVolume* volume)
{
	volume->close();
	delete volume;
}

static int reopen_volume(PfClientVolume* volume)
{
	int rc = 0;
	S5LOG_INFO( "Reopening volume %s@%s, meta_ver:%d", volume->volume_name.c_str(),
		volume->snap_seq == -1 ? "HEAD" : volume->snap_name.c_str(), volume->meta_ver);

	volume->status = VOLUME_DISCONNECTED;
	rc = volume->do_open(true);
	if (unlikely(rc))
	{
		S5LOG_ERROR("Failed reopen volume!");
		volume->state = VOLUME_REOPEN_FAIL;
		return rc;
	} else {
		//TODO: notify volume has reopened, volume size may changed
	}
	volume->state = VOLUME_OPENED;

	S5LOG_INFO("Succeeded reopen volume %s@%s(0x%lx), meta_ver:%d io_depth=%d", volume->volume_name.c_str(),
		volume->snap_seq == -1 ? "HEAD" : volume->snap_name.c_str(), volume->volume_id, volume->meta_ver, volume->io_depth);
	return rc;
}

int PfClientVolume::resend_io(PfClientIocb* io)
{
	S5LOG_WARN("Requeue IO(cid:%d", io->cmd_bd->cmd_bd->command_id);
	io->cmd_bd->cmd_bd->command_seq ++;
	__sync_fetch_and_add(&io->cmd_bd->cmd_bd->command_seq, 1);
	int rc = event_queue->post_event(EVT_IO_REQ, 0, io);
	if (rc)
		S5LOG_ERROR("Failed to resend_io io, rc:%d", rc);
	return rc;
}


const char* show_ver()
{
	return (const char*)pf_lib_ver;
}

int PfVolumeEventProc::process_event(int event_type, int arg_i, void* arg_p)
{
	return volume->process_event(event_type, arg_i, arg_p);
}

int PfClientVolume::process_event(int event_type, int arg_i, void* arg_p)
{
	//S5LOG_INFO("get event:%d", event_type);
	switch (event_type)
	{
	case EVT_IO_REQ:
	{
		PfClientIocb* io = (PfClientIocb*)arg_p;
		BufferDescriptor* cmd_bd = io->cmd_bd;
		PfMessageHead *io_cmd = io->cmd_bd->cmd_bd;

		int shard_index = (int)(io_cmd->offset >> SHARD_SIZE_ORDER);
		struct PfConnection* conn = get_shard_conn(shard_index);

		if (conn == NULL)
		{
			io->ulp_handler(io->ulp_arg, -EIO);
			S5LOG_ERROR("conn == NULL ,command id:%d task_sequence:%d, io_cmd :%d", io_cmd->command_id, io_cmd->command_seq, io_cmd->opcode);
			iocb_pool.free(io);
			break;
		}
		io_cmd->meta_ver = (uint16_t)meta_ver;

		BufferDescriptor* rbd = reply_pool.alloc();
		if(unlikely(rbd == NULL))
		{
			S5LOG_ERROR("No reply bd available to do io now, requeue IO");
			io->ulp_handler(io->ulp_arg, -EAGAIN);
			iocb_pool.free(io);
			break;
		}
		io->is_timeout = FALSE;
		io->conn = conn;
		conn->add_ref();
		io->sent_time = now_time_usec();
		int rc = conn->post_recv(rbd);
		if (rc)
		{
			S5LOG_ERROR("Failed to post_recv for reply");
			reply_pool.free(rbd);
			io->ulp_handler(io->ulp_arg, -EAGAIN);
			iocb_pool.free(io);
			break;
		}
		rc = conn->post_send(cmd_bd);
		if (rc)	{
			S5LOG_ERROR("Failed to post_send for cmd");
			//reply_pool.free(rbd); //not reclaim rbd, since this bd has in connection's receive queue
			io->ulp_handler(io->ulp_arg, -EAGAIN);
			iocb_pool.free(io);
			break;
		}
		break;
	}
	case EVT_IO_COMPLETE:
		client_do_complete(arg_i, (BufferDescriptor*)arg_p);
		break;
	case EVT_IO_TIMEOUT:
	{
		if (state != VOLUME_OPENED)
		{
			S5LOG_WARN("volume state is:%d", state);
			break;
		}
		PfClientIocb* io = (PfClientIocb*)arg_p;
		S5LOG_WARN("volume_proc timeout, cid:%d, conn:%s", io->cmd_bd->cmd_bd->command_id, io->conn);
		/*
		 * If time_recv is 0, io task:1)does not begin, 2)has finished.
		 */
		if (io->sent_time != 0 && now_time_usec() > io->sent_time + io_timeout)
		{
			PfMessageHead *io_cmd = (PfMessageHead *)io->cmd_bd->buf;
			if (unlikely(io_cmd->opcode == S5_OP_HEARTBEAT))
			{
				S5LOG_ERROR("heartbeat timeout for conn:%s", io->conn->connection_info.c_str());
				iocb_pool.free(io);
				break;
			}
			S5LOG_WARN("IO(cid:%d) timeout, vol:%s, shard:%d, store:%s will reconnect and resend...",
				io_cmd->command_id, volume_name.c_str(), io_cmd->offset >> SHARD_SIZE_ORDER, io->conn->connection_info.c_str());
			io->sent_time = 0;
			io->conn->close();
			io->conn->dec_ref();
			io->conn = NULL;
			int rc = resend_io(io);
			if(rc) {
				iocb_pool.free(io);
			}
		}
		break;
	}
	case EVT_REOPEN_VOLUME:
	{
		if ((uint64_t)arg_p > open_time)
		{
			reopen_volume(this);
		}
		break;
	}
	case EVT_VOLUME_RECONNECT:
	{
		if (iocb_pool.remain() == iocb_pool.obj_count)
			if (state == VOLUME_OPENED || state == VOLUME_REOPEN_FAIL || state == VOLUME_DISCONNECTED)
			{
				S5LOG_WARN("send volume reopen request for:%s", volume_name.c_str());
				conn_pool->close_all();
				vol_proc->event_queue->post_event(EVT_REOPEN_VOLUME, 0, (void*)(now_time_usec()));
			}
		break;
	}
	case EVT_SEND_HEARTBEAT:
	{
		if (conn_pool->ip_id_map.size() == 0)
		{
			break;
		}
		next_heartbeat_idx %= (int)conn_pool->ip_id_map.size();
		int hb_sent = 0;
		int ht_idx = 0;
		for (auto it = conn_pool->ip_id_map.begin(); it != conn_pool->ip_id_map.end();)
		{

			PfConnection* conn = it->second;
			if (conn->state != CONN_OK || __sync_fetch_and_sub(&conn->inflying_heartbeat, 0) > 2)
			{
				conn_pool->ip_id_map.erase(it++);
				S5LOG_ERROR("connection:%p:%s timeout", conn, conn->connection_info.c_str());
				conn->close();
			}
			else
			{
				++it;

				if (ht_idx >= next_heartbeat_idx && hb_sent < 4)
				{
					conn->send_heartbeat();
					hb_sent++;
					next_heartbeat_idx++;
				}
			}
			ht_idx ++;
		}
		break;
	}
	case EVT_THREAD_EXIT://dead code, event already handled in parent class
	{
		S5LOG_INFO("EVT_THREAD_EXIT received, exit now...");
		pthread_exit(0);
	}
	default:
		S5LOG_ERROR("Volume get unknown event:%d", event_type);
	}
	return 0;
}

/**
* get a shard connection from pool. connection is shared by shards on same node.
*/
PfConnection* PfClientVolume::get_shard_conn(int shard_index)
{
	PfConnection* conn = NULL;
	if (state != VOLUME_OPENED)
	{
		return NULL;
	}
	PfClientShardInfo * shard = &shards[shard_index];
	for (int i=0; i < shard->store_ips.size(); i++)
	{
		conn = conn_pool->get_conn(shard->parsed_store_ips[shard->current_ip], TCP_TYPE);
		if (conn != NULL) {
			return conn;
		}
		shard->current_ip = (shard->current_ip + 1) % (int)shard->store_ips.size();
	}
	S5LOG_ERROR("Failed to get an usable IP for vol:%s shard:%d, change volume state to VOLUME_DISCONNECTED ",
			 volume_name.c_str(), shard_index);
	state = VOLUME_DISCONNECTED;
	return NULL;
}

void PfClientVolume::timeout_check_proc()
{
	prctl(PR_SET_NAME, "clnt_time_chk");
	while (1)
	{
		if (sleep(CLIENT_TIMEOUT_CHECK_INTERVAL) != 0)
			return ;
		uint64_t now = now_time_usec();
		struct PfClientIocb *ios = iocb_pool.data;
		int64_t io_timeout_us = io_timeout * 1000000LL;
		for (int i = 0; i < iocb_pool.obj_count; i++)
		{
			if (ios[i].sent_time != 0 && now > ios[i].sent_time + io_timeout_us && ios[i].is_timeout != 1)
			{
				S5LOG_DEBUG("IO timeout detected, cid:%d, volume:%s, timeout:%luus",
                            ((PfMessageHead*)ios[i].cmd_bd->buf)->command_id, volume_name.c_str(), io_timeout_us);
				vol_proc->event_queue->post_event(EVT_IO_TIMEOUT, 0, &ios[i]);
				ios[i].is_timeout = 1;
			}
		}
	}

}

void PfClientVolume::close() {
	S5LOG_INFO("close volume:%s", volume_name.c_str());
	state = VOLUME_CLOSED;

	pthread_cancel(timeout_thread.native_handle());
	timeout_thread.join();

	vol_proc->stop();

	conn_pool->close_all();
	iocb_pool.destroy();
	cmd_pool.destroy();
	data_pool.destroy();
	reply_pool.destroy();

//	for(int i=0;i<volume->shards.size();i++)
//	{
//		delete volume->shards[i];
//	}
}
static inline size_t
iov_to_buf(const struct iovec *iov, const unsigned int iov_cnt, void *buf, size_t bytes)
{
	size_t done;
	size_t offset = 0;
	unsigned int i;
	for (i = 0, done = 0; (offset || done < bytes) && i < iov_cnt; i++) {
		if (offset < iov[i].iov_len) {
			size_t len = std::min(iov[i].iov_len - offset, bytes - done);
			memcpy((char*)buf + done, (char*)iov[i].iov_base + offset, len);
			done += len;
			offset = 0;
		} else {
			offset -= iov[i].iov_len;
		}
	}
	assert(offset == 0);
	return done;
}
size_t iov_from_buf(const struct iovec *iov, unsigned int iov_cnt, const void *buf, size_t bytes)
{
	size_t done;
	size_t offset = 0;
	unsigned int i;
	for (i = 0, done = 0; (offset || done < bytes) && i < iov_cnt; i++) {
		if (offset < iov[i].iov_len) {
			size_t len = std::min(iov[i].iov_len - offset, bytes - done);
			memcpy((char*)iov[i].iov_base + offset, (char*)buf + done, len);
			done += len;
			offset = 0;
		} else {
			offset -= iov[i].iov_len;
		}
	}
	assert(offset == 0);
	return done;
}
static int unalign_io_print_cnt = 0;
int pf_iov_submit(struct PfClientVolume* volume, const struct iovec *iov, const unsigned int iov_cnt, size_t length, off_t offset,
                 ulp_io_handler callback, void* cbk_arg, int is_write) {
	// Check request params
	if (unlikely((offset & SECT_SIZE_MASK) != 0 || (length & SECT_SIZE_MASK) != 0 )) {
		S5LOG_ERROR("Invalid offset:%ld or length:%ld", offset, length);
		return -EINVAL;
	}
	if(unlikely(length > PF_MAX_IO_SIZE)){
		S5LOG_ERROR("IO size:%ld exceed max:%ld", length, PF_MAX_IO_SIZE);
		return -EINVAL;
	}
	if(unlikely((offset & 0x0fff) || (length & 0x0fff)))	{
		unalign_io_print_cnt ++;
		if((unalign_io_print_cnt % 1000) == 1) {
			S5LOG_WARN("Unaligned IO on volume:%s OP:%s offset:0x%lx len:0x%x, num:%d", volume->volume_name.c_str(),
			        is_write?"WRITE":"READ", offset, length, unalign_io_print_cnt);
		}
	}
	auto io = volume->iocb_pool.alloc();
	if (io == NULL)
		return -EAGAIN;
	//S5LOG_INFO("Alloc iocb:%p, data_bd:%p", io, io->data_bd);
	//assert(io->data_bd->client_iocb != NULL);

	io->ulp_handler = callback;
	io->ulp_arg = cbk_arg;

	struct PfMessageHead *cmd = io->cmd_bd->cmd_bd;
	io->submit_time = now_time_usec();
	io->user_iov = iov;
	io->user_iov_cnt = iov_cnt;
	io->data_bd->data_len = (int)length;
	cmd->opcode = is_write ? S5_OP_WRITE : S5_OP_READ;
	if(is_write){
		iov_to_buf(iov, iov_cnt, io->data_bd->buf, length);
	}
	cmd->vol_id = volume->volume_id;
	//cmd->buf_addr = (__le64) buf;
	cmd->rkey = 0;
	cmd->offset = offset;
	cmd->length = (uint32_t)length;
	cmd->snap_seq = volume->snap_seq;
	int rc = volume->event_queue->post_event( EVT_IO_REQ, 0, io);
	if (rc)
		S5LOG_ERROR("Failed to submmit io, rc:%d", rc);
	return rc;
}

int pf_io_submit(struct PfClientVolume* volume, void* buf, size_t length, off_t offset,
                 ulp_io_handler callback, void* cbk_arg, int is_write) {
	// Check request params
	if (unlikely((offset & SECT_SIZE_MASK) != 0 || (length & SECT_SIZE_MASK) != 0 )) {
		S5LOG_ERROR("Invalid offset:%ld or length:%ld", offset, length);
		return -EINVAL;
	}
	if(unlikely((offset & 0x0fff) || (length & 0x0fff)))	{
		unalign_io_print_cnt ++;
		if((unalign_io_print_cnt % 1000) == 1) {
			S5LOG_WARN("Unaligned IO on volume:%s OP:%s offset:0x%lx len:0x%x, num:%d", volume->volume_name.c_str(),
			           is_write ? "WRITE" : "READ", offset, length, unalign_io_print_cnt);
		}
	}
	auto io = volume->iocb_pool.alloc();
	if (io == NULL)
		return -EAGAIN;
	//S5LOG_INFO("Alloc iocb:%p, data_bd:%p", io, io->data_bd);
	//assert(io->data_bd->client_iocb != NULL);

	io->ulp_handler = callback;
	io->ulp_arg = cbk_arg;

	struct PfMessageHead *cmd = io->cmd_bd->cmd_bd;
	io->submit_time = now_time_usec();
	io->user_buf = buf;
	io->user_iov_cnt = 0;
	memcpy(io->data_bd->buf, buf, length);
	io->data_bd->data_len = (int)length;
	cmd->opcode = is_write ? S5_OP_WRITE : S5_OP_READ;
	cmd->vol_id = volume->volume_id;
	//cmd->buf_addr = (__le64) buf;
	cmd->rkey = 0;
	cmd->offset = offset;
	cmd->length = (uint32_t)length;
	cmd->snap_seq = volume->snap_seq;
	int rc = volume->event_queue->post_event( EVT_IO_REQ, 0, io);
	if (rc)
		S5LOG_ERROR("Failed to submmit io, rc:%d", rc);
	return rc;
}

uint64_t pf_get_volume_size(struct PfClientVolume* vol)
{
	return vol->volume_size;
}

int pf_create_tenant(const char* tenant_name, const char* cfg_filename)
{
	int rc = 0;
	
	S5LOG_INFO("Creating tenant %s", tenant_name);
	try
	{
		Cleaner _clean;
		GeneralReply reply;

		if (cfg_filename == NULL)
			cfg_filename = default_cfg_file;
		conf_file_t cfg = conf_open(cfg_filename);
		if (cfg == NULL)
		{
			S5LOG_ERROR("Failed open config file:%s, rc:%d", cfg_filename, -errno);
			return -errno;
		}
		DeferCall _cfg_r([cfg]() { conf_close(cfg); });

		char* esc_t_name = curl_easy_escape(NULL, tenant_name, 0);
		if (!esc_t_name)
		{
			S5LOG_ERROR("Curl easy escape failed. ENOMEM");
			return -ENOMEM;
		}
		DeferCall _1([esc_t_name]() { curl_free(esc_t_name); });
		std::string query = format_string("op=create_tenant&tenant_name=%s", esc_t_name);

		rc = query_conductor(cfg, query, reply);
		if (rc != 0)
		{
			S5LOG_ERROR("Failed query conductor, rc:%d", rc);
			return rc;
		}

		
		S5LOG_INFO("Succeeded create tenant %s", tenant_name);

		_clean.cancel_all();
		return 0;
	}
	catch (std::exception& e)
	{
		S5LOG_ERROR("Exception in create tenant:%s", e.what());
	}
	return -1;
}

int pf_rename_volume(/*const char* tenant_name,*/const char* vol_name, const char* new_name, const char* cfg_filename)
{
	int rc = 0;

	S5LOG_INFO("rename volume %s to %s", vol_name, new_name);
	try
	{
		GeneralReply reply;

		if (cfg_filename == NULL)
			cfg_filename = default_cfg_file;
		conf_file_t cfg = conf_open(cfg_filename);
		if (cfg == NULL)
		{
			S5LOG_ERROR("Failed open config file:%s, rc:%d", cfg_filename, -errno);
			return -errno;
		}
		DeferCall _cfg_r([cfg]() { conf_close(cfg); });

		//char* esc_t_name = curl_easy_escape(NULL, tenant_name, 0);
		//if (!esc_t_name)
		//{
		//	S5LOG_ERROR("Curl easy escape failed. ENOMEM");
		//	return -ENOMEM;
		//}
		//DeferCall _1([esc_t_name]() { curl_free(esc_t_name); });

		char* esc_v_name = curl_easy_escape(NULL, vol_name, 0);
		if (!esc_v_name)
		{
			S5LOG_ERROR("Curl easy escape failed. ENOMEM");
			return -ENOMEM;
		}
		DeferCall _2([esc_v_name]() { curl_free(esc_v_name); });

		char* esc_nv_name = curl_easy_escape(NULL, new_name, 0);
		if (!esc_nv_name)
		{
			S5LOG_ERROR("Curl easy escape failed. ENOMEM");
			return -ENOMEM;
		}
		DeferCall _3([esc_nv_name]() { curl_free(esc_nv_name); });


		//std::string query = format_string("op=update_volume&tenant_name=%s&volume_name=%s&new_volume_name=%s",
		//	esc_t_name, esc_v_name, esc_nv_name);
		std::string query = format_string("op=update_volume&volume_name=%s&new_volume_name=%s",
			esc_v_name, esc_nv_name);

		rc = query_conductor(cfg, query, reply);
		if (rc != 0)
		{
			S5LOG_ERROR("Failed query conductor, rc:%d", rc);
			return rc;
		}


		S5LOG_INFO("Succeeded rename volume %s", vol_name);
		return 0;
	}
	catch (std::exception& e)
	{
		S5LOG_ERROR("Exception in rename volume:%s", e.what());
	}
	return -1;
}

int pf_delete_volume(const char* vol_name,  const char* cfg_filename)
{
	int rc = 0;

	S5LOG_INFO("delete volume %s ", vol_name);
	try
	{
		GeneralReply reply;

		if (cfg_filename == NULL)
			cfg_filename = default_cfg_file;
		conf_file_t cfg = conf_open(cfg_filename);
		if (cfg == NULL)
		{
			S5LOG_ERROR("Failed open config file:%s, rc:%d", cfg_filename, -errno);
			return -errno;
		}
		DeferCall _cfg_r([cfg]() { conf_close(cfg); });

		//char* esc_t_name = curl_easy_escape(NULL, tenant_name, 0);
		//if (!esc_t_name)
		//{
		//	S5LOG_ERROR("Curl easy escape failed. ENOMEM");
		//	return -ENOMEM;
		//}
		//DeferCall _1([esc_t_name]() { curl_free(esc_t_name); });

		char* esc_v_name = curl_easy_escape(NULL, vol_name, 0);
		if (!esc_v_name)
		{
			S5LOG_ERROR("Curl easy escape failed. ENOMEM");
			return -ENOMEM;
		}
		DeferCall _2([esc_v_name]() { curl_free(esc_v_name); });

		
		std::string query = format_string("op=delete_volume&volume_name=%s", esc_v_name);

		rc = query_conductor(cfg, query, reply);
		if (rc != 0)
		{
			S5LOG_ERROR("Failed query conductor, rc:%d", rc);
			return rc;
		}


		S5LOG_INFO("Succeeded delete volume %s", vol_name);
		return 0;
	}
	catch (std::exception& e)
	{
		S5LOG_ERROR("Exception in delete volume:%s", e.what());
	}
	return -1;
}

static void __attribute__((constructor)) app_context_init(void)
{ 
	g_app_ctx = &client_app_context;
}