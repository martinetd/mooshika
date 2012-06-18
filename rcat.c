
#include <netinet/in.h>

#if 0
#include <sys/socket.h> //sockaddr
#include <stdio.h>	//printf
#include <stdlib.h>	//malloc
#include <string.h>	//memcpy
#include <inttypes.h>	//uint*_t
#include <errno.h>	//ENOMEM
#include <pthread.h>	//pthread_* (think it's included by another one)
#endif 

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "log.h"
#include "trans_rdma.h"

int main(int argc, char **argv) {


	struct sockaddr_in addr;

	addr.sin_family = AF_INET;
	addr.sin_port = 1234;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	
/*        if (trans->sin.ss_family == AF_INET)
                ((struct sockaddr_in *) &cb->sin)->sin_port = port;
        else
                ((struct sockaddr_in6 *) &cb->sin)->sin6_port = port;
*/

	libercat_trans_t *trans;

	trans = libercat_create((struct sockaddr_storage*) &addr);

	return 0;
}
