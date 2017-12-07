/* BEGIN_ICS_COPYRIGHT3 ****************************************

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

** END_ICS_COPYRIGHT3   ****************************************/

/* [ICS VERSION STRING: unknown] */

#ifndef _IB_UTILS_OPENIB_H_
#define _IB_UTILS_OPENIB_H_

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <infiniband/umad.h>
#include <syslog.h>
#include <oib_utils.h>

#include "iba/public/datatypes.h"
#include "iba/public/isemaphore.h"
#include "iba/ib_types.h"

#define OIB_MEMORY_TAG        ((uint32)0x4F50454E) /* OPEN */

#ifndef min
#define min(a,b) (((a)<(b))?(a):(b))
#endif
#ifndef max
#define max(a,b) (((a)>(b))?(a):(b))
#endif

	/* filled in in C code so we don't need oib headers here */
#define OIB_SEND_TIMEOUT_DEFAULT -1

/* compute max timeout value to use in umad_recv based on
 * timeout and retries specified in corresponding umad_send
 * we add 1000ms (1 second) to allow for delays in local HFI and stack
 * in issuing retries, etc.
 */
#define COMPUTE_MAX_TIMEOUT(timeout, retries) (((timeout)*((retries)+1))+1000)

#define OUI_TRUESCALE_0 (0x00) /* 3-byte InfiniCon (now Intel) OUI */
#define OUI_TRUESCALE_1 (0x06)
#define OUI_TRUESCALE_2 (0x6a)


/**
 * Initailize global elements to be used for queries and access to OFED stack.    
 * 
 * There are 3 different ways to call init, all 3 have the same effect - 
 *   they initialize all the global vars to be used later for oib
 *   (ofed to ib access) utilities.
 *   They also initialize the umad interface.
 * 
 * The three ways to initialize are
 *  initialize via port guid, [hfi / port specific, no wildcards]
 *  initialize via hfi name and port num [hfi specific, port can be wildcard (0)]
 *  initialize via hfi num and port num [hfi / port specific, or wildcards (0)]
 * 
 * Wildcards for hfi num or port num look for ACTIVE ports.
 * Specific ports may be inactive, although not all globals may be initialized 
 *  in that case.
 *
 * @param hfiName   - pointer to name of HFI
 * @param hfi       - number of HFI to initialize  globals 
 * @param port      - number of port to initialize globals
 * @param port_guid - guid to initialize globals (can be invalid (-1))
 *
 * @return FSTATUS  - error if could not find the port (or any active port for wildcards)
 */

#define OIB_GLOBAL_PORTID -1


/**
 * Return the the hfi name and hfi port for the given port guid.
 * Return value is success / failure.
 *
 * @param portGuid
 * @param pointer to hfiName
 * @param pointer to ca number (1=1st CA)
 * @param pointer to port number (1=1st port)
 * @param pointer to port gid prefix
 * @param pointer to port sm Lid
 *
 * @return 0 for success, else error code
 */
extern FSTATUS oib_get_hfi_from_portguid(IN uint64_t portGuid,
                                         OUT char *pCaName,
                                         OUT int * caNum,
                                         OUT int * portNum,
                                         OUT uint64_t * pPrefix,
                                         OUT uint16_t * pSMLid,
                                         OUT uint8_t * pSMSL,
										 OUT uint8_t * pPortState);


/**
 * Return the GUIDs for the FIs.  The GUID array pointer must be null and
 * will always get filled-in, sets the number of FIs in the system
 * regardless of the initial value of the
 * count.
 *
 * @param pCaCount
 * @param pCaGuidArray
 *
 * @return FSTATUS
 */
extern FSTATUS oib_get_caguids( OUT uint32 *pCaCount, OUT uint64 **pCaGuidArray );


/* Find the ca indicated by the passed in CA guid, then allocate 
 * and fill in the ca attributes.
 *                               *
 * INPUTS:
 * 	caGuid - guid of the desired ca
 *
 * OUTPUTS:
 * 	*pCaAttributes - attributes for CA,
 * 					If PortAttributesList not null, caller must MemoryDeallocate
 *                  pCaAttributes->PortAttributesList
 *                  Meant to be equivalent for iba_get_caguids.
 *
 * RETURNS:
 * 	FNOT_FOUND 
 *
 */
extern FSTATUS oib_query_ca_by_guid_alloc(IN  EUI64 CaGuid,OUT IB_CA_ATTRIBUTES *CaAttributes );
	
/* Find the ca indicated by the passed in CA guid, then 
 * fill in the ca attributes.
 *                           
 * Note: It is assumed that the CaAttributes is already populated with 
 * allocated memory for pCaAttributes->PortAttributesList
 *                           
 * INPUTS:
 *  caGuid - guid of the desired ca
 *
 * OUTPUTS:
 *  *pCaAttributes - attributes for CA,
 *                  caller responsible for allocating/deallocating memory for
 *                  pCaAttributes->PortAttributesList
 *
 * RETURNS:
 *  FNOT_FOUND 
 *
 */

