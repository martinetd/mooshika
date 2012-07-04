
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <unistd.h>     //sleep

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "log.h"
#include "trans_rdma.h"

int main(int argc, char **argv) {
	struct sockaddr_in addr;
	libercat_trans_t *trans;
        struct ibv_mr *mr;
        libercat_data_t *rdata, *wdata;
	char *a;

	if (argc < 2) {
		ERROR_LOG("usage: %s <server ip>", argv[0]);
		return -1;
	}

        a = malloc(1000*sizeof(char));
        rdata = malloc(sizeof(libercat_data_t));
	wdata = malloc(sizeof(libercat_data_t));


	addr.sin_family = AF_INET;
	addr.sin_port = htons(1235);
	inet_pton(AF_INET, argv[1], &addr.sin_addr);


	trans = libercat_connect((struct sockaddr_storage*) &addr);

	mr = libercat_reg_mr(trans, a, 1000*sizeof(char), IBV_ACCESS_LOCAL_WRITE);


        rdata->data=mr->addr;
        rdata->size=100*sizeof(char);
        libercat_recv_wait(trans, &rdata, mr);
	INFO_LOG("recv'd data: %s", rdata->data);


	wdata->data = mr->addr+100*sizeof(char);
        wdata->size=100*sizeof(char);
	strcpy((char*)wdata->data, "Message from client side (wait)");
	sleep(1);
	libercat_send_wait(trans, wdata, mr);
	INFO_LOG("sent data: %s", wdata->data);

	return 0;
}
