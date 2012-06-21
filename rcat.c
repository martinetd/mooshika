
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy

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

#define CHUNK_SIZE 256
#define RECV_NUM 45

#define TEST_Z(x) do { if ( (x)) ERROR_LOG("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_NZ(x)  do { if (!(x)) ERROR_LOG("error: " #x " failed (returned zero/null)."); } while (0)

struct datamr {
	void *data;
	struct ibv_mr *mr;
};

void callback_send(libercat_trans_t *trans, void *arg) {

}

void callback_recv(libercat_trans_t *trans, void *arg) {
	struct datamr *datamr = arg;
	libercat_data_t **pdata = datamr->data;

	printf("%s", (char*)(*pdata)->data);

	libercat_recv(trans, pdata, datamr->mr, callback_recv, datamr);
}

int main(int argc, char **argv) {


	struct sockaddr_in addr;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(1235);
	inet_pton(AF_INET, "10.0.2.22", &addr.sin_addr);

	libercat_trans_t *trans;
	uint8_t *rdmabuf;
	struct ibv_mr *mr;

	libercat_data_t *wdata;

	if (argc == 1) { //client, no argument
		trans = libercat_connect((struct sockaddr_storage*) &addr);
	} else { // server
		trans = libercat_create((struct sockaddr_storage*) &addr);
		trans = libercat_accept_one(trans);
		//TODO split accept_one in two and post receive requests before the final rdma_accept call
	}

	TEST_NZ(rdmabuf = malloc((RECV_NUM+1)*CHUNK_SIZE*sizeof(char)));
	memset(rdmabuf, 0, (RECV_NUM+1)*CHUNK_SIZE*sizeof(char));
	TEST_NZ(mr = libercat_reg_mr(trans, rdmabuf, (RECV_NUM+1)*CHUNK_SIZE*sizeof(char), IBV_ACCESS_LOCAL_WRITE));

#if 1
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
		TEST_Z(libercat_recv(trans, &(rdata[i]), mr, callback_recv, &(datamr[i])));
	}
#endif
	
	if (trans->server) {
//		TEST_Z(libercat_accept(trans));
	}

	TEST_NZ(wdata = malloc(sizeof(libercat_data_t)));
	wdata->data = rdmabuf+RECV_NUM*CHUNK_SIZE*sizeof(char);
	wdata->size = CHUNK_SIZE*sizeof(char);
	while (1) {
		if (!fgets((char*)wdata->data, CHUNK_SIZE, stdin))
			break;
//		TEST_Z(libercat_send(trans, wdata, mr, callback_send, wdata));
		TEST_Z(libercat_send_wait(trans, wdata, mr));
	}

	libercat_destroy_trans(trans);


	return 0;
}