extern FSTATUS oib_query_ca_by_guid(IN EUI64 CaGuid, OUT IB_CA_ATTRIBUTES *CaAttributes );

/**
 * Function to register a list of managment classes with the umad OFED
 * library.  
 * 
 * To use with oib_send_rcv_mad, and the umad OFED library.
 * 
 * User must have called oib_mad_init first. Otherwise - call fails
 * 
 * User may call this fn multiple times to register for different classes.
 * If the class is already registered, a warning is printed, but the 
 * call succeeds.
 * 
 * @param  *mgmt_classes pointer to a list of managment classes to be registered.
 *						 The list is terminated by a record that has 0 for the 
 *						 base_version.
 * 
 * Returns 0 if successful, else error code.
 */ 
#define OIB_CLASS_MAX_METHODS 8

typedef struct {
	int	base_version; 	// IB_BASE_VERSION or STL_BASE_VERSION
	int	class;			// MCLASS_SUBN_ADM, STL_SA_CLASS, etc...
	int	classVersion;	// IB_BM_CLASS_VERSION, STL_SA_CLASS_VERSION, etc...
	int useMethods;		// if Nonzero, use the methods list instead of the defaults.
	int isResp;			// nonzero if registering for rx respones
	int isTrap;			// nonzero if registering for rx Traps
	int isReport;		// nonzero if registering for rx reports
	int stackRMPP;		// nonzero if OFED-style rmpp support is desired.
	uint8_t *oui;		// vendor-specific OUI for vendor classes.
	int methods[OIB_CLASS_MAX_METHODS]; // 0 terminated list of methods.
} OIB_CLASS_ARGS;

/*
 * ib_utils_user_sa.c
 */

extern FILE *dbg_file;
extern FILE *error_file;

