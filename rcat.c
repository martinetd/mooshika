
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <unistd.h>	//read

#if 0
#include <sys/socket.h> //sockaddr
#include <inttypes.h>	//uint*_t
#include <errno.h>	//ENOMEM
#include <pthread.h>	//pthread_* (think it's included by another one)
#endif 

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "log.h"
#include "trans_rdma.h"

#define CHUNK_SIZE 512
#define RECV_NUM 30

#define TEST_Z(x)  do { if ( (x)) ERROR_LOG("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_NZ(x) do { if (!(x)) ERROR_LOG("error: " #x " failed (returned zero/null)."); } while (0)

struct datamr {
	void *data;
	struct ibv_mr *mr;
};

void callback_send(libercat_trans_t *trans, void *arg) {

}

void callback_recv(libercat_trans_t *trans, void *arg) {
	struct datamr *datamr = arg;
	libercat_data_t **pdata = datamr->data;

	write(1, (char *)(*pdata)->data, (*pdata)->size);
	fflush(stdout);

	(*pdata)->size = CHUNK_SIZE;

	libercat_post_recv(trans, pdata, datamr->mr, callback_recv, datamr);
}

int main(int argc, char **argv) {


	libercat_trans_t *trans;
	uint8_t *rdmabuf;
	struct ibv_mr *mr;

	libercat_data_t *wdata;

	TEST_Z(libercat_init(&trans));

	if (!trans)
		exit(-1);

	trans->rq_depth = RECV_NUM+1;

	((struct sockaddr_in*) &trans->addr)->sin_family = AF_INET;
	((struct sockaddr_in*) &trans->addr)->sin_port = htons(1235);
	inet_pton(AF_INET, "10.0.2.22", &((struct sockaddr_in*) &trans->addr)->sin_addr);

	if (argc == 1) { //client, no argument
		TEST_Z(libercat_connect(trans));
	} else { // server
		TEST_Z(libercat_bind_server(trans));
		trans = libercat_accept_one(trans);
		//TODO split accept_one in two and post receive requests before the final rdma_accept call
	}

	TEST_NZ(rdmabuf = malloc((RECV_NUM+1)*CHUNK_SIZE*sizeof(char)));
	memset(rdmabuf, 0, (RECV_NUM+1)*CHUNK_SIZE*sizeof(char));
	TEST_NZ(mr = libercat_reg_mr(trans, rdmabuf, (RECV_NUM+1)*CHUNK_SIZE*sizeof(char), IBV_ACCESS_LOCAL_WRITE));

	libercat_data_t **rdata;
	struct datamr *datamr;
	int i;

	TEST_NZ(rdata = malloc(RECV_NUM*sizeof(libercat_data_t*)));
	TEST_NZ(datamr = malloc(RECV_NUM*sizeof(struct datamr)));

	for (i=0; i < RECV_NUM; i++) {
		TEST_NZ(rdata[i] = malloc(sizeof(libercat_data_t)));
		rdata[i]->data=rdmabuf+i*CHUNK_SIZE*sizeof(char);
		rdata[i]->size=CHUNK_SIZE*sizeof(char);
		datamr[i].data = (void*)&(rdata[i]);
		datamr[i].mr = mr;
		TEST_Z(libercat_post_recv(trans, &(rdata[i]), mr, callback_recv, &(datamr[i])));
	}
	
	if (trans->server) {
//		TEST_Z(libercat_accept(trans));
	}

	TEST_NZ(wdata = malloc(sizeof(libercat_data_t)));
	wdata->data = rdmabuf+RECV_NUM*CHUNK_SIZE*sizeof(char);
	wdata->size = CHUNK_SIZE*sizeof(char);

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(0, &rfds);

	while (1) {

		if (select(1, &rfds, NULL, NULL, NULL) == -1)
			break;
		i = read(0, (char*)wdata->data, CHUNK_SIZE);
		if (i == 0)
			break;
		wdata->size = i*sizeof(char);

		TEST_Z(libercat_wait_send(trans, wdata, mr));
	}


	libercat_destroy_trans(trans);

	return 0;
}
