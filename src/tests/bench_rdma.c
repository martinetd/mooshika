
#include "config.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <unistd.h>	//read
#include <getopt.h>
#include <errno.h>
#include <poll.h>

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "log.h"
#include "trans_rdma.h"

#define CHUNK_SIZE 1024*1024
#define RECV_NUM 2
#define libercat_post_RW libercat_post_read

#define TEST_Z(x)  do { if ( (x)) ERROR_LOG("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_NZ(x) do { if (!(x)) ERROR_LOG("error: " #x " failed (returned zero/null)."); } while (0)

struct datamr {
	struct ibv_mr *mr;
	libercat_rloc_t *rloc;
	libercat_data_t *data;
	volatile int *count;
	pthread_mutex_t *lock;
	pthread_cond_t *cond;
};

void callback_send(libercat_trans_t *trans, void *arg) {

}

void callback_disconnect(libercat_trans_t *trans) {
}

void callback_recv(libercat_trans_t *trans, void *arg) {
	struct datamr *datamr = arg;

	pthread_mutex_lock(datamr->lock);
	pthread_cond_signal(datamr->cond);
	pthread_mutex_unlock(datamr->lock);
}

void callback_read(libercat_trans_t *trans, void *arg) {
	struct datamr *datamr = arg;

	if (trans->state == LIBERCAT_CONNECTED)
		TEST_Z(libercat_post_RW(trans, datamr->data, datamr->mr, datamr->rloc, callback_read, datamr));

	*datamr->count += 1;
	pthread_cond_signal(datamr->cond);
}

void print_help(char **argv) {
	printf("Usage: %s {-s|-c addr}\n", argv[0]);
}

