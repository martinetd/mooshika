
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

void callback_send(libercat_trans_t *trans, libercat_data_t *pdata) {

        INFO_LOG("got data: %s", (char*)pdata->data);
}

void callback_recv(libercat_trans_t *trans, libercat_data_t **pdata) {

        INFO_LOG("got data: %s", (char*)(*pdata)->data);
}

int main(int argc, char **argv) {


	struct sockaddr_in addr;

	addr.sin_family = AF_INET;
	addr.sin_port = htons(1235);
	inet_pton(AF_INET, "10.0.2.22", &addr.sin_addr);

/*        if (trans->sin.ss_family == AF_INET)
                ((struct sockaddr_in *) &cb->sin)->sin_port = port;
        else
                ((struct sockaddr_in6 *) &cb->sin)->sin6_port = port;
*/

	libercat_trans_t *trans;
        char *a = malloc(1000*sizeof(char));
        struct ibv_mr *mr;

        libercat_data_t *rdata, *wdata;
        rdata = malloc(sizeof(libercat_data_t));
	wdata = malloc(sizeof(libercat_data_t));

	if (argc == 1) { //client, no argument
		trans = libercat_connect((struct sockaddr_storage*) &addr);

	} else { // server

	        trans = libercat_create((struct sockaddr_storage*) &addr);
        	trans = libercat_accept_one(trans);
	}

	mr = libercat_reg_mr(trans, a, 1000*sizeof(char), IBV_ACCESS_LOCAL_WRITE);
        rdata->data=mr->addr;
        rdata->size=100*sizeof(char);
        libercat_recv(trans, &rdata, mr, callback_recv);

#include <unistd.h>
	sleep(1);

	wdata->data = mr->addr+100*sizeof(char);
        wdata->size=100*sizeof(char);
	
	strcpy((char*)wdata->data, "wooooot, I am text, I'm aliiiiive");
	libercat_send(trans, wdata, mr, callback_send);


	sleep(3000);
	return 0;
}
