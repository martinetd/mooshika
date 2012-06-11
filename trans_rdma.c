/*
 *
 * Copyright CEA/DAM/DIF (2012)
 * contributor : Dominique Martinet  dominique.martinet.ocre@cea.fr //TODO: use a real mail?
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * ---------------------------------------
 */

/**
 * \file    trans_rdma.c
 * \brief   rdma helper
 *
 * This is (very) loosely based on a mix of diod, rping (librdmacm/examples)
 * and kernel's net/9p/trans_rdma.c
 *
 */


#include <inttypes.h>

#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>

#include "trans_rdma.h"

/**
 * rdma_recv: Post a receive buffer.
 *
 * Need to post recv buffers before the opposite side tries to send anything!
 * @param trans the trans thing! duh, obviously.
 * @param msize max size we can receive
 * @param callback function that'll be called with the received data
 *
 * @return 0 on success, dunno what yet on error.
 */
int rdma_recv(rdma_trans* trans, u32 msize, void (*callback)(rdma_data* data)) {
	return 0;
}

/**
 * Post a send buffer.
 * Same deal
 *
 */
int rdma_send(rdma_trans* trans, rdma_data* data, void (*callback)(void));

/**
 * Post a receive buffer and waits for _that one and not any other_ to be filled.
 * bad idea. do we want that one? Or place it on top of the queue? But sucks with asynchronism really
 */
int rdma_recv_wait(rdma_trans* trans, rdma_data** datap, u32 msize);

/**
 * Post a send buffer and waits for that one to be completely sent
 * @param trans
 * @param data the size + opaque data.
 */
int rdma_send_wait(rdma_trans* trans, rdma_data* data);

// callbacks would all be run in a big send/recv_thread
// OR no callback, but two recv/send_thread that just waits for semaphore an' are left to the client.
// (or spawn callbacks as a separate thread, but that's overkill)


// server specific:
/**
 *
 */
int rdma_write(trans, rdma_rloc, size);
int rdma_read(trans, rdma_rloc, size);

// client specific:
int rdma_write_request(trans, rdma_rloc, size); // = ask for rdma_write server side ~= rdma_read
int rdma_read_request(trans, rdma_rloc, size); // = ask for rdma_read server side ~= rdma_write
