/* BEGIN_ICS_COPYRIGHT2 ****************************************

Copyright (c) 2015, Intel Corporation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

** END_ICS_COPYRIGHT2   ****************************************/

/* [ICS VERSION STRING: unknown] */

#ifndef _FE_NET_BLOB_H_
#define _FE_NET_BLOB_H_

/*
 * A net_blob encapsulates the raw data of a message along with state
 * information needed to retrieve the message.
 *
 * On send, we pack a magic#,len in front of the user data.
 * On recv, we read magic#,len into magic[] then put user data into data.
 * Be careful about this difference!
 */
struct net_blob {
	size_t   len;              /* length of what data points to */
	uint8_t  *data;            /* ptr to user data (recv) or magic#,len,userdata (send) */
	ssize_t   bytes_left;       /* # bytes of this msg left to send/recv */
	uint8_t  *cur_ptr;         /* ptr into buffer where next byte sent/recvd will go */
	uint32_t magic[2];         /* buffer for reading magic#, message len */
	struct net_blob *next; /* next blob in the queue */
};

/* Function prototypes */
struct net_blob *fe_oob_new_net_blob(size_t len);
void fe_oob_free_net_buf(char *buf);
void fe_oob_free_net_blob(struct net_blob *blob);
void fe_oob_adjust_blob_cur_ptr(struct net_blob *blob, int bytes_sent);

#endif /* _FE_NET_BLOB_H_ */