/* stderr is no longer a constant which can be assigned to our global */
#ifndef OUTPUT_ERROR
#define OUTPUT_ERROR(format, args...) \
	do { \
		if (!error_file) \
			error_file = stderr; \
		if (error_file == OIB_DBG_FILE_SYSLOG) { \
			syslog(LOG_ERR, "oib_utils ERROR: [%d] %s: " format, \
				(int)getpid(), __func__, ##args); \
		} else { \
			fprintf(error_file, "oib_utils ERROR: [%d] %s: " format, \
				(int)getpid(), __func__, ##args); \
		} \
	} while(0)
#endif

/* NOTE we keep this at LOG_INFO and reserve LOG_DEBUG for packet dump */
#ifndef DBGPRINT 
#define DBGPRINT(format, args...) \
	do { \
		if (dbg_file) { \
			if (dbg_file == OIB_DBG_FILE_SYSLOG) { \
				syslog(LOG_INFO, "oib_utils: [%d] %s: " format, \
				    (int)getpid(), __func__, ##args); \
			} else { \
				fflush(dbg_file); fprintf(dbg_file, "oib_utils: [%d] %s: " format, \
				    (int)getpid(), __func__, ##args); \
			} \
		} \
	} while(0)
#endif

#ifndef OUTPUT_INFO
#define OUTPUT_INFO(format, args...) \
	do { \
		if (dbg_file) { \
			if (dbg_file == OIB_DBG_FILE_SYSLOG) { \
				syslog(LOG_INFO, "oib_utils: [%d] %s: " format, \
				    (int)getpid(), __func__, ##args); \
			} else { \
				fprintf(dbg_file, "oib_utils: [%d] %s: " format, \
				    (int)getpid(), __func__, ##args); \
			} \
		} \
	} while(0)
#endif

#ifndef DBG_ENTER_FUNC
#define DBG_ENTER_FUNC  DBGPRINT("Entering %s\n",__func__)
#endif 

#ifndef DBG_EXIT_FUNC
#define DBG_EXIT_FUNC  DBGPRINT("Exiting %s\n",__func__)
#endif 

/** =========================================================================
 * As we remove the above functionality from the external interface we will
 * want this functionality to remain available via internal header file.
 * Move that functionaltiy here to protect against further ussage and we can
 * rename this file after the conversion.
 */

#ifdef OIB_UTILS_PRIVATE

#include <infiniband/verbs.h>

#define OIB_MAX_CLASS           256
#define OIB_MAX_CLASS_VERSION   256
#define OIB_MAX_PKEYS           256
#define OIB_DEFAULT_PKEY        0xffff
#define OIB_PKEY_MASK           0x7fff
#define OIB_PKEY_FULL_BIT       0x8000
#define OIB_INVALID_AGENTID     -1
#define OIB_UTILS_DEF_TIMEOUT_MS 1000
#define DEFAULT_USERSPACE_RECV_BUF  512
#define DEFAULT_USERSPACE_SEND_BUF  128
#define NOTICE_REG_TIMEOUT_MS   1000 /* 1sec */
#define NOTICE_REG_RETRY_COUNT  15

enum oib_reg_retry_state
{
	OIB_RRS_SEND_INITIAL	= 0,
	OIB_RRS_SEND_RETRY,
};

enum oib_th_event
{
    OIB_TH_EVT_NONE = 0,
    OIB_TH_EVT_SHUTDOWN,
    OIB_TH_EVT_UD_MONITOR_ON,
    OIB_TH_EVT_UD_MONITOR_OFF,
    OIB_TH_EVT_START_OUTSTANDING_REQ_TIME,
    OIB_TH_EVT_TRAP_MSG,
    OIB_TH_EVT_TRAP_REG_ERR_TIMEOUT,
};

struct oib_thread_msg
{
    size_t            size;
    enum oib_th_event evt;
    uint8_t           data[0];
};

#define IBUSA_NOTICE_SUP_ENV "OIB_IBUSA_NOTICE"

struct ibv_sa_id;
struct oib_sa_msg;

typedef struct _oib_sa_registration
{
    uint16_t                     trap_num;
    struct ibv_sa_id            *id;
    void                        *user_context; /* for userspace notice, id == NULL */
    struct oib_sa_msg           *reg_msg;
    struct _oib_sa_registration *next;
} oib_sa_registration_t;

struct oib_sa_msg {
    struct oib_sa_msg      *prev;
    struct oib_sa_msg      *next;
    struct ibv_mr          *mr;
    struct ibv_sge          sge;
    union {
        struct ibv_send_wr  send;
        struct ibv_recv_wr  recv;
    } wr;
    int                     retries;
    int                     status;
    int                     in_q;
    oib_sa_registration_t  *reg;
    uint8_t                 data[2048];
};

/** ========================================================================= */
struct oib_port {
	int                  hfi_num;
	char                 hfi_name[IBV_SYSFS_NAME_MAX];
	uint8_t              hfi_port_num;
	int                  umad_fd;
	int                  umad_agents[OIB_MAX_CLASS_VERSION][OIB_MAX_CLASS];
	struct ibv_context  *verbs_ctx;

	/* from oib_sa_registry_t object */
	struct ibv_sa_event_channel *channel;
	oib_sa_registration_t       *regs_list;
	SEMAPHORE                    lock;

	/* cache of "port details" */
    SEMAPHORE   umad_port_cache_lock;
    umad_port_t umad_port_cache;
    int         umad_port_cache_valid;
    pthread_t   umad_port_thread;
    int         umad_port_sv[2];
    

    /* SA interaction for userspace Notice registration */
    struct ibv_comp_channel *sa_qp_comp_channel;
    struct ibv_cq           *sa_qp_cq;
    struct ibv_pd           *sa_qp_pd;
    struct ibv_qp           *sa_qp;
    struct ibv_ah           *sa_ah;
    uint32_t                 next_tid; /* 32 bits only */
    int                      run_sa_cq_poll;
    int                      poll_timeout_ms;

    int                      num_userspace_recv_buf;
    int                      num_userspace_send_buf;
    int                      outstanding_sends_cnt;
    struct oib_sa_msg        pending_reg_msg_head;
    struct oib_sa_msg       *recv_bufs;

    /* For PA client interface */
    IB_GID              local_gid;
    FILE                *verbose_file;
    int                 pa_verbose;
    int                 pa_client_state;
    uint16_t            primary_pm_lid;
    uint8_t             primary_pm_sl;
    uint16_t            mad_status;
};

int oib_get_hfiNum (IN char * hfiName);

/**
 * Clears registration structures
 *
 * @param port      port opened by oib_open_port_*
 *
 * @return none
 **/
void oib_sa_clear_regs_unsafe(struct oib_port *port);

int oib_lock_sem(SEMAPHORE* const	pSema);
void oib_unlock_sem(SEMAPHORE * const pSema);
void oib_sa_unreg_all(struct oib_port *port);

void handle_sa_ud_qp(struct oib_port *port);
int stop_ud_cq_monitor(struct oib_port *port);
int repost_pending_registrations(struct oib_port *port);
int reregister_traps(struct oib_port *port);
void oib_sa_remove_all_pending_reg_msgs(struct oib_port *port);

#define LIST_INIT(obj) \
    (obj)->next = obj; \
    (obj)->prev = obj;

#define LIST_ADD(head, new_obj) \
    (head)->prev->next = (new_obj); \
    (new_obj)->next = (head); \
    (new_obj)->prev = (head)->prev; \
    (head)->prev = (new_obj);

#define LIST_DEL(obj) \
    (obj)->prev->next = (obj)->next; \
    (obj)->next->prev = (obj)->prev; \
    LIST_INIT(obj);

#define LIST_FOR_EACH(head, item) \
    for (item = (head)->next; item != (head); item = item->next)

#define LIST_EMPTY(head) \
    ((head) == (head)->next)

#endif /* OIB_UTILS_PRIVATE */

#endif /* _IB_UTILS_OPENIB_H_ */