int main(int argc, char **argv) {


	libercat_trans_t *trans;
	uint8_t *rdmabuf;
	struct ibv_mr *mr;

	libercat_data_t *wdata;

	libercat_trans_attr_t attr;

	memset(&attr, 0, sizeof(libercat_trans_attr_t));

	attr.server = -1; // put an incorrect value to check if we're either client or server
	// sane values for optional or non-configurable elements
	attr.rq_depth = 1;
	attr.sq_depth = RECV_NUM+2; // RECV_NUM for read requets, one for the final wait_send, one to have a free one (post in a callback)
	attr.addr.sa_in.sin_family = AF_INET;
	attr.addr.sa_in.sin_port = htons(1235);
//	attr.disconnect_callback = callback_disconnect;

	// argument handling
	static struct option long_options[] = {
		{ "client",	required_argument,	0,		'c' },
		{ "server",	required_argument,	0,		's' },
		{ "port",	required_argument,	0,		'p' },
		{ "help",	no_argument,		0,		'h' },
		{ 0,		0,			0,		 0  }
	};

	int option_index = 0;
	int op;
	while ((op = getopt_long(argc, argv, "@hvsc:p:", long_options, &option_index)) != -1) {
		switch(op) {
			case '@':
				printf("%s compiled on %s at %s\n", argv[0], __DATE__, __TIME__);
				printf("Release = %s\n", VERSION);
				printf("Release comment = %s\n", VERSION_COMMENT);
				printf("Git HEAD = %s\n", _GIT_HEAD_COMMIT ) ;
				printf("Git Describe = %s\n", _GIT_DESCRIBE ) ;
				exit(0);
			case 'h':
				print_help(argv);
				exit(0);
			case 'v':
				ERROR_LOG("verbose switch not ready just yet, come back later!\n");
				break;
			case 'c':
				attr.server = 0;
				inet_pton(AF_INET, optarg, &attr.addr.sa_in.sin_addr);
				break;
			case 's':
				attr.server = 10;
				inet_pton(AF_INET, "0.0.0.0", &((struct sockaddr_in*) &attr.addr)->sin_addr);
				break;
			case 'p':
				((struct sockaddr_in*) &attr.addr)->sin_port = htons(atoi(optarg));
				break;
			default:
				ERROR_LOG("Failed to parse arguments");
				print_help(argv);
				exit(EINVAL);
		}
	}

	if (attr.server == -1) {
		ERROR_LOG("must be either a client or a server!");
		print_help(argv);
		exit(EINVAL);
	}

	TEST_Z(libercat_init(&trans, &attr));

	if (!trans)
		exit(-1);


	if (trans->server) {
		TEST_Z(libercat_bind_server(trans));
		trans = libercat_accept_one(trans);
		TEST_Z(libercat_start_cm_thread(trans));
	} else { //client
		TEST_Z(libercat_connect(trans));
	}

	TEST_NZ(rdmabuf = malloc((RECV_NUM+2)*CHUNK_SIZE*sizeof(char)));
	memset(rdmabuf, 0, (RECV_NUM+2)*CHUNK_SIZE*sizeof(char));
	TEST_NZ(mr = libercat_reg_mr(trans, rdmabuf, (RECV_NUM+2)*CHUNK_SIZE*sizeof(char), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ));



	libercat_data_t *ackdata;
	TEST_NZ(ackdata = malloc(sizeof(libercat_data_t)));
	ackdata->data = rdmabuf+(RECV_NUM+1)*CHUNK_SIZE*sizeof(char);
	ackdata->max_size = CHUNK_SIZE*sizeof(char);
	ackdata->size = 1;
	ackdata->data[0] = 0;

	pthread_mutex_t lock;
	pthread_cond_t cond;

	pthread_mutex_init(&lock, NULL);
	pthread_cond_init(&cond, NULL);

	libercat_data_t **rdata;
	struct datamr *datamr;
	int i;

	TEST_NZ(rdata = malloc(RECV_NUM*sizeof(libercat_data_t*)));
	TEST_NZ(datamr = malloc(RECV_NUM*sizeof(struct datamr)));

	for (i=0; i < RECV_NUM; i++) {
		TEST_NZ(rdata[i] = malloc(sizeof(libercat_data_t)));
		rdata[i]->data=rdmabuf+i*CHUNK_SIZE*sizeof(char);
		rdata[i]->max_size=CHUNK_SIZE*sizeof(char);
		rdata[i]->size=CHUNK_SIZE*sizeof(char);
		datamr[i].data = rdata[i];
		datamr[i].mr = mr;
		datamr[i].lock = &lock;
		datamr[i].cond = &cond;
	}

	pthread_mutex_lock(&lock);
	TEST_Z(libercat_post_recv(trans, &(rdata[0]), mr, callback_recv, &(datamr[0]))); // post only one, others will be used for reads

	if (trans->server) {
		TEST_Z(libercat_finalize_accept(trans));
	} else {
		TEST_Z(libercat_finalize_connect(trans));
	}

	TEST_NZ(wdata = malloc(sizeof(libercat_data_t)));
	wdata->data = rdmabuf+RECV_NUM*CHUNK_SIZE*sizeof(char);
	wdata->max_size = CHUNK_SIZE*sizeof(char);

	libercat_rloc_t *rloc;

	if (trans->server) {
		printf("wait for rloc\n");
		TEST_Z(pthread_cond_wait(&cond, &lock)); // receive rloc

		TEST_NZ(rloc = malloc(sizeof(libercat_rloc_t)));
		memcpy(rloc, (rdata[0])->data, sizeof(libercat_rloc_t));
		printf("got rloc! key: %u, addr: %lu, size: %d\n", rloc->rkey, rloc->raddr, rloc->size);

		volatile int count = 0;

		for (i=0; i < RECV_NUM; i++) {
			datamr[i].rloc = rloc;
			datamr[i].count = &count;
			TEST_Z(libercat_post_RW(trans, rdata[i], mr, rloc, callback_read, &(datamr[i])));
		}

		while (count < 10000) {
			pthread_cond_wait(&cond, &lock);
			if (count%100 == 0)
				printf("count: %d\n", count);
		}

		printf("count: %d\n", count);

		wdata->size = 1;
		TEST_Z(libercat_wait_send(trans, wdata, mr)); // ack - other can quit


	} else {
		rloc = libercat_make_rloc(mr, (uint64_t)ackdata->data, ackdata->max_size);

		memcpy(wdata->data, rloc, sizeof(libercat_rloc_t));
		wdata->size = sizeof(libercat_rloc_t);
		libercat_post_send(trans, wdata, mr, NULL, NULL);

void *write_thread(libercat_data_t *data) {
	int i = 0;

	srand(42);

	while (1) {
		i = (i+13)%data->max_size;
		data->data[i] = (char) rand();
	}

	return NULL;

}

		pthread_t id;
		pthread_create(&id, NULL, (void*(*)(void*)) write_thread, ackdata);
	
		printf("sent rloc, waiting for server to say they're done\n");
		TEST_Z(pthread_cond_wait(&cond, &lock)); // receive server ack (they wrote stuff)

	}
	pthread_mutex_unlock(&lock);


	libercat_destroy_trans(&trans);

	return 0;
}
