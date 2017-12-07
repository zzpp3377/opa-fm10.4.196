/* BEGIN_ICS_COPYRIGHT7 ****************************************

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

** END_ICS_COPYRIGHT7   ****************************************/

/* [ICS VERSION STRING: unknown] */

//===========================================================================//
//									     //
// FILE NAME								     //
//    topology_thread.c							     //
//									     //
// DESCRIPTION								     //
//    This thread will do the bulk of the work.  It discovers the	     //
//    the topology, initializes the FIs and switches, and then		     //
//    tells the state machine to transition to another state.		     //
//									     //
// DATA STRUCTURES							     //
//    None								     //
//									     //
// FUNCTIONS								     //
//    topology_thread			main entry point		     //
//    topology_thread_discovery		topology discovery		     //
//    topology_thread_resolve		resolve PS with new info	     //
//    topology_thread_assignments	assign LIDs, GIDs, etc		     //
//    topology_thread_paths		calculate paths and LFTs	     //
//    topology_thread_transition	send event to update SM state 	     //
//									     //
// DEPENDENCIES								     //
//    ib_mad.h								     //
//    ib_status.h							     //
//    ib_const.h							     //
//									     //
//									     //
//===========================================================================//

#include "os_g.h"
#include "ib_types.h"
#include "ib_macros.h"
#include "ib_mad.h"
#include "ib_sa.h"
#include "ib_status.h"
#include "cs_g.h"
#include "cs_csm_log.h"
#include "sm_counters.h"
#include "sm_l.h"
#include "sa_l.h"
#include "cs_queue.h"
#include "cs_bitset.h"
#include "sm_dbsync.h"
#include "stl_cca.h"
#include "ispinlock.h"
#include "topology.h"
#include <stl_helper.h>

#if defined(__VXWORKS__)
#include "bspcommon/h/usrBootManager.h"
#endif
#ifdef BIU
#include "string.h"
#include "sm_dor.h"
#include "sm_dorbiu.h"
#endif

extern uint8_t smTerminateAfter;
extern char*   smDumpCounters;

extern  IBhandle_t  fd_sminfo;

extern	char* printLoopPaths(int, int);
extern char * snprintfcat(char * buf, int * len, const char * format, ...);

#ifdef IB_STACK_OPENIB
#include "mal_g.h"
#endif

#define PREFETCH_ENABLED 1 /* change to 0 to disable prefetching, 1 to enable prefetching */

#if PREFETCH_ENABLED
#if defined(__i386__)
                                                                                
extern inline void prefetchnta(const void *x) {                                                                               
	__asm__ __volatile__ ("prefetchnta (%0)" : : "r"(x));
}                                                                               
 
extern inline void prefetcht0(const void *x) {                                                                               
	__asm__ __volatile__ ("prefetcht0 (%0)" : : "r"(x));                   
}                                                                               

 
#define PREFETCH_NTA(addr) prefetchnta((addr))                                  
#define PREFETCH_T0(addr) prefetcht0((addr))                                    

#elif defined(TARGET_CPU_FAMILY_MIPS)

#include "bspcommon/h/mips.h"
#define PREFETCH_NTA(addr) prefetchForStore32(addr)
#define PREFETCH_TO(addr) prefetchForLoad32(addr)

#else
//#warning Do not know how to prefetch for this environment
#undef PREFETCH_ENABLED
#define PREFETCH_ENABLED 0
#endif
#endif /* defined(PREFETCH_ENABLED) */

#if 0
#if PREFETCH_ENABLED == 0
#warning MEMORY prefetching for Floyd algorithm is not enabled
#define PREFETCH_NTA(addr) ((void)(0))
#define PREFETCH_T0(addr) ((void)(0))
#endif
#endif 

#define PREFETCH PREFETCH_T0

// time after arming when we're in the idle flit propagation window
#define IDLE_FLIT_PROPAGATION_TIME (50 * VTIMER_1_MILLISEC)

int isSweeping = 0;
int	activateInProgress = 0;
int	forceRebalanceNextSweep = 0;
int oldSmActiveCount = 0;
void dump_cost_array(uint16_t *);
void showSmParms(void);

Status_t topology_load_predef(void);
Status_t topology_initialize(void);
Status_t topology_discovery(void);
Status_t topology_transition(void);
Status_t topology_resolve(void);
Status_t topology_assignments(void);
Status_t topology_adaptiverouting(void);
Status_t topology_activate(void);
Status_t topology_changes(Topology_t *old_topo, Topology_t *new_topo);
Status_t topology_TrapUp(STL_NOTICE * noticep, Topology_t *, Topology_t *, Node_t *, Port_t *);
Status_t topology_TrapDown(STL_NOTICE * noticep, Topology_t *, Topology_t *, Node_t *, Port_t *);
Status_t topology_copy(void);
Status_t topology_release_saved_topology(void);
Status_t topology_multicast(void);
Status_t topology_clearNew(void);
Status_t topology_cache_build(void);
Status_t topology_cache_copy(void);
Status_t topology_congestion(void);
Status_t topology_db(void);
Status_t topology_fe(void);
Status_t topology_dump(void);
Status_t topology_loopTest(void);

Status_t topology_setup_routing_cost_matrix(void);
Status_t topology_sm_port_init_failure(void);
Status_t topology_setup_switches_LR_DR(void);
Status_t topology_update_cableinfo(void);


typedef	Status_t  (*TFunc_t)(void);

TFunc_t	topology_functions[] = {
	topology_initialize,		// Initialize the local port.
	topology_discovery,			// Initial exploration of the fabric
	topology_transition,		// Determine if this SM should be MASTER or STANDBY.
	topology_userexit,			// NOOP!
	topology_resolve,			// Sync the old topology with the new one.
	topology_assignments,		// Assign LIDS, build LFTs, PGTs and PGFTs.
	topology_adaptiverouting,	// Transmit PGTs and PGFTs to switches.
	topology_activate,			// Bring all links to active.
    sm_dbsync_upsmlist,			// Update our list of SMs in the fabric
    topology_multicast,			// Build MFTs
	topology_cache_build,		// Builds caches that are used by the SA. 
    topology_loopTest,			// Embedded only. Injects packets into fabric.
	NULL
};

static const char *sweep_reasons[] = {
	[SM_SWEEP_REASON_INIT] = "Initial sweep.",
	[SM_SWEEP_REASON_SCHEDULED] = "Scheduled sweep interval",
	[SM_SWEEP_REASON_RECONFIG] = "FM reconfigured.",
	[SM_SWEEP_REASON_MCMEMBER] = "Multicast group Membership change.",
	[SM_SWEEP_REASON_ACTIVATE_FAIL] = "Failed to activate port(s) in the fabric.",
	[SM_SWEEP_REASON_ROUTING_FAIL] = "Error while programming linear fowarding table(s) in fabric.",
	[SM_SWEEP_REASON_MC_ROUTING_FAIL] = "Error while programming multicast table(s) in fabric.",
	[SM_SWEEP_REASON_UNEXPECTED_BOUNCE] = "Port(s) in fabric bounced unexpectedly during bringup.", 
	[SM_SWEEP_REASON_LOCAL_PORT_FAIL] = "Our SM's port wasn't ready, or went down unexpectedly.",
	[SM_SWEEP_REASON_STATE_TRANSITION] = "Our SM state transitioned to DISCOVERY or MASTER.",
	[SM_SWEEP_REASON_SECONDARY_TROUBLE] = "Abnormal behavior from a secondary SM observed.",
	[SM_SWEEP_REASON_MASTER_TROUBLE] = "Abnormal behavior from the MASTER SM observed.",
	[SM_SWEEP_REASON_UPDATED_STANDBY] = "Status changed for a STANDBY SM in fabric.",
	[SM_SWEEP_REASON_HANDOFF] = "Preparation for handoff to another SM.",
	[SM_SWEEP_REASON_UNEXPECTED_SM] = "Found a SM we haven't seen before.",
	[SM_SWEEP_REASON_FORCED] = "SM sweep forced by user.",
	[SM_SWEEP_REASON_INTERVAL_CHANGE] = "SM sweep interval changed by user.",
	[SM_SWEEP_REASON_FAILED_SWEEP] = "Problems during previous sweep, retrying.",
	[SM_SWEEP_REASON_TRAP_EVENT] = "Trap event occurred that requires re-sweep.",
	[SM_SWEEP_REASON_UNDETERMINED] = "No reason was specified (WARNING: abormal!)."
};

SweepReason_t sm_resweep_reason = SM_SWEEP_REASON_INIT;

uint64_t	topology_wakeup_time;

int	topo_retry_backoff = 0;		/*Default behavior - no retry back off*/
uint32_t topo_retry_backoff_interval = 0;

static SmCsmMsgType_t nodeAppearanceSeverity = CSM_SEV_NOTICE;
static int sweepNodeChangeMsgCount = 0;
static int sweepNodeAppearanceInfoMsgCount = 0;
static int sweepNodeDisappearanceInfoMsgCount = 0;

Topology_t	old_topology;
Topology_t	sm_newTopology;
Topology_t	save_topology;
Topology_t	*sm_topop = &sm_newTopology;

FabricData_t preDefTopology;

bitset_t    old_switchesInUse;
bitset_t    new_switchesInUse;
bitset_t    new_endnodesInUse;
int     topo_errors=0;              /* number of errors during fabric init */
int     topo_abandon_count=0;       /* number of consecutive times we abandoned sweep */
int		topology_once = -1;
int		topology_changed = 0;
int		topology_switch_port_changes = 0;	/* there are switches with switch port change flag set*/
int		topology_cost_path_changes = 0;
uint8_t topology_set_client_reregistration = 0;
int		topology_changed_count = 0;
uint32_t	topology_passcount = 0ull;
int		routing_recalculated = 0;
int     smSendOutMFTs = 0;              /* send mft changes to switches flag */
sm_dispatch_t sm_asyncDispatch;
uint32_t topology_port_bounce_log_num = 0; // Number of times we have logged a port bounce this sweep

static int topology_resweep = 0; // request to resweep immediately
static ATOMIC_UINT topology_triggered; // true when a sweep has been triggered

// unconditionally send MFTs regardless of previous sweep state
static int topology_forceMfts = 0; 

static  int topology_main_exit = 0;

static int	sm_port_retry_count=0;		/*keep track of sm port initialization retries*/

static int cca_discovery_count = 0;

extern Sema_t topology_sema;

#ifdef __VXWORKS__
#include "bspcommon/h/sysPrintf.h"
void topology_uninitialize(void);
#else
#define sysPrintf printf
#define SYS_PINFO
#endif

// external functions
extern int saSubscriberSize(void);
extern int saServiceRecordSize(void);
extern int saMcGroupSize(void);
extern int saMcMemberSize(void);
extern int saMaxResponseSize(void);
extern int saNodeRecordSize(void);
extern Status_t sm_sa_forward_trap(STL_NOTICE * noticep);
extern char* printSwitchLft(int nodeIdx, int useNew, int haveLock, int buffer);

// externs
extern  cs_Queue_ptr    sm_async_rcv_resp_queue;

VirtualFabrics_t *previousVfPtr;
extern VirtualFabrics_t *updatedVirtualFabrics;

#ifndef __VXWORKS__
extern 	uint32_t 		xml_trace;

static int sm_peer_quarantined = 0;
#endif

static const char*
GetStr_SpeedActive(STL_PORT_INFO *portInfo, char *buf, size_t len)
{
    return (StlLinkSpeedToText(portInfo->LinkSpeed.Active, buf, len));
}

static const char*
GetStr_SpeedSupport(STL_PORT_INFO *portInfo, char *buf, size_t len)
{
    return (StlLinkSpeedToText(portInfo->LinkSpeed.Supported, buf, len));
}

/*
 * clean up the SA tables when we transition to Standby state
 */
static void clearSaTables(void)
{
    /*
     * clear everything if transitioning from master to standby or less
    */
    if (sm_prevState == SM_STATE_MASTER) {
        IB_LOG_INFO0("Clearing lidmap, services, subscriptions, SA context, groups and SM list");
        /* clear the lidmap and reset sm_lid */
        if (vs_wrlock(&old_topology_lock) == VSTATUS_OK) {
			cl_qmap_remove_all(sm_GuidToLidMap);
            memset(lidmap, 0, sizeof(LidMap_t) * (UNICAST_LID_MAX + 1));
            (void)vs_rwunlock(&old_topology_lock);
        }
        // Reset the lid assignment hints
    	sm_lmc_e0_freeLid_hint = 1 << sm_config.lmc_e0;
    	sm_lmc_freeLid_hint = 1 << sm_config.lmc;
    	sm_lmc_0_freeLid_hint = 1;

        sm_lid = sm_config.lid;

        /* clear subscriptions, sa context, broadcast groups, sm table, and service records here */
        (void) sa_SubscriberClear();
        (void) sa_cntxt_clear();
        (void) sa_ServiceRecClear();
        (void) sm_dbsync_upsmlist();        /* clean out all sm records except for ours */
        if (sm_state > SM_STATE_NOTACTIVE) (void)clearBroadcastGroups(TRUE);
    } else {
        IB_LOG_INFO0("Clearing lidmap and the Sm list");
        /* clear the lidmap and reset sm_lid */
        if (vs_wrlock(&old_topology_lock) == VSTATUS_OK) {
			cl_qmap_remove_all(sm_GuidToLidMap);
            memset(lidmap, 0, sizeof(LidMap_t) * (UNICAST_LID_MAX + 1));
            (void)vs_rwunlock(&old_topology_lock);
        }
        // Reset the lid assignment hints
    	sm_lmc_e0_freeLid_hint = 1 << sm_config.lmc_e0;
    	sm_lmc_freeLid_hint = 1 << sm_config.lmc;
    	sm_lmc_0_freeLid_hint = 1;

        /* clear just the SM table otherwise */
        (void) sm_dbsync_upsmlist();        /* clean out all sm records except for ours */
    }
}

//----------------------------------------------------------------------------

boolean sm_uses_PreemptRemap(const Port_t * port)
{
	return 1;
}

//----------------------------------------------------------------------------

/**
    @param compare @c a and @c b 
*/
boolean sm_eq_XmitQ(const struct XmitQ_s * a, const struct XmitQ_s * b, uint8 actVls)
{
	uint8 i;

	for (i = 0; i < MAX(actVls, 16); ++i) {
		if (actVls < 16 && i == actVls)
			i = 15; // Skip to VL15

		if (a[i].VLStallCount != b[i].VLStallCount)
				return 0;

		if (a[i].HOQLife != b[i].HOQLife)
				return 0;
	}

	return 1;
}


//----------------------------------------------------------------------------

int sm_evalPreemptLargePkt(int cfgLarge, Node_t * nodep) 
{
	static boolean report = 1;
	int retVal;

	// The following evaluations are based on PRR limitations
	// Eventually this may be come node specific.
	retVal = cfgLarge / SM_PREEMPT_LARGE_PACKET_INC;
	if (retVal > 1) retVal--;
	if (retVal > 15) retVal = 15;

	// Now check -
	if (report==1) {
		int prrLimit = (retVal*SM_PREEMPT_LARGE_PACKET_INC)+SM_PREEMPT_LARGE_PACKET_INC;

		if (cfgLarge != prrLimit) {
			report=0;
			IB_LOG_WARN_FMT(__func__,
			  "Node %s guid "FMT_U64", User LargePktPreempt changed from %d to %d",
			  sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID,
			  cfgLarge, prrLimit);
		} 
	}

	return retVal;
}

//----------------------------------------------------------------------------

int sm_evalPreemptSmallPkt(int cfgSmall, Node_t * nodep) 
{
	static boolean report = 1;
	int retVal;

	// The following evaluations are based on PRR limitations
	// Eventually this may be come node specific.
	retVal = cfgSmall / SM_PREEMPT_SMALL_PACKET_INC;
	if (retVal < 1) retVal = 1;
	if (retVal > 31) retVal = 31;
	retVal--;
	retVal = ((retVal >> 1) << 1) + 1;  // Outcome: odd, positive vales 1-31

	// Now check -
	if (report==1) {
		int prrLimit = ((retVal>>1)*64) + 64;
		// Handle special bump-up case in PRR ASIC
		if (prrLimit==1024) prrLimit=1152;

		if (cfgSmall != prrLimit) {
			report=0;
			IB_LOG_WARN_FMT(__func__,
			  "Node %s guid "FMT_U64", User SmallPktPreempt changed from %d to %d",
			  sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID,
			  cfgSmall, prrLimit);
		} 
	}

	return retVal;
}

//----------------------------------------------------------------------------

int sm_evalPreemptLimit(int cfgLimit, Node_t * nodep)
{
	static boolean report = 1;
	int retVal;

	// The following evaluations are based on PRR limitations
	// Eventually this may be come node specific.
	retVal = cfgLimit / SM_PREEMPT_LIMIT_INC;

    if (retVal > 255) retVal = 255;
	// Now check -
	if (report==1) {
		int prrLimit = retVal*SM_PREEMPT_LIMIT_INC;

		if (cfgLimit != prrLimit) {
			report=0;
			IB_LOG_WARN_FMT(__func__,
			  "Node %s guid "FMT_U64", User PreemptLimit changed from %d to %d",
			  sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID,
			  cfgLimit, prrLimit);
		} 
	}

	return retVal;
}

//----------------------------------------------------------------------------

static void sm_PreemptRemap(struct Preemption_t * dest, const struct Preemption_t * src)
{
	if (dest != src) {
		memcpy(dest, src, sizeof(struct Preemption_t));
	}
	if (dest->MinInitial > 255) dest->MinInitial = 255;
	if (dest->MinInitial > 0) --dest->MinInitial;
	if (dest->MinInitial < 6) dest->MinInitial = 6;
	dest->MinInitial = ((dest->MinInitial >> 1) + 1) << 1;

	if (dest->MinTail > 255) dest->MinTail = 255;
	if (dest->MinTail > 0) --dest->MinTail;
	if (dest->MinTail < 6) dest->MinTail = 6;
	dest->MinTail = ((dest->MinTail >> 1) + 1) << 1;

	// this isn't actually a remapping or conversion, just limit enforcement
	dest->LargePktLimit = dest->LargePktLimit & 0xF;

	if (dest->SmallPktLimit < 1) dest->SmallPktLimit = 1;
	if (dest->SmallPktLimit > 31) dest->SmallPktLimit = 31;
	--dest->SmallPktLimit;
	dest->SmallPktLimit = ((dest->SmallPktLimit >> 1) << 1) + 1;
}

//----------------------------------------------------------------------------

boolean sm_eq_Preempt(const struct Preemption_t * a, const struct Preemption_t * b, boolean doRemap)
{
	if (!doRemap)
		return memcmp(a, b, sizeof(struct Preemption_t)) == 0;

	struct Preemption_t modA, modB;
	sm_PreemptRemap(&modA, a);
	sm_PreemptRemap(&modB, b);

	return modA.MinInitial == modB.MinInitial &&
		modA.MinTail == modB.MinTail &&
		modA.LargePktLimit == modB.LargePktLimit &&
		modA.SmallPktLimit == modB.SmallPktLimit &&
		modA.PreemptionLimit == modB.PreemptionLimit;
}

//----------------------------------------------------------------------------
// API for interacting with topology_activate()'s retry logic.

typedef struct ActivationRetry {
	uint8_t attempts;
	uint32_t failures;
} ActivationRetry_t;

uint8_t
activation_retry_attempts(pActivationRetry_t retry)
{
	return retry ? retry->attempts : 0;
}

void
activation_retry_inc_failures(pActivationRetry_t retry)
{
	if (retry)
		++ retry->failures;
}

//----------------------------------------------------------------------------

/**
    Update the and head of queue lifetime limit (HLL) values on
    @c nodep and @c portp, respectively. Accounts for 5 bits
    vs. 4 bits for HLL storage on the SMA when determining if an
    update is required but does not convert values.

	@param updHoq [out] Does HOQ need to be updated on this port?

	@return VSTATUS_OK on success or something else on error.
*/

static Status_t UpdateHoq(Node_t * nodep, Port_t * curPort, boolean * updHoq)
{
    VlVfMap_t vlvfmap;
    VlBwMap_t vlbwmap;
    Status_t status = VSTATUS_OK;

    uint32_t maxHoqVals[STL_MAX_VLS];

    uint32_t preemptibleVLs = 0;

    memset(maxHoqVals, 0, sizeof(maxHoqVals));

    // Default VL15 to SM instance values.
    maxHoqVals[15] = sm_config.hoqlife_n2;

    status = sm_topop->routingModule->funcs.select_vlvf_map(sm_topop, nodep, curPort, &vlvfmap);

    if (status != VSTATUS_OK) {
        IB_LOG_ERROR_FMT(__func__,
          "Unable to acquire VF-VL map for node %s guid "FMT_U64", port %d, status = %d",
          sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, curPort->index, status);
        return status;
    }

    status = sm_topop->routingModule->funcs.select_vlbw_map(sm_topop, nodep, curPort, &vlbwmap);
    if (status != VSTATUS_OK) {
        IB_LOG_ERROR_FMT(__func__,
          "Unable to acquire VF-BW map for node %s guid "FMT_U64", port %d, status = %d",
          sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, curPort->index, status);
        return status;
    }


    // Bitfields of active VLs (based on VF) for this port
    bitset_t isVlActive;
    bitset_init(&sm_pool, &isVlActive, STL_MAX_VLS);
    bitset_set(&isVlActive, 15);

	VirtualFabrics_t *vfs = sm_topop->vfs_ptr;
    int vl;
    uint8_t numVls = (curPort->portData->vl1 <= 15 ? curPort->portData->vl1 : curPort->portData->vl1+1);
    for (vl = 0; vl < MAX(numVls, 16); vl++) {
        if (numVls < 16 && vl==numVls)
            vl = 15;   // skip to VL15

        if (vl == 15) continue;

        uint8_t vfIdx;

        if (vlvfmap.vf[vl][0] != -1)
            bitset_set(&isVlActive, vl);

        // find the max HoQ lifetime for each VL on the port
        for (vfIdx  = 0; vfIdx < vfs->number_of_vfs; ++vfIdx) {
            if (vlvfmap.vf[vl][vfIdx] == -1) break; // Done list.

            uint8_t vf = vlvfmap.vf[vl][vfIdx];

            if (vfs->v_fabric[vf].hoqlife_vf > maxHoqVals[vl])
                maxHoqVals[vl] = vfs->v_fabric[vf].hoqlife_vf;

            // get a bitmask of preemptible VLs.
            preemptibleVLs |= curPort->portData->curArb.vlarbMatrix[vl];
        }
    }

    // Bitfield indicating hi priority VLs 
    bitset_t isVlHigh;
    bitset_init(&sm_pool, &isVlHigh, STL_MAX_VLS);

    uint8_t idx;
    for (idx=0; idx<STL_MAX_LOW_CAP; idx++) {
        if (curPort->portData->curArb.vlarb.vlarbHigh[idx].Weight > 0) {
            bitset_set(&isVlHigh, curPort->portData->curArb.vlarb.vlarbHigh[idx].s.VL);
        }
    }

    // Setup the values per-VL values for this port.
    for (vl = 0; vl < MAX(numVls, 16); vl++) {
        if (numVls < 16 && vl==numVls)
            vl = 15;   // skip to VL15

        // Since perVL-VLStallCnt is limited in HW implementation, 
        // just copy the SM-wide VLStallCnt into all active VLs
        // (including VL 15).
        // Not applicable to Port 0.
        if (curPort->index!=0) {
            uint8_t vlStallCnt = sm_config.vlstall;
            if (curPort->portData->portInfo.XmitQ[vl].VLStallCount != vlStallCnt) {
                curPort->portData->portInfo.XmitQ[vl].VLStallCount = vlStallCnt;
                *updHoq = 1;
            }
        }

        if (bitset_test(&isVlActive, vl)) {
            boolean incrTimeouts = 0;

            // Increment to next timer value under specific conditions.
            if (vl != 15 && maxHoqVals[vl] < IB_LIFETIME_MAX) {
                incrTimeouts = (!bitset_test(&isVlHigh, vl) && (bitset_nset(&isVlHigh) > 0 || vlbwmap.bw[vl] < 25)) ||
                    (curPort->portData->portInfo.FlitControl.Preemption.PreemptionLimit == STL_PORT_PREEMPTION_LIMIT_NONE &&
                    ((1<<vl) & preemptibleVLs)!=0);
            }

            if (incrTimeouts && (sm_config.timerScalingEnable!=0)) {
                uint32_t oldHoq;

                oldHoq = maxHoqVals[vl];
                if (maxHoqVals[vl] < IB_LIFETIME_MAX)
                    maxHoqVals[vl]++;

                IB_LOG_DEBUG1_FMT(__func__,
                    "Adjusting HOQ Lifetime values.  "
                    "Node %s guid "FMT_U64": port %d; VL : %d;"
                    " current value HOQLife : %d; new values HOQLife : %d",
                    sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, curPort->index,
                    vl, oldHoq, maxHoqVals[vl]);
            }

            // Update HLL, only if not port 0
            if (curPort->index!=0) {
                uint8 vlHll = maxHoqVals[vl];
                if (curPort->portData->portInfo.XmitQ[vl].HOQLife != vlHll) {
                    curPort->portData->portInfo.XmitQ[vl].HOQLife = vlHll;
                    *updHoq = 1;
                }
            }
        }
    }

    bitset_free(&isVlHigh);
    bitset_free(&isVlActive);

    return status;
}

static int LdrCacheKey_cmp(const uint64 ia, const uint64 ib)
{
	LdrCacheKey_t *a = (LdrCacheKey_t*) ((long int)ia);
	LdrCacheKey_t *b = (LdrCacheKey_t*) ((long int)ib);

	if (a->guid != b->guid)
		return (a->guid < b->guid? -1: 1);
	if (a->index != b->index)
		return (a->index < b->index? -1: 1);
	return 0;
}


// to be called from within a sweep.  requests another sweep be performed
// after this one, but allows the current sweep to succeed
void
sm_request_resweep(int forceLfts, int forceMfts, SweepReason_t reason)
{
	uint64_t now, next;

	if (! topology_resweep || (forceMfts && ! topology_forceMfts)
		|| (forceLfts && ! forceRebalanceNextSweep) ){
		IB_LOG_INFINI_INFO_FMT(__func__, "SM Resweep (with%s LFT, with%s MFT) scheduled.",
				(forceLfts||forceRebalanceNextSweep)?"":"out",
				(forceMfts||topology_forceMfts)?"":"out");
	}
	if (forceLfts) forceRebalanceNextSweep = 1;
	if (forceMfts) topology_forceMfts = 1;
	topology_resweep = 1;

	setResweepReason(reason);

	// if we're already queued up for the next sweep, this will wake us.
	// if we're already in a sweep, this will get overwritten to the correct
	// value because of hte topology_resweep flag (we may end up with one
	// more sweep than expected)
	(void)vs_time_get(&now);
	next = now + VTIMER_1S * 5;
	if (next < topology_wakeup_time)
		topology_wakeup_time = next;
}

void
sm_trigger_sweep(SweepReason_t reason)
{
	setResweepReason(reason);

	AtomicWrite(&topology_triggered, 1);
	(void)vs_time_get(&topology_sema_setTime);
	(void)cs_vsema(&topology_sema);
}

/*
 * interval is the smallest interval in seconds with which retries will start
 * interval_max_limit is the upper limit for the retry interval in seconds
 * interval_reset = 1 means after upper limit of intervals is hit, retries will again
 * 					start with topo_retry_interval
 * interval_reset = 0 means after upper limit of intervals is hit, retries will continue
 *					to use a retry interval of topo_retry_interval_max_limit	
 * retry_count is the number of retries attempted till now
 */
static 
void topo_enable_retry_backoff(uint32_t interval, uint32_t interval_max_limit, int interval_reset, int retry_count)
{
	/* Use an incremental backoff of multiples of topo_retry_interval with an upper bound of
	 * topo_retry_interval_limit. 
	 */
	topo_retry_backoff_interval = interval * retry_count;

	if (topo_retry_backoff_interval > interval_max_limit) {
		if (interval_reset) {
			/* Reset interval to start again with topo_retry_interval */
			topo_retry_backoff_interval = topo_retry_backoff_interval % interval_max_limit;
			if (topo_retry_backoff_interval == 0)
				topo_retry_backoff_interval = interval_max_limit;
		}
		else {
			/* Continue to use topo_retry_interval_max_limit as the retry interval */
			topo_retry_backoff_interval = interval_max_limit;
		}

	}

	topo_retry_backoff = 1;
}

void
topology_main(uint32_t argc, uint8_t ** argv)
{
	int		i;
    int     topologSemCount=0;
	uint64_t	now, temp64;
	Status_t	status = VSTATUS_OK;
    int     newTopologyValid = 1;   // PR# 101511 - make sure we don't try to use an incomplete new topology
    int     sweepStartPacketCount=0;
	int		oldSmCount = 0, newSmActiveCount = 0, oldSwitchCount = 0, oldHfiCount = 0, oldEndPortCount = 0, oldTotalPorts = 0;
	int		force;
#ifdef __VXWORKS__
	uint8_t shutdown=0;
#endif

	IB_ENTER(__func__, 0, 0, 0, 0);

	topology_main_exit = 0;
	AtomicWrite(&topology_triggered, 0);
	cca_discovery_count = 0;

//
//	Get my thread name.
//
	(void)vs_thread_name(&sm_threads[SM_THREAD_TOPOLOGY].name);

//
//	Load pre-defined topology.
//
	(void)vs_lock(&new_topology_lock);
	(void)vs_wrlock(&old_topology_lock);

	status = topology_load_predef();

	if (status == VSTATUS_OK) {
		status = sm_routing_init();
		if (status) {
			IB_LOG_ERRORRC("Failed to initialize routing algorithm; rc:", status);
		}
	}

	(void)vs_rwunlock(&old_topology_lock);
	(void)vs_unlock(&new_topology_lock);

//
//	bail out if init failed
// 
	if (status) return;

//
//	Init data structures for tracking entities removed from fabric.
//
	sm_removedEntities_init();

	if (!bitset_init(&sm_pool, &new_switchesInUse, SM_NODE_NUM) ||
		!bitset_init(&sm_pool, &old_switchesInUse, SM_NODE_NUM) ||
		!bitset_init(&sm_pool, &new_endnodesInUse, SM_NODE_NUM)) {
		return;
	}

//
// Initialize our counters
//
	sm_init_counters();

//
//	We need to set our LID to the one specified by the parameters.
//
	if ((sm_lid != RESERVED_LID) || (sm_lid != PERMISSIVE_LID) || (sm_lid != STL_LID_PERMISSIVE)) {
		lidmap[sm_lid].guid = sm_portguid;
		lidmap[sm_lid].pass = topology_passcount;
	}

    //
    // wait for topology_rcv thread
    //
    while ((status = cs_psema(&topology_rcv_sema)) != VSTATUS_OK) {
        IB_LOG_ERRORRC("timeout returned while waiting for topology_rcv_sema rc:", status);
    }

//
//	Wait for the async thread to kick us off.  After it does, we run through
//	the topology setup code.  We then get the timer to determine when to do
//	this again.
//
    /*
     * move SM to discovery state now
     */
    (void)sm_transition(SM_STATE_DISCOVERING);

	while (1) {
		if(topology_main_exit == 1){
#ifdef __VXWORKS__
			ESM_LOG_ESMINFO("Topology Task exiting OK.", 0);
#endif
			break;
		}

		nodeAppearanceSeverity = CSM_SEV_NOTICE;
		sweepNodeChangeMsgCount = 0;
		sweepNodeAppearanceInfoMsgCount = 0;
		sweepNodeDisappearanceInfoMsgCount = 0;

		oldSmCount = sm_dbsync_getSmCount();
		if (sm_config.config_consistency_check_level != CHECK_ACTION_CCC_LEVEL)
			oldSmActiveCount = oldSmCount;

        smSendOutMFTs = 0;
        newTopologyValid = 1;  
        topology_changed = 0;
		topology_switch_port_changes = 0;
		topology_cost_path_changes = 0;
		routing_recalculated = 0;
        status = VSTATUS_OK;
        topologSemCount = -1;
        topology_resweep = 0;
        while (status == VSTATUS_OK && topologSemCount != 0) {
            if ((status = cs_psema(&topology_sema)) != VSTATUS_OK) {
                IB_FATAL_ERROR("TT: aborting - failed to take topology_sema");
            } else {
                if ((status = cs_sema_getcount(&topology_sema, &topologSemCount)) != VSTATUS_OK) {
                    IB_FATAL_ERROR("TT: aborting - failed to get topology_sema count");
                }
            }
			AtomicWrite(&topology_triggered, 0);
        }
		if(topology_main_exit == 1){
#ifdef __VXWORKS__
			ESM_LOG_ESMINFO("Topology Task exiting OK.", 0);
#endif
			break;
		}
		(void)vs_time_get(&topology_sema_runTime);
        sweepStartPacketCount = sm_smInfo.ActCount;

		if (sm_debug != 0) {
			(void)printf(".");
			(void)fflush(stdout);
		}

		topo_retry_backoff = 0;

#ifndef __VXWORKS__
		// sm_peer_quarantined implies sm_state == SM_STATE_NOTACTIVE
		if (sm_peer_quarantined && sm_state == SM_STATE_NOTACTIVE) {
			STL_PORT_INFO portInfo;
			uint8_t path[64];

			memset((void*)path, 0, 64);
			status = SM_Get_PortInfo(fd_sminfo, 1<<24, path, &portInfo);
			if (status != VSTATUS_OK) {
				IB_LOG_ERROR_FMT(__func__,
				  "Failed to get local port info; cannot transition back to DISCOVERING");
			} else if (sm_peer_quarantined &&
			  !portInfo.PortStates.s.IsSMConfigurationStarted &&
			  portInfo.PortStates.s.PortState > IB_PORT_DOWN) {
				sm_peer_quarantined = 0;
				sm_transition(SM_STATE_DISCOVERING);
			}
		}
#endif

		if ((sm_state == SM_STATE_DISCOVERING) || (sm_state == SM_STATE_MASTER)) {

			if (!sweepsPaused) {
				char tempStr[256];
				int prev_topo_errors = topo_errors;
				int prev_topo_abandon_count = topo_abandon_count;

				snprintf(tempStr, 256, "TT: DISCOVERY CYCLE START - REASON: %s\n", sweep_reasons[sm_resweep_reason]);
#if !defined(__VXWORKS__)
				IB_LOG_INFINI_INFO0(tempStr);
#else
				if (smDebugPerf) {
					IB_LOG_INFINI_INFO0(tempStr);
				}
#endif
				sm_resweep_reason = SM_SWEEP_REASON_UNDETERMINED;
				isSweeping = 1;

				(void)vs_lock(&new_topology_lock);

				for (i = 0; topology_functions[i] != NULL; i++) {
					status = (topology_functions[i])();
					if (status != VSTATUS_OK) {
						// catch-all for any error condition that did not adjust the global topology
						// error counter or topology abandonment counter  
						if (topo_errors == prev_topo_errors)
						   topo_errors++;
						if (topo_abandon_count == prev_topo_abandon_count)
						   topo_abandon_count++;
						if (topo_errors > sm_config.topo_errors_threshold &&
						   topo_abandon_count > sm_config.topo_abandon_threshold) {
						   // abandonment threshold exceeded, so ignore error conditions and
						   // continue the sweep of the entire fabric
						   continue;
						}

						/*
						 * PR# 101511
						 * new topology is most likely incomplete.  This can be caused when a
						 * switch, discovered by topology_discovery, is removed from fabric before 
						 * topology_assigments is called.  The window of opportunity for hitting 
						 * this grows with the size of the fabric.
						 */
						newTopologyValid = 0;   // new topology is not reliable, re-sweep
						if (status != VSTATUS_NOT_MASTER) {
							IB_LOG_WARNRC("TT: too many errors during sweep, will re-sweep in a few seconds rc:", status);
						}
                        
						break;
					}
					if(topology_main_exit == 1){
#ifdef __VXWORKS__
						ESM_LOG_ESMINFO("Topology Task exiting OK.", 0);
#endif
						newTopologyValid = 0;   
						break;
					}
				}

				/* clear topology error counters and sweep abandonment counters */
				if (topo_abandon_count > sm_config.topo_abandon_threshold || topo_errors == 0) topo_abandon_count = 0;
				topo_errors = 0;

				/* bump dispatcher passcount to ensure inflight requests handled appropriately */
				sm_dispatch_bump_passcount(&sm_asyncDispatch);

				/* copy new topology view to old only if complete */
				if (newTopologyValid) {

					oldSwitchCount = old_topology.num_sws;
					oldHfiCount = (old_topology.num_nodes - old_topology.num_sws);
					oldEndPortCount = old_topology.num_endports;
					oldTotalPorts = old_topology.num_ports;

					sm_compactSwitchSpace(&sm_newTopology, &new_switchesInUse);

					sm_clearSwitchPortChange(&sm_newTopology);

					(void)topology_copy();

					if (previousVfPtr) {
						//release old vfs_ptr
						releaseVirtualFabricsConfig(previousVfPtr);
						updatedVirtualFabrics = NULL;
						previousVfPtr = NULL;
					}


					// IB_LOG_INFINI_INFO("SM Pool Size= ", vs_pool_size(&sm_pool));

					/* Track how many times we do full discovery */
					++topology_passcount;
					activateInProgress = 0;
					isSweeping = 0;
					/* send out all the mft changes from old_topology to not hold up the SA */
					if (smSendOutMFTs || topology_forceMfts || topology_changed || topology_passcount <= 1) {
						force = topology_forceMfts;
						topology_forceMfts = 0;
						if (topology_passcount > 1)
							status = sm_set_all_mft(force, &save_topology);
						else
							status = sm_set_all_mft(force, 0);

						if (status != VSTATUS_OK) {
							if (status != VSTATUS_NOT_MASTER) {  /* reprogram the switches */
								sm_request_resweep(0, 1, SM_SWEEP_REASON_MC_ROUTING_FAIL);
							}
						}
					}
					/* release the copied old topology */
					(void)topology_release_saved_topology();

					/* dump the topology if changed and sm_debug on */
					(void)topology_dump();
				} else {
					/* release allocated storage in bad new topology */
					(void)topology_clearNew();

					if (sm_state == SM_STATE_STANDBY || sm_state == SM_STATE_NOTACTIVE) {
						/* we are no longer the master sm, clear passcount and SA tables */
						topology_passcount = 0;
						clearSaTables();
					}
				}
				(void)vs_unlock(&new_topology_lock);

				/* print DG and VF memberships per node */
				printDgVfMemberships();

			} //skip sweep if sweepsPaused
			else {
				newTopologyValid = 0;
			}

            (void)vs_time_get(&now);
            if (newTopologyValid) {
				int newSmCount = sm_dbsync_getSmCount();

				int swDelta = ((int)sm_topop->num_sws - oldSwitchCount);
				int hcaDelta = (((int)sm_topop->num_nodes - (int)sm_topop->num_sws) - oldHfiCount);
				int epDelta = ((int)sm_topop->num_endports - oldEndPortCount);
				int tpDelta = ((int)sm_topop->num_ports - oldTotalPorts);
				int smDelta	= (newSmCount - oldSmCount);
				int haveDelta = (swDelta | hcaDelta | epDelta | tpDelta | smDelta);

				temp64 = now - topology_sema_runTime;
#ifdef __VXWORKS__
				if (haveDelta || smDebugPerf || ((sm_config.timer/1000000) >= 300) || ((temp64/1000000) > 60)) {
#endif
					IB_LOG_INFINI_INFO_FMT(__func__, 
                       "DISCOVERY CYCLE END. %d SWs, %d HFIs, %d end ports, %d total ports, %d SM(s), %d packets, %d retries, %d.%.3d sec sweep",
                       sm_topop->num_sws, (sm_topop->num_nodes-sm_topop->num_sws), sm_topop->num_endports, sm_topop->num_ports, newSmCount,
                       ((unsigned int)sm_smInfo.ActCount - sweepStartPacketCount),
				       (int)AtomicRead(&smCounters[smCounterPacketRetransmits].sinceLastSweep), 
                       (unsigned int)(temp64/1000000), (unsigned int)((temp64 - temp64/1000000*1000000))/1000);
#ifdef __VXWORKS__
                    if (!shutdown && sm_topop->num_nodes >= MAX_SUBNET_SIZE) {
                        shutdown=1;
                        IB_LOG_ERROR_FMT(__func__, "TT: aborting - fabric size exceeds the %d maximum nodes supported by the ESM", MAX_SUBNET_SIZE);
                        smCsmLogMessage(CSM_SEV_NOTICE, CSM_COND_SM_SHUTDOWN,
                            getMyCsmNodeId(), NULL, 
                            "Terminating SM after %d sweeps.", topology_passcount);
                        sm_control_shutdown(NULL);
                        exit(0);
                    }
				}
#endif
				SET_PEAK_COUNTER(smMaxSweepTime, (uint32)(temp64/1000));

#ifndef __VXWORKS__
				if (smDumpCounters) {
					char *buff = sm_print_counters_to_buf();
					FILE *f = fopen(smDumpCounters,"a");
					if (f && buff) fputs(buff, f);
					if (f) fclose(f);
					if (buff) vs_pool_free(&sm_pool, (void*)buff);
				}
#endif

				if (smTerminateAfter && topology_passcount >= smTerminateAfter) {
					smCsmLogMessage(CSM_SEV_NOTICE, CSM_COND_SM_SHUTDOWN,
						getMyCsmNodeId(), NULL, 
						"Terminating SM after %d sweeps.", topology_passcount);
					IB_FATAL_ERROR_NODUMP("Terminating SM.");
				} else if ( (topology_passcount > 1) && ( haveDelta )) {

					if (sweepNodeAppearanceInfoMsgCount != 0) {
						smCsmLogMessage(CSM_SEV_NOTICE, CSM_COND_APPEARANCE, getMyCsmNodeId(), NULL,
						                "An additional %d nodes appeared in the fabric and were logged "
						                "as INFO messages", sweepNodeAppearanceInfoMsgCount);
					}

					if (sweepNodeDisappearanceInfoMsgCount != 0) {
						smCsmLogMessage(CSM_SEV_NOTICE, CSM_COND_DISAPPEARANCE, getMyCsmNodeId(), NULL,
						"An additional %d nodes disappeared from the fabric and were logged "
						"as INFO messages", sweepNodeDisappearanceInfoMsgCount);
					}

					smCsmLogMessage(CSM_SEV_NOTICE, CSM_COND_FABRIC_SUMMARY, getMyCsmNodeId(), NULL,
					                "Change Summary: %d SWs %s, %d HFIs %s, %d end ports %s, "
					                "%d total ports %s, %d SMs %s",
					                abs(swDelta), swDelta >= 0 ? "appeared" : "disappeared",
					                abs(hcaDelta), hcaDelta >= 0 ? "appeared" : "disappeared",
					                abs(epDelta), epDelta >= 0 ? "appeared" : "disappeared",
					                abs(tpDelta), tpDelta >= 0 ? "appeared" : "disappeared",
					                abs(smDelta), smDelta >= 0 ? "appeared" : "disappeared");

				}

				if (haveDelta)
					smCsmLogMessage(CSM_SEV_INFO, CSM_COND_FABRIC_SUMMARY, getMyCsmNodeId(), NULL,
				                "%d SWs, %d HFIs, %d end ports, %d total ports, %d SM(s)",
				                sm_topop->num_sws, (sm_topop->num_nodes-sm_topop->num_sws), sm_topop->num_endports, sm_topop->num_ports, newSmCount);

				if (sm_config.config_consistency_check_level == CHECK_ACTION_CCC_LEVEL)
					newSmActiveCount = sm_dbsync_getActiveSmCount();
				else
					newSmActiveCount = newSmCount;

				if ((oldSmActiveCount > 1) && (newSmActiveCount == 1))
					smCsmLogMessage(CSM_SEV_WARNING, CSM_COND_REDUNDANCY_LOST, getMyCsmNodeId(), NULL, "Only one SM remains in fabric");
				else if ((topology_passcount == 1) && (newSmActiveCount == 1))
					IB_LOG_INFINI_INFO_FMT(__func__, "SM redundancy not available");
				else if ((oldSmActiveCount == 1) && (newSmActiveCount > 1))
					smCsmLogMessage(CSM_SEV_NOTICE, CSM_COND_REDUNDANCY_RESTORED, getMyCsmNodeId(), NULL, "%d SM's now online in fabric", newSmActiveCount);

				if (sm_config.config_consistency_check_level == CHECK_ACTION_CCC_LEVEL)
					oldSmActiveCount = newSmActiveCount;

                // if we've found more HFI's than we're configured for, issue a warning
                if (sm_topop->num_endports > sm_config.subnet_size) {
                	IB_LOG_WARN_FMT(__func__, 
                    	"the number of HFI EndPorts %d on the fabric exceeds the configured subnet size %d",
                        sm_topop->num_endports, sm_config.subnet_size);
                }


#if 0
				if (smDebugPerf || saDebugPerf)
					sm_print_counters_to_stream(stdout);
#endif

				sm_process_sweep_counters();

				/* if topology change allow a rediscovery of CCA */
				if (haveDelta)
					cca_discovery_count = 0;
            }
		} else {
            /* clear the SA tables if necessary */
            clearSaTables();
			topology_passcount=0;
		}

		(void)vs_time_get(&now);
        if (newTopologyValid && !topology_resweep) {
            if (sm_config.timer != 0)
            {
                topology_wakeup_time = now + sm_config.timer;
            }
            else {
                topology_wakeup_time = 0;
            }
        } else {
			const uint32_t factor = topo_retry_backoff ? topo_retry_backoff_interval : 5;
			topology_wakeup_time = now + VTIMER_1S * factor; // re-sweep after delay.
			
			setResweepReason(SM_SWEEP_REASON_FAILED_SWEEP);
		}

		if (newTopologyValid) {
			// configure congestion control outside of the sweep
			// this will abort if another sweep is triggered
			(void)topology_congestion();
		}



	}	// End of while(1)

    /* clear the isSM bit from our port */
    sm_clearIsSM();

#ifdef __VXWORKS__
// We need to clean the globals here.
	topology_uninitialize();

#endif

	IB_EXIT(__func__, VSTATUS_OK);
	//IB_LOG_INFINI_INFO0("topology_main thread: Exiting OK");
	return;
}

#ifdef __VXWORKS__
void
topology_uninitialize(void)
{
	topology_wakeup_time = 0;

	bitset_free(&old_switchesInUse);
	bitset_free(&new_switchesInUse);
	bitset_free(&new_endnodesInUse);

	memset(&old_topology, 0, sizeof(old_topology));
	memset(&sm_newTopology, 0, sizeof(sm_newTopology));
	memset(&save_topology, 0, sizeof(save_topology));
	sm_topop = &sm_newTopology;

	topology_once = -1;
	topology_changed = 0;
	topology_switch_port_changes = 0;
	topology_cost_path_changes = 0;
	topology_passcount = 0ull;
	
	sm_dispatch_destroy(&sm_asyncDispatch);
	sm_removedEntities_destroy();
}
#endif

Status_t
topology_load_predef(void)
{
	IB_ENTER(__func__, 0, 0, 0, 0);

	// If the pre-defined topology feature has been enabled, load and parse the file. (HSM only!)
	if(sm_config.preDefTopo.enabled) {
		FSTATUS parseStatus = FSUCCESS;
		InitFabricData(&preDefTopology, FF_LIDARRAY);

#ifndef __VXWORKS__
		parseStatus = Xml2ParseTopology(sm_config.preDefTopo.topologyFilename, 1, &preDefTopology);
#else
		XML_Memory_Handling_Suite memsuite;
		memsuite.malloc_fcn = &getParserMemory;
		memsuite.realloc_fcn = &reallocParserMemory;
		memsuite.free_fcn = &freeParserMemory;

		parseStatus = Xml2ParseTopology(sm_config.preDefTopo.topologyFilename, 1, &preDefTopology, &memsuite);
#endif

		if(parseStatus != FSUCCESS) {
			IB_LOG_ERROR_FMT(__func__, "Pre Defined Topology: Failed parsing pre-defined topology input file: %s", sm_config.preDefTopo.topologyFilename);
			IB_FATAL_ERROR_NODUMP("Pre Defined Topology: terminating FM due to previous errors.");
			return VSTATUS_BAD;
		} else {
			int topologyFileValid = 1;

			if(QListHead(&preDefTopology.ExpectedLinks) == NULL) {
				IB_LOG_ERROR_FMT(__func__, "Pre Defined Topology: Topology file %s does not have link definitions. Cannot verify fabric topology.", sm_config.preDefTopo.topologyFilename);
				topologyFileValid = 0;
			}

			if(QListHead(&preDefTopology.ExpectedFIs) == NULL) {
				IB_LOG_ERROR_FMT(__func__, "Pre Defined Topology: Topology file %s does not haveFI node definitions. Cannot verify fabric topology.", sm_config.preDefTopo.topologyFilename);
				topologyFileValid = 0;
			}
			
			if(QListHead(&preDefTopology.ExpectedSWs) == NULL) {
				IB_LOG_ERROR_FMT(__func__, "Pre Defined Topology: Topology file %s does not have switch node definitions. Cannot verify fabric topology.", sm_config.preDefTopo.topologyFilename);
				topologyFileValid = 0;
			}

			if(topologyFileValid) {
				char buf[FILENAME_SIZE + 256];
				snprintf(buf, sizeof(buf), "SM: Pre-Defined Topology: (Enabled) Using topology file: %s", sm_config.preDefTopo.topologyFilename);
				vs_log_output_message(buf, FALSE);

				snprintf(buf, sizeof(buf),
						"SM: Pre-Defined Topology: Field Enforcement: NodeDesc: %s, NodeGUID: %s, PortGUID: %s, UndefinedLink: %s", 
						SmPreDefFieldEnfToText(sm_config.preDefTopo.fieldEnforcement.nodeDesc),
						SmPreDefFieldEnfToText(sm_config.preDefTopo.fieldEnforcement.nodeGuid),
						SmPreDefFieldEnfToText(sm_config.preDefTopo.fieldEnforcement.portGuid),
						SmPreDefFieldEnfToText(sm_config.preDefTopo.fieldEnforcement.undefinedLink));
				vs_log_output_message(buf, FALSE);
			} else {
				IB_FATAL_ERROR_NODUMP("Pre Defined Topology: terminating FM due to previous errors.");
				return VSTATUS_BAD;
			}
		}
	}

	IB_EXIT(__func__, VSTATUS_OK);
	return VSTATUS_OK;
}

static int
verify_admin_membership(Port_t *portp)
{
	int vfi, dg;
	VFConfig_t *vfp;
	int okay = 0;

	(void)vs_rdlock(&old_topology_lock);
	if (old_topology.vfs_ptr) {

		// Find the Admin VF.
		for (vfi=0; 
			vfi < old_topology.vfs_ptr->number_of_vfs &&
			((old_topology.vfs_ptr->v_fabric[vfi].pkey & ~0x8000) != STL_DEFAULT_PKEY);
			vfi++) {
			// Empty body. Work is done in the conditional.
		}

		if (vfi >= old_topology.vfs_ptr->number_of_vfs) {
			(void)vs_rwunlock(&old_topology_lock);
			IB_LOG_ERROR_FMT(__func__,"Could not identify the Admin VF.");
			goto done;
		}

		vfp = vf_config.vf[vfi];
		IB_LOG_INFO_FMT(__func__,"\"%s\" is the admin VF.", vfp->name);

		for (dg=0; dg < vfp->number_of_full_members; dg++) {
			int dgIdx = smGetDgIdx(vfp->full_member[dg].member);
			if (dgIdx != -1) {
				if(isDgMember(dgIdx,portp->portData)) {
					okay=1;
					break;
				}
			}
		}

		if (!okay) {
			IB_LOG_ERROR_FMT(__func__,
				"SM's port is not a full member of the \"%s\" VF.",
				vfp->name);
		} else {
			IB_LOG_INFO_FMT(__func__,
				"SM's port is a full member of the \"%s\" fabric via the"
				"\"%s\" group.",
				vfp->name,
				vfp->full_member[dg].member);
		}
	} else {
		IB_LOG_ERROR_FMT(__func__,"Unabled to validate SM's membership in Admin VF.");
	}

done:
	(void)vs_rwunlock(&old_topology_lock);
	return okay;
}

Status_t
topology_initialize(void)
{
	uint8_t path[64] = { 0 };

	// Used by the IB_*_NOREPEAT macros. Declared static in case the function
	// gets called multiple times in a single sweep.
	static uint8_t lastMsg = 0;

#if defined(CAL_IBACCESS)
	uint32_t mask;
#endif
	Status_t status;
	STL_PORT_INFO portInfo;
	STL_NODE_INFO nodeInfo;
	Port_t *portp;
	Node_t *nodep;

	IB_ENTER(__func__, 0, 0, 0, 0);


	//
	// Loop until we've successfully talked to the local port. In general, if
	// we fail to get a MAD, or if a status check fails, wait a few seconds
	// and re-start the loop.
	//
	do {
		// Reset the path back to our local port.
		memset(path,0, sizeof(path));

		// Stop if we've been externally terminated.
		if(topology_main_exit) {
#ifdef __VXWORKS__
			ESM_LOG_ESMINFO("Topology Task exiting OK.", 0);
#endif
			return(VSTATUS_BAD);
		}

		// get PortInfo record of the local port
		status = SM_Get_PortInfo(fd_topology, (1<<24) | STL_SM_CONF_START_ATTR_MOD,
			path, &portInfo);

		if (status != VSTATUS_OK) {
			IB_WARN_NOREPEAT(lastMsg,1,"can't get PortInfo, sleeping rc: %u", status);
			vs_thread_sleep(5 * VTIMER_1S);
			continue;
		}

		if (portInfo.PortStates.s.PortState < IB_PORT_INIT) {
			// Wait until the port is at least in INIT because the LNI process
			// may change the local pkey table and other port information.
			//
			// Note that we use the portdown_warned flag to prevent the log
			// from being flooded with warnings.
			IB_WARN_NOREPEAT(lastMsg,2,"Waiting for port (portnum=%d)"
				" to reach state INIT (portstate=%s)...",
				sm_config.port,
				IbPortStateToText(portInfo.PortStates.s.PortState));
			vs_thread_sleep(5 * VTIMER_1S);
			status = VSTATUS_BAD;
			continue;
		}

		// Get NodeInfo record of the local port.
		status = SM_Get_NodeInfo(fd_topology, 0, path, &nodeInfo);
		if (status != VSTATUS_OK) {
			IB_WARN_NOREPEAT(lastMsg,3, "can't get NodeInfo, sleeping rc: %u", status);
			vs_thread_sleep(5 * VTIMER_1S);
			continue;
		}

		//
		// Verify that the port has MgmtAllowed authority.
		//
		if (nodeInfo.NodeType == NI_TYPE_SWITCH) {
			// get SwitchInfo record of the local switch
			STL_SWITCH_INFO switchInfo;
			status = SM_Get_SwitchInfo(fd_topology, 0, path, &switchInfo);
			if (status != VSTATUS_OK) {
				IB_WARN_NOREPEAT(lastMsg,4,"can't get local SwitchInfo, sleeping rc: %u", status);
				vs_thread_sleep(5 * VTIMER_1S);
				continue;
			}

			if (!switchInfo.u2.s.EnhancedPort0) {
				status = VSTATUS_MISMATCH;
				IB_WARN_NOREPEAT(lastMsg,5, "MgmtAllowed not enabled for the local "
					"Embedded SM, sleeping rc: %u", status);
				vs_thread_sleep(5 * VTIMER_1S);
				continue;
			}

		} else if (nodeInfo.NodeType == NI_TYPE_CA) {
			if (portInfo.PortNeighborMode.NeighborNodeType ==
				STL_NEIGH_NODE_TYPE_HFI) {
				// Back to back configuration.
				sm_hfi_direct_connect = TRUE;
				IB_LOG_INFO_FMT(__func__,"Host SM's port is connected to an "
					"hfi port.");
			} else if (portInfo.PortNeighborMode.NeighborNodeType ==
				STL_NEIGH_NODE_TYPE_SW) {
				// Normal fabric configuration.
				sm_hfi_direct_connect = FALSE;
				if (!portInfo.PortNeighborMode.MgmtAllowed) {
					IB_WARN_NOREPEAT(lastMsg,6,"Host SM's port is connected to a"
						" switch port but MgmtAllowed is not set. sleeping rc: %u",
						VSTATUS_BAD);
					vs_thread_sleep(5 * VTIMER_1S);
					status = VSTATUS_BAD;
					continue;
				} else {
					IB_LOG_INFO_FMT(__func__,"Host SM's port is connected to a "
						"switch port.");
				}
			} else {
				IB_ERROR_NOREPEAT(lastMsg,7,"SM's port is connected to an"
						" unknown type of node (NeighborNodeType = %d)",
						portInfo.PortNeighborMode.NeighborNodeType);
				vs_thread_sleep(5 * VTIMER_1S);
				status = VSTATUS_BAD;
				continue;
			}
		} else {
			status = VSTATUS_MISMATCH;
			IB_WARN_NOREPEAT(lastMsg,8, "SM is running on an unknown node type. sleeping rc: %u",
				status);
			vs_thread_sleep(5 * VTIMER_1S);
			continue;
		}
	} while (status != VSTATUS_OK);

	//
	// Clear and initialize the new topology structure.
	// Note that our caller must hold the new topology lock.
	//
	(void)memset((void *)&sm_newTopology, 0, sizeof(Topology_t));

	//
	// Update the new topology's virtual fabrics ptr and save off the old ptr
	// to be deleted later
	//
	if (updatedVirtualFabrics) {
		sm_newTopology.vfs_ptr = updatedVirtualFabrics;
		previousVfPtr = old_topology.vfs_ptr;
	}
	else {
		sm_newTopology.vfs_ptr = old_topology.vfs_ptr;
	}

	sm_newTopology.maxMcastMtu = STL_MTU_MAX;
	sm_newTopology.maxMcastRate = IB_STATIC_RATE_MAX;
	
	bitset_clear_all(&new_switchesInUse);
	bitset_clear_all(&new_endnodesInUse);

	// Initialize the quick maps that store the sorted GUID lists
	status = vs_pool_alloc(&sm_pool, sizeof(cl_qmap_t), (void *)&sm_newTopology.nodeIdMap);
	if (status != VSTATUS_OK) {
		IB_ERROR_NOREPEAT(lastMsg,9,"can't malloc node Id map: %u", status);
		return(status);
	}
	
	status = vs_pool_alloc(&sm_pool, sizeof(cl_qmap_t), (void *)&sm_newTopology.nodeMap);
	if (status != VSTATUS_OK) {
		IB_ERROR_NOREPEAT(lastMsg,10,"can't malloc node map: %u", status);
		return(status);
	}
	
	status = vs_pool_alloc(&sm_pool, sizeof(cl_qmap_t), (void *)&sm_newTopology.portMap);
	if (status != VSTATUS_OK) {
		IB_ERROR_NOREPEAT(lastMsg,11,"can't malloc port map: %u", status);
		return(status);
	}
	
	status = vs_pool_alloc(&sm_pool, sizeof(cl_qmap_t), (void *)&sm_newTopology.quarantinedNodeMap);
	if (status != VSTATUS_OK) {
		IB_ERROR_NOREPEAT(lastMsg,12,"can't malloc node map: %u", status);
		return(status);
	}

	cl_qmap_init(sm_newTopology.nodeIdMap, NULL);
	cl_qmap_init(sm_newTopology.nodeMap, NULL);
	cl_qmap_init(sm_newTopology.portMap, NULL);
	cl_qmap_init(sm_newTopology.quarantinedNodeMap, NULL);

	sm_newTopology.smaChanges = NULL;
	sm_newTopology.nodeArray = NULL;
	memset(&sm_newTopology.preDefLogCounts, 0, sizeof(PreDefTopoLogCounts));

	status = vs_wrlock(&old_topology_lock);
	if (status != VSTATUS_OK) {
		return status;
	}

	if (sm_topop->routingModule == NULL && old_topology.routingModule != NULL) {
		if ((status = vs_pool_alloc(&sm_pool, sizeof(RoutingModule_t), (void*)&sm_topop->routingModule)) != VSTATUS_OK) {
			goto unlock_bail;
		}
		memset(sm_topop->routingModule, 0, sizeof(RoutingModule_t));

		if (old_topology.routingModule->copy) {
 			status = old_topology.routingModule->copy(sm_topop->routingModule, old_topology.routingModule);
			if (status != VSTATUS_OK) {
				goto unlock_bail;
			}
		}
		else {
			memcpy(sm_topop->routingModule, old_topology.routingModule, sizeof(RoutingModule_t));
		}
	}

unlock_bail:

	vs_rwunlock(&old_topology_lock);
	if (status != VSTATUS_OK) {
		IB_ERROR_NOREPEAT(lastMsg,13,"Failed to initialize routing module. Sleeping rc: %u", status);
		return status;
	}

	(void)memset((void *)path, 0, sizeof(path));
	status = sm_setup_node(sm_topop, &preDefTopology, NULL, NULL,
		path);
	if (status != VSTATUS_OK) {
		IB_ERROR_NOREPEAT(lastMsg,14, "Can't set up my local node, sleeping rc: %u", status);
		return status;
	}

	nodep = sm_topop->node_head;
	if (!nodep) {
		IB_ERROR_NOREPEAT(lastMsg,15,"Local node has not been initialized, sleeping rc: %u", status);
		return VSTATUS_BAD;
	}
	if (!sm_valid_port((portp = sm_get_port(nodep,sm_config.port)))) {
		IB_ERROR_NOREPEAT(lastMsg,16,"Failed to get a valid SM port, sleeping rc: %u", VSTATUS_BAD);
		return(VSTATUS_BAD);
    }

	//
	// Validate our Admin VF membership. We could not do this until after
	// sm_setup_node() had been run on our local node.
	//
	if (!verify_admin_membership(portp)) {
		IB_ERROR_NOREPEAT(lastMsg,16,"SM port is not a full member of the admin VF, "
			"sleeping rc: %u", VSTATUS_BAD);
		vs_thread_sleep(5 * VTIMER_1S);
		return VSTATUS_BAD;
	}

	//
	// Always set the pkey here. In the B2B case it isn't set in LNI,
	// in the switch case, the pkey is set in LNI but might have been
	// cleared by another SM after LNI but before we started.
	//
	status = sm_set_local_port_pkey(&nodeInfo);
	if (status != VSTATUS_OK) {
		IB_ERROR_NOREPEAT(lastMsg,17, "can't set PKey table, sleeping rc: %u", status);
		return status;
	}

	//
	// Verify we can talk to our neighbor.
	// Note that there's no equivalent check for the ESM because switch port 0
	// doesn't have a neighbor.
	//
	if (nodeInfo.NodeType == NI_TYPE_CA) {
		STL_NODE_INFO neighborNodeInfo;
		// Build a 1 hop DR path.
		path[0]++;
		path[path[0]] = sm_config.port;

		// Verify we can talk to our neighbor.
		status = SM_Get_NodeInfo(fd_topology, 0, path, &neighborNodeInfo);
		if (status != VSTATUS_OK) {
			IB_ERROR_NOREPEAT(lastMsg,18, "Can't get neighbor NodeInfo, sleeping rc: %u", status);
			return status;
		}
	}

	// Announce that we are an SM. Note that this is safe to do on each sweep.
#if defined(IB_STACK_OPENIB)
	status = ib_enable_is_sm();
	if (status != VSTATUS_OK) {
		IB_ERROR_NOREPEAT(lastMsg,19, "Can't set isSM, sleeping rc: %u", status);
		return status;
	}
#elif defined(CAL_IBACCESS)
	mask = portInfo.CapabilityMask.AsReg32 | PI_CM_IS_SM;
	status = sm_set_CapabilityMask(fd_topology, sm_config.port, mask);
	if (status != VSTATUS_OK) {
		IB_ERROR_NOREPEAT(lastMsg,19, "can't set isSM, sleeping rc: %u", status);
		return status;
	}
#endif

	// Rather than get the port info again, just to get the updated capmask,
	// let's just set the bit in the capmask we've already got:
	portp->portData->capmask |= PI_CM_IS_SM;
	portp->portData->portInfo.CapabilityMask.AsReg32 |= PI_CM_IS_SM;

	topology_wakeup_time = 0ull;

	// We've completed successfully, clear the IB_*_ONCE tracking.
	lastMsg = 0;

	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

Status_t
topology_discovery(void)
{
	int		i;
	int		start_port;
	int		end_port;
	int		temp_lmc;
	uint8_t		path[72];
	Node_t		*nodep;
	Port_t		*portp, *neighborPortp = NULL;
	Status_t	status;
    STL_SMINFO_RECORD sminforec={{0}, 0};
    uint64_t    sTime, eTime;
	void *routingContext;


	IB_ENTER(__func__, 0, 0, 0, 0);

	if (sm_topop->routingModule == NULL) {
		status = vs_wrlock(&old_topology_lock);
		if (status != VSTATUS_OK) {
			IB_LOG_ERROR_FMT(__func__, "Failed to acquire old_topology_lock");
			return status;
		}

		status = sm_routing_makeCopy(&sm_topop->routingModule,
			old_topology.routingModule);

		if (status != VSTATUS_OK)
			IB_LOG_ERROR_FMT(__func__, "Failed to copy prior routing module data");

		Status_t unlockStatus = vs_rwunlock(&old_topology_lock);
		if (unlockStatus != VSTATUS_OK) {
			IB_LOG_ERROR_FMT(__func__, "Error on unlocking old_topology_lock");
			return unlockStatus;
		}

		if (status != VSTATUS_OK)
			return status;
	}    

	status = sm_topop->routingModule->funcs.pre_process_discovery(sm_topop, &routingContext);
	if (status != VSTATUS_OK) {
		IB_LOG_ERRORRC("Failed to process 'pre-discovery' routing hook; rc:", status);
		return status;
	}

	//
	//	Find out about the Node that I am on.
	//
	nodep = sm_topop->node_head;
	if (!nodep) {
		IB_LOG_ERROR_FMT(__func__,"Local node has not been initialized.");
		return VSTATUS_BAD;
	}
	if (!sm_valid_port((portp = sm_get_port(nodep,sm_config.port)))) {
		IB_LOG_ERROR0("failed to get SM port");
		sm_topop->routingModule->funcs.post_process_discovery(sm_topop, VSTATUS_BAD, routingContext);
		return VSTATUS_BAD;
    }

	vs_time_get(&sm_newTopology.sweepStartTime);

//
//	Start a directed route exploration of the fabric.
//
    if (smDebugPerf) {
        vs_time_get(&sTime);
        IB_LOG_INFINI_INFO0("START directed route exploration of fabric");
    }

	// Note that the node list can grow while we are inside this loop, so
	// we are guaranteed to loop over all new and existing nodes in the fabric.
	for_all_nodes(sm_topop, nodep) {

		status = sm_topop->routingModule->funcs.discover_node(sm_topop, nodep, routingContext);
		if (status != VSTATUS_OK) {
			IB_LOG_ERRORRC("Failed to process 'discover node' routing hook; rc:", status);
			sm_topop->routingModule->funcs.post_process_discovery(sm_topop, status, routingContext);
			return status;
		}

		if ((nodep != sm_topop->node_head) && (nodep->nodeInfo.NodeType != NI_TYPE_SWITCH)) {
			continue;
		}

		if (nodep->nodeInfo.NodeType == NI_TYPE_SWITCH) {
			start_port = 1;
			end_port = nodep->nodeInfo.NumPorts;
		} else {
			start_port = sm_config.port;
			end_port = sm_config.port;
		}

		(void)memcpy((void *)path, (void *)nodep->path, 64);
		path[0]++; // element zero is path length

		if (path[0] >= 62) {
			/* This means the end node is 63 hops away.
			 * The SM must be within 62 hops (allowing SM to look 1 node beyond end of fabric).
			 *
			 * For now, giving up on routing this fabric.
			 * TODO: check if we can ignore nodes beyond this node and still successfully program
			 * rest of the fabric.
			 */
			IB_LOG_ERROR_FMT(__func__,
				 "NodeGuid "FMT_U64" [%s] is 63 hops away from the SM. Maximum allowed hops is 62.",
				 nodep->nodeInfo.NodeGUID, sm_nodeDescString(nodep));
			status = VSTATUS_BAD;
			IB_LOG_ERRORRC("Invalid topology ! Please check your fabric connections.", status);
			sm_topop->routingModule->funcs.post_process_discovery(sm_topop, status, routingContext);
			return status;
		}

		// Add all the neighbors of this node to the list of nodes.
		for (i = start_port; i <= end_port; i++) {
			if ((portp = sm_get_port(nodep,i)) == NULL || portp->state <= IB_PORT_DOWN) {
				continue;
			}
			if (portp->nodeno == -1 || portp->portno == -1) {
				path[path[0]] = portp->index;		// Add this port to the end
//#ifndef BIU
				status = sm_setup_node(sm_topop, &preDefTopology, nodep, portp, path);
//#else
//				IB_LOG_WARN_FMT(__func__,"zp log : portp->index--%d portbiu--%d",portp->index,sm_config.smDorRouting.dimensionbiu.port);
//				if((strcmp(sm_topop->routingModule->name,"dorbiu")!=0)||(portp->index!=sm_config.smDorRouting.dimensionbiu.port)){
//					IB_LOG_WARN_FMT(__func__,"zp log : run sm_setup_node!");
//					status = sm_setup_node(sm_topop, &preDefTopology, nodep, portp, path);
//				}else{
//					status = VSTATUS_OK;
//				}
//#endif
			} else {
				// PR 110535 Don't loop back to node that has already been discovered.
				status = VSTATUS_OK;
			}

			if (topology_main_exit == 1) {
#ifdef __VXWORKS__
					ESM_LOG_ESMINFO("topology_discovery: SM has been stopped", 0);
#endif
					IB_EXIT(__func__, VSTATUS_OK);
					return(VSTATUS_OK);
			}

			if (status == VSTATUS_NOT_MASTER) {
				IB_LOG_INFO("now running as a STANDBY SM", sm_state);
				status = sm_topop->routingModule->funcs.post_process_discovery(
					sm_topop, VSTATUS_NOT_MASTER, routingContext);
				IB_EXIT(__func__, VSTATUS_NOT_MASTER);
				return(VSTATUS_NOT_MASTER);
			} else if (status != VSTATUS_OK) {
#ifndef __VXWORKS__
				Node_t *sm_nodep;
				Port_t *sm_portp;

				sm_nodep = sm_topop->node_head;
				sm_portp = sm_get_port(sm_nodep, sm_config.port);
#endif
				IB_LOG_WARN_FMT(__func__, 
					" unable to setup port[%d] of node %s, nodeGuid "FMT_U64
					", ignoring port!", portp->index, sm_nodeDescString(nodep),
					nodep->nodeInfo.NodeGUID);
				sm_mark_link_down(sm_topop, portp);
				topology_changed = 1;	/* indicates a fabric change has been detected */

#ifndef __VXWORKS__
				// Self-quarantining not implementing for ESM due to ESM
				// presumably having multiple ports to the fabric
				if (nodep == sm_nodep && portp == sm_portp &&
				  sm_portp->portData->neighborQuarantined) {
					sm_peer_quarantined = 1;
					IB_LOG_ERROR_FMT(__func__, "SM neighbor is quarantined."
					  " Cannot manage fabric. Transitioning to NOTACTIVE.");
					sm_transition(SM_STATE_NOTACTIVE);
					return status;
				}
#endif
			} else {
				// discovery went well

				// update fabric MTU & rate for use in multicast group creation/join requests...
				// and the maximum ISL MTU and rate we have seen so far
				// we only care about ISL's so that's all we're checking
				if (  sm_valid_port(portp)
					 && sm_valid_port(neighborPortp = sm_find_port_peer(sm_topop, nodep->nodeInfo.NodeGUID, portp->index))
					 && nodep->nodeInfo.NodeType == NI_TYPE_SWITCH
					 && neighborPortp->portData->nodePtr->nodeInfo.NodeType == NI_TYPE_SWITCH )
				{
					uint32_t rate = portp->portData->rate;

					if (sm_mc_config.disable_mcast_check == McGroupBehaviorStrict) {
												// PR123793: First time through, the maxVlMtu is not setup yet.
												// So don't check it against the maxMcastMtu until the port maxVlMtu is non-zero.
						if ((portp->portData->maxVlMtu !=0) && (portp->portData->maxVlMtu < sm_topop->maxMcastMtu)) {
							sm_topop->maxMcastMtu = portp->portData->maxVlMtu;
						}

						if (linkrate_lt(rate, sm_topop->maxMcastRate)) {
							sm_topop->maxMcastRate = rate;
						}
					}

					if (portp->portData->maxVlMtu > sm_topop->maxISLMtu) {
						sm_topop->maxISLMtu = portp->portData->maxVlMtu;
					}

					if (linkrate_gt(rate, sm_topop->maxISLRate)) {
						sm_topop->maxISLRate = rate;
					}
				}

				if (nodep->nodeInfo.NodeType == NI_TYPE_SWITCH) {
					status = sm_topop->routingModule->funcs.discover_node_port(sm_topop, nodep, portp, routingContext);
					if (status != VSTATUS_OK) {
						IB_LOG_ERRORRC("Failed to process 'discover node/port' routing hook; rc:", status);
						sm_topop->routingModule->funcs.post_process_discovery(sm_topop, status, routingContext);
						return status;
					}
				}
			}
		}
	}

	status = sm_topop->routingModule->funcs.post_process_discovery(sm_topop, VSTATUS_OK, routingContext);
	if (status != VSTATUS_OK) {
		IB_LOG_ERRORRC("Failed to process 'post-discovery' routing hook; rc:", status);
		return status;
	}

	// Do not become master of a fabric where our own port is down
	// This prevents an odd situation where a SM with an unstable link becomes
	// master when its link is down, and later when its link is up, it has an
	// elevated priority and becomes master for the whole fabric.  Which would
	// be contrary to the goals of sticky failover.
	// Note that port DOWN won't happen for SWE0
	if (sm_topop->node_head->nodeInfo.NodeType != NI_TYPE_SWITCH) {
		portp = sm_get_port(sm_topop->node_head, sm_config.port);
		if (  ! sm_valid_port(portp) || portp->state <= IB_PORT_DOWN) {
			if (sm_state == SM_STATE_MASTER)
				(void)sm_transition(SM_STATE_STANDBY);
			return(VSTATUS_NOT_MASTER);
		}
	}

	if (sm_config.sm_debug_vf) {
 		if (topology_passcount <= 2) smLogVFs();
	}
 
	// did we remove any ports?  if so, abort sweep
	if (sm_topop->numRemovedPorts) {
		IB_LOG_WARN_FMT(__func__,
		       "Removed %d port(s) from the fabric; will initiate re-sweep",
		       sm_topop->numRemovedPorts);
		IB_EXIT(__func__, VSTATUS_BAD);
		return VSTATUS_BAD;
	}
	
//
//	If the sm_lid is zero, we need to find one to use.
//
	if ((sm_lid == RESERVED_LID) || (sm_lid == PERMISSIVE_LID) || (sm_lid == STL_LID_PERMISSIVE)) {
        if (!sm_valid_port((portp = sm_get_port(sm_topop->node_head,sm_config.port)))) {
            IB_LOG_ERROR0("failed to get SM port");
            IB_EXIT(__func__, VSTATUS_BAD);
            return(VSTATUS_BAD);
        }

        temp_lmc = sm_topop->node_head->nodeInfo.NodeType == NI_TYPE_SWITCH ? (sm_topop->node_head->switchInfo.u2.s.EnhancedPort0 ? sm_config.lmc_e0 : 0) : sm_config.lmc; 
		sm_lid = sm_check_lid(sm_topop->node_head, portp, temp_lmc);
		if (sm_lid == RESERVED_LID) {
			sm_lid = sm_set_lid(sm_topop->node_head, portp, temp_lmc);
		}
	}

    /* 
     * add ourselves to the SM list during first sweep
     */
    if (!topology_passcount) {
        sminforec.RID.LID = sm_lid;
        memcpy((void *)&sminforec.SMInfo, (void *)&sm_smInfo, sizeof(sminforec.SMInfo));
        if (!sm_valid_port((portp = sm_get_port(sm_topop->node_head,sm_config.port)))) {
            IB_LOG_ERROR0("failed to get SM port");
            IB_EXIT(__func__, VSTATUS_BAD);
            return(VSTATUS_BAD);
        }

        (void) sm_dbsync_addSm(sm_topop->node_head, portp, &sminforec);
    }
	
	// discovery went well, build the node array
	status = sm_build_node_array(sm_topop);
	if (status != VSTATUS_OK)
		IB_LOG_INFINI_INFORC("failed to build node array: node lookup optimization is disabled for this sweep. rc:", status);

	// This was moved out of the above loop because that loop only considers
	// switch nodes.  This can be moved back in if that loop doesn't skip
	// non-switch nodes
	if (sm_config.cableInfoPolicy > CIP_NONE) {
		for_all_nodes(sm_topop, nodep) {
			for_all_ports(nodep, portp) {
				if (sm_Port_t_IsCableInfoSupported(portp) && portp->state == IB_PORT_ACTIVE) {

					// CableInfo_t is reference counted, so acquire mutex before copying
					// May not be necessary if all other threads are read only and don't care about the reference
					// count since this process will never result in a CableInfo_t
					// being freed
					if (vs_wrlock(&old_topology_lock) == VSTATUS_OK) {
						Node_t * oldNode = sm_find_guid(&old_topology, nodep->nodeInfo.NodeGUID);

						Port_t * oldPort = NULL;

						if (oldNode)
							oldPort = sm_get_port(oldNode, portp->index);

						if (sm_valid_port(oldPort) && oldPort->portData->cableInfo) {
							portp->portData->cableInfo = sm_CableInfo_copy(oldPort->portData->cableInfo);

							if (!portp->portData->cableInfo) {
								IB_LOG_ERROR_FMT(__func__,
									"Failed to copy cable data from old_topology to current topology"
									".  Node %s node GUID "FMT_U64" port %d",
									sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, portp->index);
							}
						}

						vs_rwunlock(&old_topology_lock);
					}
				}
			}
		}
	}
/*
#if (defined BIU)||(defined BIUSI)
if(strcmp(sm_topop->routingModule->name,"dorbiu")==0||strcmp(sm_topop->routingModule->name,"dorbiusi")==0){
	Node_t *swnodep;
	IB_LOG_WARN_FMT(__func__,"zp log : switch list start");
	for_all_switch_nodes(sm_topop,swnodep){
		DorBiuNode_t *swdornodep=(DorBiuNode_t*)swnodep->routingData;
		int i=0;
		int offset=0;
		char string[DORBIU_COORDINATE_STRING_LEN]={0};
		{
			offset+=sprintf(string+offset,"(");
			offset+=sprintf(string+offset,"%d:",SM_DOR_MAX_DIMENSIONS);
			for(i=0;i<SM_DOR_MAX_DIMENSIONS;i++){
				offset+=sprintf(string+offset,"%d",swdornodep->coords[i]);
				if(offset>(DORBIU_COORDINATE_STRING_LEN/4*3)){
					IB_LOG_WARN_FMT(__func__,"zp log : DORBIU_COORDINATE_STRING_LEN is too small to show coordinate !");
					break;
				}
				if( i == (SM_DOR_MAX_DIMENSIONS-1) )continue;
				offset+=sprintf(string+offset,",");
			}
			sprintf(string+offset,")");
			IB_LOG_WARN_FMT(__func__,"zp log : swnode--%s ",string);
		}
	}
	IB_LOG_WARN_FMT(__func__,"zp log : switch list end");
	
	IB_LOG_WARN_FMT(__func__,"zp log : link list start");
	for_all_switch_nodes(sm_topop,swnodep){
		DorBiuNode_t *swdornodep=(DorBiuNode_t*)swnodep->routingData;
		int i=0;
		for(i=0;i<SM_DOR_MAX_DIMENSIONS;i++){
			if(swdornodep->left[i]!=NULL){
				IB_LOG_WARN_FMT(__func__,"zp log : %d-left->%d ",swdornodep->node->swIdx,swdornodep->left[i]->node->swIdx);
			}
			if(swdornodep->right[i]!=NULL){
				IB_LOG_WARN_FMT(__func__,"zp log : %d-right->%d ",swdornodep->node->swIdx,swdornodep->right[i]->node->swIdx);
			}
		}
		if(swdornodep->brother!=NULL){
			IB_LOG_WARN_FMT(__func__,"zp log : %d-brother->%d ",swdornodep->node->swIdx,swdornodep->brother->swIdx);
		}
	}
	IB_LOG_WARN_FMT(__func__,"zp log : link list end");
}
	
#endif 
*/
    if (smDebugPerf) {
        vs_time_get(&eTime);
        IB_LOG_INFINI_INFO("END directed route exploration of fabric, elapsed time(usecs)=", 
                           (int)(eTime-sTime));
    }

	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

/*
 * Determine wheter we should be the MAster of the fabric using 
 * the info collected during the discovery phase.
 */
Status_t
topology_transition(void)
{
	Status_t	status;
    uint8_t     *path;
    uint8_t     doHandover=0, suspendDiscovery=0;
    uint8_t     willHandover=0;
	STL_SM_INFO smInfoCopy;
    SmRecp      smrecp;
    CS_HashTableItr_t itr;
    SmRecp      topSm=NULL; 

	IB_ENTER(__func__, 0, 0, 0, 0);

    /* 
     * Find highest priority SM in list with the proper SMKey other than us
     * We are the first entry in the list
     */
    if (cs_hashtable_count(smRecords.smMap) > 1) {
        sm_saw_another_sm = TRUE;
        cs_hashtable_iterator(smRecords.smMap, &itr);
        do {
            smrecp = cs_hashtable_iterator_value(&itr);
            /* skip ourselves, inactive SM's, and SM's without the proper SMKey */
            if (smrecp->portguid == sm_smInfo.PortGUID ||   /* this is our entry */
                smrecp->smInfoRec.SMInfo.u.s.SMStateCurrent == SM_STATE_NOTACTIVE || 
                smrecp->smInfoRec.SMInfo.SM_Key != sm_smInfo.SM_Key) {
                continue;
            } else if (topSm) {
                if (topSm->smInfoRec.SMInfo.u.s.Priority > smrecp->smInfoRec.SMInfo.u.s.Priority) {
                    /* topSm is still the one */
                } else if (topSm->smInfoRec.SMInfo.u.s.Priority < smrecp->smInfoRec.SMInfo.u.s.Priority) {
                    topSm = smrecp;
                } else if (topSm->smInfoRec.SMInfo.PortGUID > smrecp->smInfoRec.SMInfo.PortGUID) {
                    topSm = smrecp;
                }
            } else {
                topSm = smrecp;
            }
        } while (cs_hashtable_iterator_advance(&itr));
    }

    if (!topSm) {
        /* we are it; become or stay master */
        if (sm_state != SM_STATE_MASTER)
            (void)sm_transition(SM_STATE_MASTER);
        // no other SM and we're master... re-elevate our priority
        sm_elevatePriority();
        return(VSTATUS_OK);
    }

    /* 
     * Determine who should be MASTER based on the rules outlined in Infiniband 
     * Architecture Release 1.2 Volume 1 - General Specification, section 14.4.1 
     */  
    switch (sm_smInfo.u.s.SMStateCurrent) {
    case SM_STATE_DISCOVERING:
        switch (topSm->smInfoRec.SMInfo.u.s.SMStateCurrent) {
        case SM_STATE_MASTER:
            /* make the transition to STANDBY now, remote will decide if we should takeover */
            IB_LOG_INFO_FMT(__func__, "%s STANDBY, found MASTER SM %s : "FMT_U64,
                   (sm_smInfo.u.s.SMStateCurrent==SM_STATE_DISCOVERING) ? "Becoming" : "Remaining in", 
                   topSm->nodeDescString, topSm->smInfoRec.SMInfo.PortGUID);
            (void)memcpy((void *)sm_topop->sm_path, (void *)topSm->path, 64);
            (void)sm_transition(SM_STATE_STANDBY);
            break;
        case SM_STATE_STANDBY:
        case SM_STATE_DISCOVERING:
            /* determine who should become master based on priority/Guid rules */
            if (sm_smInfo.u.s.Priority > topSm->smInfoRec.SMInfo.u.s.Priority || 
                (sm_smInfo.u.s.Priority == topSm->smInfoRec.SMInfo.u.s.Priority && sm_smInfo.PortGUID < topSm->smInfoRec.SMInfo.PortGUID)) {
                /* make the transition to MASTER now */
                IB_LOG_INFO_FMT(__func__, FMT_U64" becoming MASTER over remote SM %s : "FMT_U64,
                       sm_smInfo.PortGUID, topSm->nodeDescString, topSm->smInfoRec.SMInfo.PortGUID);
                (void)sm_transition(SM_STATE_MASTER);
            } else {
                /* make the transition to STANDBY now */
                IB_LOG_INFO_FMT(__func__, FMT_U64" becoming STANDBY, remote %s : "FMT_U64" will be MASTER",
                       sm_smInfo.PortGUID, topSm->nodeDescString, topSm->smInfoRec.SMInfo.PortGUID);
                (void)memcpy((void *)sm_topop->sm_path, (void *)topSm->path, 64);
                (void)sm_transition(SM_STATE_STANDBY);
            }
            break;
        default:
            IB_LOG_INFINI_INFO_FMT(__func__, "Remote SM %s : "FMT_U64" is not Active",
                   topSm->nodeDescString, topSm->smInfoRec.SMInfo.PortGUID);
            break;      // do nothing for Not Active state
        } /* end switch on your_state */
        break;
    case SM_STATE_MASTER:
        switch (topSm->smInfoRec.SMInfo.u.s.SMStateCurrent) {
        case SM_STATE_MASTER:
        case SM_STATE_STANDBY:
            /* determine who should be master based on priority/Guid rules */
            if (sm_smInfo.u.s.Priority < topSm->smInfoRec.SMInfo.u.s.Priority || 
                (sm_smInfo.u.s.Priority == topSm->smInfoRec.SMInfo.u.s.Priority && sm_smInfo.PortGUID > topSm->smInfoRec.SMInfo.PortGUID)) {
                willHandover = 1;  // will eventually handover once dbsync done
                /* handover to any master SM and to stanby SMs that support synchronization if they're in synchronized state */
				char *reason;
				if (sm_dbsync_isUpToDate(topSm->smInfoRec.SMInfo.PortGUID, &reason)) {
					doHandover = 1;
				} else {
                    IB_LOG_INFINI_INFO_FMT(__func__, 
                           "Delaying handover to remote standby SM %s : "FMT_U64": %s",
                           topSm->nodeDescString, topSm->smInfoRec.SMInfo.PortGUID,reason);
				}
            } else if (topSm->smInfoRec.SMInfo.u.s.SMStateCurrent == SM_STATE_MASTER) {
                /* 
                 * Remote, if Master, is expected to relinquish his part of subnet by sending us a HANDOVER
                 */
                IB_LOG_WARN_FMT(__func__, "Suspending Discovery - expecting handover from Master SM %s : "FMT_U64,
                       topSm->nodeDescString, topSm->smInfoRec.SMInfo.PortGUID);
                /* suspend discovery until remote hands over */
                suspendDiscovery = 1;
            }
            break;
        case SM_STATE_DISCOVERING:
            if (sm_smInfo.u.s.Priority < topSm->smInfoRec.SMInfo.u.s.Priority || 
                (sm_smInfo.u.s.Priority == topSm->smInfoRec.SMInfo.u.s.Priority && sm_smInfo.PortGUID > topSm->smInfoRec.SMInfo.PortGUID)) {
                willHandover = 1;  // will eventually handover once discovers
                IB_LOG_INFINI_INFO_FMT(__func__, "Remote SM %s : "FMT_U64" is still DISCOVERING, will catch next sweep",
                       topSm->nodeDescString, topSm->smInfoRec.SMInfo.PortGUID);
            }
            break;
        default:
            IB_LOG_INFINI_INFO_FMT(__func__, "Remote SM %s : "FMT_U64" is not Active",
                   topSm->nodeDescString, topSm->smInfoRec.SMInfo.PortGUID);
            break;      // do nothing for Not Active state
        } /* end switch on your_state */
        break;
    case SM_STATE_STANDBY:
        IB_LOG_WARN_FMT(__func__, 
               "The SM on this node["FMT_U64"] is in STANDBY and should just be polling the master", sm_smInfo.PortGUID);
        break;
    default:
        IB_LOG_INFINI_INFO_FMT(__func__, "The SM on this node["FMT_U64"] is not Active", sm_smInfo.PortGUID);
        break;      /* should not be here if we're not active */
    } /* end switch on my_state */

    /*
     * If we are relinquishing MASTERness of the subnet to the other guy, 
     * we must send him a HANDOVER request.
     */
    if (doHandover) {
        sm_smInfo.ActCount++;
        smInfoCopy = sm_smInfo;
        //path = PathToPort(topSm->nodep, topSm->portp);
        path = topSm->path;
        status = SM_Set_SMInfo(fd_topology, SM_AMOD_HANDOVER, path, &smInfoCopy, sm_config.mkey);
        if (status != VSTATUS_OK) {
            IB_LOG_ERROR_FMT(__func__, "could not perform HANDOVER to remote SM %s : "FMT_U64,
                   topSm->nodeDescString, topSm->smInfoRec.SMInfo.PortGUID);
        } else {
            IB_LOG_INFINI_INFO_FMT(__func__, "sent HANDOVER to remote SM %s : "FMT_U64,
                   topSm->nodeDescString, topSm->smInfoRec.SMInfo.PortGUID);
            /* set path to new master now but don't change state till ACK received in async */
            (void)memcpy((void *)sm_topop->sm_path, (void *)topSm->path, 64);
        }
        /* suspend discovery when handing over */
        suspendDiscovery = 1;
		/* set handover flag to indicate async thread (sm_fsm.c) that we have processed the handover.
		 * do this even in case of failing to successfully send the handover, otherwise async thread
		 * will not trigger a sweep for handover
		 */
		(void)vs_lock(&handover_sent_lock);
		handover_sent = 1;
		(void)vs_unlock(&handover_sent_lock);
    } else if (sm_state == SM_STATE_MASTER && ! willHandover) {
        // we don't plan to handover and we're master... re-elevate our priority
		sm_elevatePriority();
    }

    /* 
     * stop discovery if not master or dual master
     * In dual master case, we will suspend discovery actions 
     * until remote hands over control of it's portion of subnet
     */
    if (sm_state != SM_STATE_MASTER || suspendDiscovery) 
        status = VSTATUS_NOT_MASTER;
    else {
        status = VSTATUS_OK;
    }

	IB_EXIT(__func__, status);
	return(status);
} /* topology_transition */

Status_t
topology_resolve(void)
{
	int		max_lid;
	int		delta;
	uint32_t	temp_lmc;
	Node_t		*nodep;
	Port_t		*portp;
    uint64_t    sTime, eTime;

	IB_ENTER(__func__, 0, 0, 0, 0);

    if (smDebugPerf) {
        vs_time_get(&sTime);
        IB_LOG_INFINI_INFO0("STARTING resolve topology pass");
    }

//
//	Setup the basic LID information for the topology.
//

	sm_topop->lmc = sm_config.lmc;
    temp_lmc = sm_topop->node_head->nodeInfo.NodeType == NI_TYPE_SWITCH ? (sm_topop->node_head->switchInfo.u2.s.EnhancedPort0 ? sm_config.lmc_e0 : 0) : sm_config.lmc; 

    if (!sm_valid_port((portp = sm_get_port(sm_topop->node_head,sm_config.port)))) {
        IB_LOG_ERROR0("failed to get SM port");
        IB_EXIT(__func__, VSTATUS_BAD);
        return(VSTATUS_BAD);
    }

	sm_lid = sm_check_lid(sm_topop->node_head, portp, temp_lmc);
	if (sm_lid == RESERVED_LID) {
		topology_changed = 1; 
		sm_lid = sm_set_lid(sm_topop->node_head, portp, temp_lmc);
	}
		
	sm_topop->lid = sm_lid;

//
//	This routine tries to reconcile the old view of the fabric (pointed
//	to by 'old_topology') with the new view of the fabric (pointed to
//	by 'sm_newTopology').  We try to keep everything the same between
//	the 2 view in order to not disturb things that are already in place.
//

//
//	If we aren't the master SM, then we go no further.
//
	if (sm_state != SM_STATE_MASTER) {
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

//
//	For every link that is up and doesn't have a LID assigned, we need to
//	give it a range of LIDs.
//

	for_all_nodes(&sm_newTopology, nodep) {
        temp_lmc = nodep->nodeInfo.NodeType == NI_TYPE_SWITCH ? (nodep->switchInfo.u2.s.EnhancedPort0 ? sm_config.lmc_e0 : 0) : sm_config.lmc; 
		for_all_end_ports(nodep, portp) {
			if (!sm_valid_port(portp) || portp->state <= IB_PORT_DOWN) {
				continue;
			}
            /* reset lid if it's no good */
            portp->portData->lid = sm_check_lid(nodep, portp, temp_lmc);
		}
	}

	for_all_nodes(&sm_newTopology, nodep) {
        temp_lmc = nodep->nodeInfo.NodeType == NI_TYPE_SWITCH ? (nodep->switchInfo.u2.s.EnhancedPort0 ? sm_config.lmc_e0 : 0) : sm_config.lmc; 
		for_all_end_ports(nodep, portp) {
			if (!sm_valid_port(portp) || portp->state <= IB_PORT_DOWN) {
				continue;
			}

			if (portp->portData->lid == RESERVED_LID || portp->portData->lid == PERMISSIVE_LID || portp->portData->lid == STL_LID_PERMISSIVE) {
				topology_changed = 1;
				if (sm_set_lid(nodep, portp, temp_lmc) == RESERVED_LID) {
					IB_LOG_ERROR0("no more LID space");
					sm_mark_link_down(&sm_newTopology, portp);
				}
			}
		}
	}

//
//	Find the maximum LID assigned for using in the LFT later.
//
	sm_newTopology.maxLid = 0;

	for_all_nodes(&sm_newTopology, nodep) { 
		for_all_end_ports(nodep, portp) {
			if (!sm_valid_port(portp) || portp->state <= IB_PORT_DOWN) {
				continue;
			}

			if ((portp->portData->lid != RESERVED_LID) &&
                (portp->portData->lid != PERMISSIVE_LID) &&
                (portp->portData->lid != STL_LID_PERMISSIVE)) {
				delta = 1 << portp->portData->lmc; 
				max_lid = portp->portData->lid + delta - 1;
				if (max_lid > sm_newTopology.maxLid) {
					sm_newTopology.maxLid = max_lid;
				}
			}
		}
	}

    if (smDebugPerf) {
        vs_time_get(&eTime);
        IB_LOG_INFINI_INFO("END resolve topology pass, elapsed time(usecs)=", (int)(eTime-sTime));
    }
	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

Status_t topology_sm_port_init_failure()
{
	Status_t		status=VSTATUS_BAD;

	topo_errors = 1;
    topo_abandon_count++;

    /* PR 111089 - For HSM Enable retry backoff trying in multiples of 5s
     * with an upper limit of 15s.
     */
    sm_port_retry_count++;
    topo_enable_retry_backoff(5, 15, 0, sm_port_retry_count);

    IB_LOG_ERROR_FMT(__func__,
               "Unable to initialize the local port, please check the state of the SM device");
    return(status);
}

/* Initialize port 0 of switches and setup LFTs using mixed LR-DR SMPs*/
Status_t topology_setup_switches_LR_DR()
{
	SwitchList_t	root_switch;
	Status_t		status;
	Port_t			*portp;
	int				rebalance = forceRebalanceNextSweep;
	int				routing_needed = 1;
	int				route_root_switch = 0;
	uint64_t		sTime, eTime;

	if (smDebugPerf) {
		vs_time_get(&sTime);
		IB_LOG_INFINI_INFOX("STARTING ; MAXLID=", sm_topop->maxLid);
	}

	status = topology_setup_routing_cost_matrix();
	
	if (status == VSTATUS_KNOWN)	{
		routing_needed = 0;	/* Known topology i.e no changes in topology, old lft data was copied. No new LFT programing required */
	} else if (status != VSTATUS_OK) {
		IB_LOG_INFINI_INFO_FMT(__func__, "Routing calculations failed");
		return status;
	} else {
		routing_needed = 1;
	}

	if (sm_config.force_rebalance || rebalance || sm_config.forceAttributeRewrite) {
		routing_needed = 1;
		rebalance = 1;
	}

	/* First initialize and assign LID to the SM port so that SM can send/recv LR SMPs*/
	portp = sm_get_port(sm_topop->node_head, sm_config.port);
	if (!sm_valid_port(portp)) {
		IB_LOG_WARN_FMT(__func__, "Failed to get SM's port "FMT_U64,
				sm_topop->node_head->nodeInfo.NodeGUID);
		return VSTATUS_BAD;
	}
	status = sm_initialize_port_LR_DR(sm_topop, sm_topop->node_head, portp);
	if (status != VSTATUS_OK) {
    	/* we are unable to initialize the local port */
		return topology_sm_port_init_failure();
	} else {
		sm_port_retry_count = 0;
	}

	/* Check if we have any switches at all - HSM back to back with another host */
	if (sm_topop->num_sws == 0)
		return VSTATUS_OK;

	if (sm_topop->routingModule->funcs.init_switch_routing) {
		status = sm_topop->routingModule->funcs.init_switch_routing(sm_topop, &routing_needed, &rebalance);

		if (status != VSTATUS_OK) {
			IB_LOG_ERROR_FMT(__func__,
				"Switch forwarding table initialization failed.  status : %d",
				status);
			return status;
		}
	}

	/* First program the root switch*/
	/* RMW - TBD - this will write entry switch LRDR even in case of loss/gain of remote ISL */
	if (!bitset_test(&old_switchesInUse, sm_topop->switch_head->swIdx) || routing_needed)
		route_root_switch = 1;
	else
		route_root_switch = 0;

	/* initialize port 0 of root switch and program minimal LFT if routing is required*/
	if ((status = sm_initialize_switch_LR_DR(sm_topop, sm_topop->switch_head, 0, 0, route_root_switch)) != VSTATUS_OK) {
		IB_LOG_INFINI_INFO_FMT(__func__, "Basic LFT programming failed for root switch "FMT_U64" status %d",
				sm_topop->switch_head->nodeInfo.NodeGUID, status);
		if (sm_topop->deltaLidBlocks_init) {
			bitset_free(&sm_topop->deltaLidBlocks);
			sm_topop->deltaLidBlocks_init=0;
		}
		return status;
	}

	if (route_root_switch) {
		Port_t *neighbor_portp = sm_find_port(sm_topop, portp->nodeno, portp->portno);
		/* Program full LFT*/
		root_switch.switchp = sm_topop->switch_head;
		root_switch.next = NULL;

		if ((neighbor_portp == NULL) || (status = sm_initialize_port(sm_topop, sm_topop->switch_head, neighbor_portp, 0, NULL) != VSTATUS_OK)) {
			IB_LOG_INFINI_INFO_FMT(__func__, "Neighbor port programming of root switch "FMT_U64" failed with status %d",
					sm_topop->switch_head->nodeInfo.NodeGUID, status);
			if (sm_topop->deltaLidBlocks_init) {
				bitset_free(&sm_topop->deltaLidBlocks);
				sm_topop->deltaLidBlocks_init=0;
			}
			return status;
		}

		// PR-119954 reported a memory leak in the case when sm_calculate_lft() is called to allocate a new lft for the new
		//			 root node structure and this lft is replaced by another lft in sm_routing_route_old_switch_LR() without
		//			 freeing the lft allocated in sm_calculate_lft().  This function was updated so that 
		//			 route_root_switch is ORed with rebalance in the call to sm_routing_route_switch_LR().  
		//			 This prevents sm_routing_route_old_switch_LR() from being called in the above scenario.  This fixes the memory leak.
		if ((status = sm_routing_route_switch_LR(sm_topop, &root_switch, route_root_switch || rebalance)) != VSTATUS_OK) {
			IB_LOG_INFINI_INFO_FMT(__func__, "Full LFT programming failed for root switch "FMT_U64" status %d",
					sm_topop->switch_head->nodeInfo.NodeGUID, status);
			if (sm_topop->deltaLidBlocks_init) {
				bitset_free(&sm_topop->deltaLidBlocks);
				sm_topop->deltaLidBlocks_init=0;
			}
			return status;
		}		
	}

	sm_topop->switch_head->initDone = 1;

	status = sm_topop->routingModule->funcs.setup_switches_lrdr(sm_topop, rebalance, routing_needed);

	if (sm_topop->deltaLidBlocks_init) {
		bitset_free(&sm_topop->deltaLidBlocks);
		sm_topop->deltaLidBlocks_init=0;
	}

	if (smDebugPerf) {
		vs_time_get(&eTime);
		IB_LOG_INFINI_INFO("END time=", (int)(eTime-sTime));
	}
	return status;
}

static Status_t buffer_control_assignments(void)
{
	Status_t                  status;
	Port_t                   *portp;
	Node_t                   *nodep;

    //
    //	For every node/port combo, we need to setup the VL Buffer Control.
    //
	for_all_nodes(sm_topop, nodep) {
		int needSet = 0;
		/* use other memory allocators here */
		STL_BUFFER_CONTROL_TABLE *tmp_bcts;
		size_t bcts_size = sizeof(STL_BUFFER_CONTROL_TABLE) * (nodep->nodeInfo.NumPorts+1);
		status = vs_pool_alloc(&sm_pool, bcts_size, (void**)&tmp_bcts);
		if (status != VSTATUS_OK) {
			cs_log(VS_LOG_ERROR, "buffer_control_assignments",
				"Failed to allocate buffer control table temp mem; node %s",
				sm_nodeDescString(nodep));
			return (VSTATUS_NOMEM);
		}
		memset(tmp_bcts, 0, bcts_size);

        for_all_ports(nodep, portp) {
            if (!sm_valid_port(portp) || portp->state <= IB_PORT_DOWN)
				continue;

			if (nodep->nodeInfo.NodeType == STL_NODE_SW && portp->index == 0)
				continue;

			if ((status=sm_initialize_Port_BfrCtrl(sm_topop, nodep, portp,
											&tmp_bcts[portp->index]))
					!= VSTATUS_OK) {
			    if (++topo_errors > sm_config.topo_errors_threshold &&
						++topo_abandon_count <= sm_config.topo_abandon_threshold) {
					goto exit;
			    }
			    sm_mark_link_down(sm_topop, portp);
			    topology_changed = 1;   /* indicates a fabric change has been detected */
			    IB_LOG_ERROR_FMT(__func__,
			           "Failed to init Buffer Control (setting port down) on node %s nodeGuid "FMT_U64" node index %d port index %d",
			           sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, nodep->index, portp->index);
			} else {
				Node_t *old_node = sm_find_guid(&old_topology, nodep->nodeInfo.NodeGUID);
				if (old_node) {
					Port_t *old_port = sm_get_port(old_node, portp->index);
					if (!sm_valid_port(old_port) /* Port did not exist before */
								/* OR new sweep does not match old sweep */
							|| memcmp(&portp->portData->bufCtrlTable,
								&old_port->portData->bufCtrlTable,
								sizeof(portp->portData->bufCtrlTable))
								/* OR new data does not match old sweep */
							|| memcmp(&tmp_bcts[portp->index],
								&old_port->portData->bufCtrlTable,
								sizeof(old_port->portData->bufCtrlTable))) {
						needSet = 1;
					} else {
						/* If the BCT's were read in from the SMA this is not
						 * needed.  However it is anticipated that the SMA read
						 * will be optimized away at some point.  If they are
						 * this will be required to ensure correctness within
						 * the SA.
						 */
						memcpy(&portp->portData->bufCtrlTable,
								&tmp_bcts[portp->index],
								sizeof(portp->portData->bufCtrlTable));
					}
				} else {
					needSet = 1;
				}
			}
			// PR 125129 - Set all ports individually, no multi-block write
			// Temporary to move forward with debug, but should optimize in future
			//if ((sm_config.forceAttributeRewrite || needSet) && nodep->nodeInfo.NodeType == NI_TYPE_CA) {
			if (sm_config.forceAttributeRewrite || needSet) {
				if ((status=sm_set_buffer_control_tables(fd_topology, nodep, portp->index,
												portp->index, &tmp_bcts[portp->index]))
									!= VSTATUS_OK) {
					if (++topo_errors > sm_config.topo_errors_threshold &&
							++topo_abandon_count <= sm_config.topo_abandon_threshold) {
						goto exit;
					}
				}
				needSet = 0;
			}
        }
		// PR 125129 - Do not do switches in bulk.
		// Temporary to move forward with debug, but should optimize in future
		///* set Switches in bulk */
		//if ((needSet || sm_config.forceAttributeRewrite) && nodep->nodeInfo.NodeType == NI_TYPE_SWITCH) {
		//	if (sm_set_buffer_control_tables(fd_topology, nodep, 1,
		//									nodep->nodeInfo.NumPorts, &tmp_bcts[1])
		//				!= VSTATUS_OK) {
		//		if (++topo_errors > sm_config.topo_errors_threshold &&
		//				++topo_abandon_count <= sm_config.topo_abandon_threshold) {
		//			goto exit;
		//		}
		//	}
		//}

exit:
		vs_pool_free(&sm_pool, tmp_bcts);

		/* early exit on error or shutdown */
		if (status != VSTATUS_OK)
			return (status);
		if (topology_main_exit == 1)
		    return(VSTATUS_OK);
	}

	return (VSTATUS_OK);
}

Status_t
topology_assignments(void)
{
	Port_t			*portp;
	Node_t			*nodep;
	Status_t		status;
    uint64_t        sTime, iTime=0, eTime;
	uint8_t			vlInUse = 0xff;
	Node_t			*oldnodep;

	IB_ENTER(__func__, 0, 0, 0, 0);

    //
    //	If we aren't the master SM, then we go no further.
    //
	if (sm_state != SM_STATE_MASTER) {
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

    //
    // loop test - find loop paths through the switches
    //
    if ((status = loopTest_userexit_findPaths(sm_topop)) != VSTATUS_OK) {
        IB_LOG_ERROR_FMT(__func__, "loopTest can't generate lids");
    } else if (loopPathLidEnd != 0) {
        sm_topop->maxLid = loopPathLidEnd;
    }

    if (smDebugPerf) {
        vs_time_get(&sTime);
        IB_LOG_INFINI_INFOX("STARTING topology assignments; START switches setup; MAXLID=",sm_topop->maxLid);
    }
    //
    //	For each Node in the fabric, we need to initialize the fields as
    //	specified in IBTA:  Volume 1, Section 14.4.3.
    //
    for_all_switch_nodes(sm_topop, nodep) {
    /* skip switches that are not under our control */
	/* MWHEINZ - FIXME Is this test actually valid? Under what circumstance 
 	 * could we have "switches that are not under our control"? */
    if (sm_valid_port((portp = sm_get_port(nodep,0)))) {
        uint32_t    doSet=0;
        uint8_t     pauseState=0;
        /* if getSwitchInfo failed during discovery sweep, try now */
        if (nodep->switchInfo.LinearFDBCap == 0 && nodep->switchInfo.MulticastFDBCap == 0) {
            status = SM_Get_SwitchInfo(fd_topology, 0, nodep->path, &nodep->switchInfo);
            if (status != VSTATUS_OK) {
                IB_LOG_ERROR_FMT(__func__,
                   "Failed to get Switchinfo for node %s guid "FMT_U64": status = %d",
                   sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, status);
                IB_EXIT(__func__, status);
                return(status);
            }
        }
        /* check MFTCap is sufficient */
        if (nodep->switchInfo.MulticastFDBCap < sm_mcast_mlid_table_cap) {
           IB_LOG_WARN_FMT(__func__,
                   "MLIDTableCap %u exceeds MFT Cap %u of Switch %s guid "FMT_U64,
                   sm_mcast_mlid_table_cap, nodep->switchInfo.MulticastFDBCap,
                   sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID);
        }
        /* make the necessary updates */
        if (nodep->switchInfo.LinearFDBTop != sm_topop->maxLid) {
            nodep->switchInfo.LinearFDBTop = sm_topop->maxLid;
        	if (nodep->switchInfo.LinearFDBCap < sm_topop->maxLid) {
                IB_LOG_ERROR_FMT(__func__,
                   		"Switchinfo LinearFDBCap too low for node %s guid "FMT_U64": cap = %d, maxLid = %d",
                   		sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID,
						nodep->switchInfo.LinearFDBCap, sm_topop->maxLid);
				nodep->switchInfo.LinearFDBTop = nodep->switchInfo.LinearFDBCap;
			}
            doSet = 1;
        }

        {
            Lid_t maxMCLid;
            if (nodep->switchInfo.MulticastFDBTop != (maxMCLid = sm_multicast_get_max_lid())) {
                nodep->switchInfo.MulticastFDBTop = maxMCLid;
                doSet = 1;
            }
        }

        // Check for pause indication, needs to be set if routing changes are being made or additional
        // alternate routes indicated.
        pauseState = (nodep->arSupport && (topology_changed || topology_passcount == 0 ||
                            forceRebalanceNextSweep ||
                            !bitset_equal(&old_switchesInUse, &new_switchesInUse) ||
                            new_endnodesInUse.nset_m ||
                            old_topology.num_endports != sm_newTopology.num_endports ||
                            old_topology.num_sws != sm_newTopology.num_sws)) ? 1 : 0;

        if (sm_VerifyAdaptiveRoutingConfig(nodep)) {
            doSet = 1;
            if (sm_adaptiveRouting.debug) {
                IB_LOG_INFINI_INFO_FMT(__func__, "Switch node %s guid "FMT_U64" setting config vendor info",
                        sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID);
            }
        }

        if (pauseState != nodep->switchInfo.AdaptiveRouting.s.Pause) {
            // Pause indicated or switch is in bad pause state.
            doSet = 1;

        } else if (sm_adaptiveRouting.debug) {
            IB_LOG_INFINI_INFO_FMT(__func__, "Switch node %s guid "FMT_U64" already has correct pause state %d",
                        sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, pauseState);
        }

        /* 
         * portChange indicates switch had a port state change event on one of it's ports
         * We have to echo this back to clear the bit 
         */
        if (nodep->switchInfo.u1.s.PortStateChange) {
            doSet = 1;
            IB_LOG_INFO_FMT(__func__,
                   "Switch node %s guid "FMT_U64": port mkey="FMT_U64", SM mkey="FMT_U64" had a port state change",
                   sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, portp->portData->portInfo.M_Key, sm_config.mkey);
        }
        // Check the switch lifetime value.
        if (nodep->switchInfo.u1.s.LifeTimeValue != sm_config.switch_lifetime_n2) {
            nodep->switchInfo.u1.s.LifeTimeValue = sm_config.switch_lifetime_n2;
            doSet = 1;
            IB_LOG_INFO_FMT(__func__,
                   "Switch node %s guid "FMT_U64": updating switch lifetime value to %d",
                   sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, sm_config.switch_lifetime_n2);
        }
        if (doSet || sm_config.forceAttributeRewrite) {
            // make the necessary changes at the switch, update the SwitchInfo
            // data of the switch
            nodep->switchInfo.AdaptiveRouting.s.Pause = pauseState ? 1 : 0;
            status = SM_Set_SwitchInfo(fd_topology, 0, nodep->path, &nodep->switchInfo, portp->portData->portInfo.M_Key);

            if (status == VSTATUS_OK) {
                nodep->switchInfo.AdaptiveRouting.s.Pause = pauseState ? 1 : 0;
                if (sm_adaptiveRouting.debug) {
                    IB_LOG_INFINI_INFO_FMT(__func__,
                        "Setting adaptive routing pause (%d) for switch %s guid "FMT_U64,
                        pauseState, sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID);
                }

            }
        }
    }
	}

    if (smDebugPerf) {
        vs_time_get(&eTime);
        IB_LOG_INFINI_INFO("topology assignments; END switches setup/START port setup, elapsed time(usecs)=",
                           (int)(eTime-sTime));
        iTime = sTime;
    }

	if (topology_passcount) {
		/* MWHEINZ: FIXME - investigate why we do this here and not in the topology copy routines. */
		/* If we found switches that do not support mixed LR DR SMPs in the last sweep,
		 * copy that information to the new topology so that we do not waste time retrying
		 * sending mixed LR DR SMPs to such switches and directly send only pure DR SMPs.
		 */
		for_all_switch_nodes(&old_topology, oldnodep) {
			if (oldnodep->noLRDRSupport) {
				/* Find the node in the new topology*/
				nodep = sm_find_guid(sm_topop, oldnodep->nodeInfo.NodeGUID);
				if (nodep)
					nodep->noLRDRSupport = 1;
			}
		}
	}

	/* Setup the switches using mixed LR-DR SMPs*/
	if ((status = topology_setup_switches_LR_DR()) != VSTATUS_OK) {
		IB_LOG_INFINI_INFO_FMT(__func__, "Setting up switches with mixed LR-DR SMPs failed");
		return status;
	}

    //
    //      Set the Lids for all end ports.
    //
	for_all_nodes(sm_topop, nodep) {
		vlInUse = 0xff;
		for_all_ports(nodep, portp) { 
			if (sm_valid_port(portp) && portp->state > IB_PORT_DOWN) {
				/* Initialize ports using mixed LR-DR SMPs*/
				if ((nodep->nodeInfo.NodeType == NI_TYPE_SWITCH && portp->index == 0) ||
					 (nodep == sm_topop->node_head && portp->index == sm_config.port))
						continue;	/* Port 0 of switches and SM port are already initialized above*/
				status = sm_initialize_port_LR_DR(sm_topop, nodep, portp);
				
				if (status != VSTATUS_OK) {
                    /* see if we are unable to initialize the local port */
                    if (nodep == sm_topop->node_head && portp->index == sm_config.port) {
						return topology_sm_port_init_failure();
                    } else {
                        /* when not local port */
						sm_port_retry_count = 0;	/*reset the counter used for retry backoff*/
                        if (topology_main_exit == 1) {
#ifdef __VXWORKS__
                            ESM_LOG_ESMINFO("topology_assignments: SM has been stopped", 0);
#endif
                            IB_EXIT(__func__, VSTATUS_OK);
                            return(VSTATUS_OK);
                        }
                        if (++topo_errors > sm_config.topo_errors_threshold &&
                            ++topo_abandon_count <= sm_config.topo_abandon_threshold) {
                            return(status);
                        }
                    }
                    sm_mark_link_down(sm_topop, portp);
                    topology_changed = 1;   /* indicates a fabric change has been detected */
					IB_LOG_ERROR_FMT(__func__,
					       "Marking port[%d] of node[%d] %s guid "FMT_U64" DOWN in the topology",
					       portp->index, nodep->index, sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID);
					status = VSTATUS_OK;

        		} else if (nodep->nodeInfo.NodeType == NI_TYPE_SWITCH &&
						   (portp->index != 0 || nodep->switchInfo.u2.s.EnhancedPort0)) {
					if (vlInUse == 0xff) {
						vlInUse = portp->portData->vl1;
					} else if (vlInUse != portp->portData->vl1) {
						nodep->uniformVL = 0;
					}
					nodep->vlCap = MAX(nodep->vlCap, portp->portData->vl0);
				}
			}
		}
		if (nodep->nodeInfo.NodeType == NI_TYPE_SWITCH && nodep->uniformVL) {
			nodep->activeVLs = vlInUse;
		}
	}


    if (smDebugPerf) {
        vs_time_get(&eTime);
        IB_LOG_INFINI_INFO("topology assignments; END PORT setup/START SLVL/VLARB setup; elapsed time(usecs)=", 
                           (int)(eTime-iTime));
        iTime = eTime;
    }

	for_all_nodes(sm_topop, nodep) {
		Port_t * smaportp;

		for_all_sma_ports(nodep, smaportp) {
			if (!sm_valid_port(smaportp) || (smaportp->state <= IB_PORT_DOWN)) {
				continue;
			}

			status = sm_node_updateFields(fd_topology, sm_lid, nodep, smaportp);

			if (status != VSTATUS_OK) {
				Port_t *neigh = sm_find_port(sm_topop, smaportp->nodeno, smaportp->portno);

				IB_LOG_WARN_FMT(__func__,
					"Non-OK status returned on updating node attached to %s port %d via SMA:"
					"  Node %s, nodeGuid "FMT_U64", port %d, status %d",
					sm_valid_port(neigh) ? sm_nodeDescString(neigh->portData->nodePtr) : "???", 
					neigh ? neigh->index : -1, sm_nodeDescString(nodep), 
					nodep->nodeInfo.NodeGUID, smaportp->index, status);
				continue;
			}
		}
	}

    //
    //	For every node/port combo, we need to setup the SLVL Mapping.
    //	Note: 9000 Series of QLogic switch is reporting incorrect bit for optimized slvl support
	for_all_nodes(sm_topop, nodep) {
		if (nodep->nodeInfo.NodeType == NI_TYPE_SWITCH) {
			// Node is a switch.
			if ((status = sm_initialize_Switch_SLMaps(sm_topop, nodep)) != VSTATUS_OK) {
				if (topology_main_exit == 1) {
#ifdef __VXWORKS__
					ESM_LOG_ESMINFO("topology_assignments: SM has been stopped", 0);
#endif
					IB_EXIT(__func__, VSTATUS_OK);
					return(VSTATUS_OK);
				}
				if (++topo_errors > sm_config.topo_errors_threshold &&
					++topo_abandon_count <= sm_config.topo_abandon_threshold) {
					return(status);
				}
				// sm_request_resweep(0, 0, "Error programming fabric SC tables")
			}

            // Setup vlarb for each port on switch
            for_all_ports(nodep, portp) {
                if (!sm_valid_port(portp) || portp->state <= IB_PORT_DOWN) {
                    continue; 
                }

				if ((status = sm_initialize_VLArbitration(sm_topop, nodep, portp)) != VSTATUS_OK) {
                    if (topology_main_exit == 1) {
#ifdef __VXWORKS__
                        ESM_LOG_ESMINFO("topology_assignments: SM has been stopped", 0);
#endif
                        IB_EXIT(__func__, VSTATUS_OK);
                        return(VSTATUS_OK);
                    }

                    if (++topo_errors > sm_config.topo_errors_threshold &&
                        ++topo_abandon_count <= sm_config.topo_abandon_threshold) {
                        return(status);
                    }

                    sm_mark_link_down(sm_topop, portp);
                    topology_changed = 1;   /* indicates a fabric change has been detected */
                    IB_LOG_ERROR_FMT(__func__, 
                                     "Failed to init VL Arb (setting port down) on node %s nodeGuid "
                                     FMT_U64 " node index %d port index %d", 
                                     sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, 
                                     nodep->index, portp->index);
				}
            }

			// Do HOQLife initialization
			uint8_t port;
			portp = sm_get_port(nodep, 0);

			if (!sm_valid_port(portp)) continue;

			boolean updHoq = 0;

			for (port = 0; port <= nodep->nodeInfo.NumPorts; port++) {
				Port_t * curPort = sm_get_port(nodep, port);

				if (!sm_valid_port(curPort) || curPort->state <= IB_PORT_DOWN) continue;

				status = UpdateHoq(nodep, curPort, &updHoq);
				if (status != VSTATUS_OK) {
					IB_LOG_ERROR_FMT(__func__,
					"Failed to compute HOQ values for node %s nodeGuid "FMT_U64" port %d status = %d",
					sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, curPort->index, status);

					//@todo: should this always exit, or only once the error threshold has been exceeded
					IB_EXIT(__func__, status);
					return status;
				}

				// If the port is already ARMED, the SM should make sure that HOQLife gets updated.
				// Otherwise, the SM will update HOQLife when it goes to ARM or ACTIVE-ate the port
				if (updHoq && curPort->state >= IB_PORT_ARMED) {
					struct XmitQ_s xmitQVals[STL_MAX_VLS];

					memcpy(xmitQVals, &curPort->portData->portInfo.XmitQ, sizeof(xmitQVals));

					uint32 amod = (1 << 24) | curPort->index;

					Port_t * switchP0 = sm_get_port(nodep, 0);
					STL_LID_32 dlid = switchP0->portData->lid;
					status = SM_Set_PortInfo_LR(fd_topology, amod, sm_lid, dlid,
						&curPort->portData->portInfo, curPort->portData->portInfo.M_Key, NULL);

					if (status != VSTATUS_OK) {
						IB_LOG_ERROR_FMT(__func__,
							"Failed to set PortInfo for node %s nodeGuid "FMT_U64", port %d: status = %d",
							sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, curPort->index, status);
						IB_EXIT(__func__, status);
						return status;
					}

					if (!sm_eq_XmitQ(xmitQVals, curPort->portData->portInfo.XmitQ, curPort->portData->portInfo.s4.OperationalVL)) {
						IB_LOG_ERROR_FMT(__func__,
							"Mismatch between expected and actual XmitQ values on port.  Failed to set PortInfo for node %s nodeGuid "FMT_U64", port %d: status = %d.",
							sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, curPort->index, status);
						IB_EXIT(__func__, VSTATUS_BAD);
						return VSTATUS_BAD;
					}
				}
			}
		}
		else for_all_ports(nodep, portp) {
			// Iterate over the ports of this HFI.
			if (sm_valid_port(portp) && portp->state > IB_PORT_DOWN) {
				if ((status=sm_initialize_Node_SLMaps(sm_topop, nodep, portp)) != VSTATUS_OK) {
                    if (topology_main_exit == 1) {
#ifdef __VXWORKS__
                        ESM_LOG_ESMINFO("topology_assignments: SM has been stopped", 0);
#endif
                        IB_EXIT(__func__, VSTATUS_OK);
                        return(VSTATUS_OK);
                    }
                    if (++topo_errors > sm_config.topo_errors_threshold &&
                        ++topo_abandon_count <= sm_config.topo_abandon_threshold) {
                        return(status);
                    }
                    sm_mark_link_down(sm_topop, portp);
                    topology_changed = 1;   /* indicates a fabric change has been detected */
					IB_LOG_ERROR_FMT(__func__,
					       "Failed to init SL2SC/SC2SL Map (setting port down) on node %s nodeGuid "FMT_U64" node index %d port index %d",
					       sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, nodep->index, portp->index);
					continue;
				}

                if ((status = sm_initialize_VLArbitration(sm_topop, nodep, portp)) != VSTATUS_OK) {
                    if (topology_main_exit == 1) {
#ifdef __VXWORKS__
                        ESM_LOG_ESMINFO("topology_assignments: SM has been stopped", 0);
#endif
                        IB_EXIT(__func__, VSTATUS_OK);
                        return(VSTATUS_OK);
                    }
					
                    if (++topo_errors > sm_config.topo_errors_threshold &&
                        ++topo_abandon_count <= sm_config.topo_abandon_threshold) {
                        return(status);
                    }
                    sm_mark_link_down(sm_topop, portp);
                    topology_changed = 1;   /* indicates a fabric change has been detected */
                    IB_LOG_ERROR_FMT(__func__,
                           "Failed to init VL Arb (setting port down) on node %s nodeGuid "FMT_U64" node index %d port index %d",
                           sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, nodep->index, portp->index);
                }
            }
		}
	}

    if (smDebugPerf) {
        vs_time_get(&eTime);
        IB_LOG_INFINI_INFO("topology assignments; END SLVL/VLARB setup/START BfrCtrl; elapsed time(usecs)=", 
                           (int)(eTime-iTime));
        iTime = eTime;
    }

	if ((status = buffer_control_assignments()) != VSTATUS_OK)
		return (status);

	if (topology_main_exit == 1) {
#ifdef __VXWORKS__
		ESM_LOG_ESMINFO("topology_assignments: SM has been stopped", 0);
#endif
		IB_EXIT(__func__, VSTATUS_OK);
		return(VSTATUS_OK);
	}

    if (smDebugPerf) {
        vs_time_get(&eTime);
        IB_LOG_INFINI_INFO("topology assignments; END BfrCtrl setup; elapsed time(usecs)=", (int)(eTime-iTime));
        IB_LOG_INFINI_INFO("END topology assignments; elapsed time(usecs)=", (int)(eTime-sTime));
    }

	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

Status_t topology_setup_routing_cost_matrix(void)
{
	Status_t	status = VSTATUS_OK;
	uint64_t	sTime, eTime;
	int			newSwitchesInFabric=0;
	int			rebalance;

	IB_ENTER(__func__, 0, 0, 0, 0);

	rebalance = forceRebalanceNextSweep || sm_config.force_rebalance || sm_config.forceAttributeRewrite;

    /* If we aren't the master SM, then we go no further.*/
	if (sm_state != SM_STATE_MASTER) {
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

	if (smDebugPerf) {
        vs_time_get(&sTime);
		IB_LOG_INFINI_INFO("START topology_setup_routing_cost_matrix for nodes=", sm_topop->num_nodes);
		IB_LOG_INFINI_INFO("topology_changed=", topology_changed);
		bitset_info_log(&old_switchesInUse, "old_switchesInUse");
		bitset_info_log(&new_switchesInUse, "new_switchesInUse");
		if (new_endnodesInUse.nset_m) {
			bitset_info_log(&new_endnodesInUse, "new_endnodesInUse");
		}
	}
	
	newSwitchesInFabric = !bitset_equal(&old_switchesInUse, &new_switchesInUse);

    /* recalculate the path and cost arrays only if topology has changed or first time through */
    if (  topology_changed
       || topology_passcount == 0
       || newSwitchesInFabric
       || old_topology.num_sws != sm_newTopology.num_sws) {

		rebalance = 1;

		if (smDebugPerf) IB_LOG_INFINI_INFO0("Calculating topology paths");

		status = sm_topop->routingModule->funcs.allocate_cost_matrix(sm_topop);
		if (status != VSTATUS_OK) {
			IB_LOG_ERRORRC("failed to allocate cost data; rc:", status);
			IB_EXIT(__func__, status);
			return status;
		}
    
    	if (smDebugPerf) {
    		vs_time_get(&eTime);
			IB_LOG_INFINI_INFO("END topology_setup_routing_cost_matrix/setup init path array;"
								" elapsed time(usecs)=", (int)(eTime-sTime));
    		vs_time_get(&sTime);
    	}
 
		sm_topop->routingModule->funcs.initialize_cost_matrix(sm_topop);

    	if (smDebugPerf) {
    		vs_time_get(&eTime);
			IB_LOG_INFINI_INFO("END topology_setup_routing_cost_matrix/setup initial cost/path arrays;"
									" elapsed time(usecs)=", (int)(eTime-sTime));
    		vs_time_get(&sTime);
    	}
        
		sm_topop->routingModule->funcs.calculate_cost_matrix(sm_topop, sm_topop->max_sws, sm_topop->cost, sm_topop->path);

    	if (smDebugPerf) {
    		vs_time_get(&eTime);
			IB_LOG_INFINI_INFO("END topology_setup_routing_cost_matrix/calculation of cost and path arrays;"
									" elapsed time(usec)=", (int)(eTime-sTime));
        }

		status = sm_topop->routingModule->funcs.post_process_routing(sm_topop, &old_topology, &rebalance);
		if (status != VSTATUS_OK) {
			IB_LOG_ERRORRC("Failed to process 'post-routing' routing hook; rc:", status);
			return status;
		}
    
    } else if (old_topology.num_sws) {
        /* no topology change, just copy over the old data */
		status = sm_routing_copy_cost_matrix(&old_topology, sm_topop);
		if (status != VSTATUS_OK) {
			IB_LOG_ERRORRC("failed to copy cost data; rc:", status);
			IB_EXIT(__func__, status);
			return status;
		}

		status = sm_topop->routingModule->funcs.post_process_routing_copy(&old_topology, sm_topop, &rebalance);
		if (status != VSTATUS_OK) {
			IB_LOG_ERRORRC("Failed to process 'post-routing (copy)' routing hook; rc:", status);
			return status;
		}

		if (!rebalance) {
			status = sm_topop->routingModule->funcs.copy_routing(&old_topology, sm_topop);

			if (status != VSTATUS_OK) {
				IB_LOG_ERRORRC("failed to copy LFTs; rc:", status);
				IB_EXIT(__func__, status);
				return status;
			}

			if (smDebugPerf) {
				vs_time_get(&eTime);
				IB_LOG_INFINI_INFO("END topology_setup_routing_cost_matrix/copy over cost, path"
					", and lft arrays; elapsed time(usec)=",
					(int)(eTime-sTime));
			}
		}
    } else {
        // this is the odd case where HFM is the only thing in fabric
		// (it has it's port down)
        sm_topop->bytesCost = 0;
        sm_topop->bytesPath = 0;
        sm_topop->cost = NULL;
        sm_topop->path = NULL;
        if (!sm_newTopology.num_ports) {
            IB_LOG_WARN0("Host SM's port is down, re-starting sweep");
			sm_request_resweep(0, 0, SM_SWEEP_REASON_LOCAL_PORT_FAIL);
            return(VSTATUS_NOT_MASTER);
        } else {
            IB_LOG_INFINI_INFO0("Host SM's port is connected to another HFI port");
        }
    }

	if ((status == VSTATUS_OK) && !rebalance) 
		status = VSTATUS_KNOWN;

	IB_EXIT(__func__, status);
	return status;
}

Status_t
topology_adaptiverouting(void)
{
	Node_t		*nodep;
	Status_t	status;
    uint64_t    sTime, eTime;

	IB_ENTER(__func__, 0, 0, 0, 0);

//
//	If we aren't the master SM, then we go no further.
//
	if (sm_state != SM_STATE_MASTER) {
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

//
//	For every switch that supports adaptive routing, we need to do adaptive routing switch table setup.
//
	if (sm_adaptiveRouting.enable) {
    	if (smDebugPerf) {
        	vs_time_get(&sTime);
        	IB_LOG_INFINI_INFO0("START");
    	}

		for_all_switch_nodes(sm_topop, nodep) {
			if (nodep->arSupport) {
				if ((status=sm_AdaptiveRoutingSwitchUpdate(sm_topop, nodep)) != VSTATUS_OK) {
					if (topology_main_exit == 1) {
#ifdef __VXWORKS__
						ESM_LOG_ESMINFO("topology_adaptiverouting: SM has been stopped", 0);
#endif
						IB_EXIT(__func__, VSTATUS_OK);
						return(VSTATUS_OK);
					}
					continue;
					
					if (++topo_errors > sm_config.topo_errors_threshold &&
						++topo_abandon_count <= sm_config.topo_abandon_threshold) {
						return(status);
					}
				}
			}
		}
		if (smDebugPerf) {
			vs_time_get(&eTime);
			IB_LOG_INFINI_INFO("END AR setup; elapsed time(usecs)=", (int)(eTime-sTime));
		}
	}

	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

Status_t
topology_update_cableinfo(void) {
	Status_t status = VSTATUS_OK;
	Node_t* nodep;
	Port_t* portp;

	if (sm_config.cableInfoPolicy > CIP_NONE) {
		for_all_nodes(sm_topop, nodep) {
			for_all_ports(nodep, portp) {
				// CableInfo updates take a long time, so allow us to short circuit out of them for now if
 				// we have been told to resweep.
				if (topology_resweep) return status;
				if (sm_valid_port(portp) && !portp->portData->cableInfo &&
					portp->state == IB_PORT_ACTIVE && sm_Port_t_IsCableInfoSupported(portp)) {
	
					Port_t * neighPort = NULL;
					if (sm_config.cableInfoPolicy == CIP_LINK) {
						Node_t * neighNode = NULL;
						neighPort = sm_find_neighbor_node_and_port(&sm_newTopology, portp, &neighNode);
					}
	
					if (neighPort && neighPort->portData->cableInfo) {
						portp->portData->cableInfo = sm_CableInfo_copy(neighPort->portData->cableInfo);
					}
					else {
						status = sm_update_cableinfo(sm_topop, nodep, portp);
	
						if (status == VSTATUS_OK) {
						}
						else if (status == VSTATUS_NOSUPPORT) {
							IB_LOG_INFO_FMT(__func__,
								"CableInfo not supported for node %s nodeGuid "FMT_U64" port %d",
								sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, portp->index);
							sm_Port_t_SetCableInfoSupported(portp, FALSE);
						}
						else {
							IB_LOG_ERROR_FMT(__func__,
								"Failed to Get(CableInfo) for node %s nodeGuid "FMT_U64" port %d",
								sm_nodeDescString(nodep), nodep->nodeInfo.NodeGUID, portp->index);
						}
					}
				}
			}
		}
	}

	return status;
}

Status_t
topology_activate(void)
{
	Port_t		*portp;
	Node_t		*nodep;
	uint64_t    sTime, iTime, eTime;
	ActivationRetry_t retry = {0};

	IB_ENTER(__func__, 0, 0, 0, 0);

//
//	If we aren't the master SM, then we go no further.
//
	if (sm_state != SM_STATE_MASTER) {
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

	/* we are about to activate new nodes so mark us as sweeping now */
	activateInProgress = 1;
	topology_port_bounce_log_num = 0;

	if (smDebugPerf) {
		vs_time_get(&sTime);
		IB_LOG_INFINI_INFO_FMT(__func__, "START ARM ports");
	}

	// Transition ports from INIT to ARMED per DN0567
	for_all_ca_nodes(sm_topop, nodep)
		sm_arm_node(sm_topop, nodep);
	for_all_switch_nodes(sm_topop, nodep)
		sm_arm_node(sm_topop, nodep);

	if (smDebugPerf) {
		vs_time_get(&iTime);
		IB_LOG_INFINI_INFO_FMT(__func__, "END ARM ports; elapsed time(usecs)=%"PRIu64, iTime - sTime);
	}

	// sending of armed idle flits can be delayed by several millis, preventing
	// activation due to NeighborNormal not being set.  simply wait out the
	// expected propagation time to avoid spamming the log with failures due
	// to expected conditions, and to avoid complicating the code with edge
	// cases.  for sweeps proportional to this time, performance isn't critical,
	// and for significantly longer sweeps, this delay is a non-issue
	vs_thread_sleep(IDLE_FLIT_PROPAGATION_TIME);

	if (smDebugPerf) {
		// update activation phase start time to not include delay
		vs_time_get(&iTime);
		IB_LOG_INFINI_INFO_FMT(__func__, "START ACTIVATE ports");
	}

	// Transition ports from ARMED to ACTIVE per DN0567.
	// Retry with backoff in the presence of NeighborNormal-induced failures.
	do {
		retry.failures = 0;

		switch (sm_config.switchCascadeActivateEnable) {
			case SCAE_DISABLED:
				// failsafe activation: activate ports serially via Set(PortInfo)
				// only. activate HFIs first to allow traffic flow a bit sooner
				sm_activate_all_hfi_first_safe(sm_topop, &retry);
				break;
			case SCAE_SW_ONLY:
				// only switches will cascade, as HFI activation should be paced.
				// activate HFIs first to allow traffic flow a bit sooner and
				// switches will implicitly cascade as a result
				sm_activate_all_hfi_first(sm_topop, &retry);
				break;
			case SCAE_ALL:
				// all ports will cascade: do switches first for faster overall
				// cascade at scale, relying on activating switches to implicitly
				// activate hfis
				sm_activate_all_switch_first(sm_topop, &retry);
				break;
		}

		if (sm_debug && (retry.failures || retry.attempts))
			IB_LOG_WARN_FMT(__func__, "%u ports failed to go active on attempt %u", retry.failures, retry.attempts);

		if (retry.failures)
			vs_thread_sleep(MIN(VTIMER_10_MILLISEC * (1 << retry.attempts), VTIMER_1S));

	} while (retry.failures && ++retry.attempts <= sm_config.neighborNormalRetries);

	if (retry.failures) {
		IB_LOG_ERROR_FMT(__func__, "%d ports failed to go active due to NeighborNormal never being set", retry.failures);
	}

	if (smDebugPerf) {
		vs_time_get(&eTime);
		IB_LOG_INFINI_INFO_FMT(__func__, "END ACTIVATE ports; elapsed time(usecs)=%"PRIu64, eTime - iTime);
		IB_LOG_INFINI_INFO_FMT(__func__, "START CABLEINFO");
		iTime = eTime;
	}

	// And update CableInfo for the ports who are still alive
	topology_update_cableinfo();

	if (smDebugPerf) {
		vs_time_get(&eTime);
		IB_LOG_INFINI_INFO_FMT(__func__, "END CABLEINFO; elapsed time(usecs)=%"PRIu64, eTime - iTime);
		IB_LOG_INFINI_INFO_FMT(__func__, "END topology_activate; elapsed time(usecs)=%"PRIu64, eTime - sTime);
	}

	//Make sure LinkInitReason is set correctly for all quarantined nodes
	//that we are not activating
	QuarantinedNode_t *qnodep;
	int linkInitReason=0;
	for_all_quarantined_nodes(sm_topop, qnodep) {
		if((qnodep->quarantineReasons & STL_QUARANTINE_REASON_SMALL_MTU_SIZE) ||
			(qnodep->quarantineReasons & STL_QUARANTINE_REASON_VL_COUNT)) 
			linkInitReason = STL_LINKINIT_INSUFIC_CAPABILITY;
		else	
			linkInitReason = STL_LINKINIT_QUARANTINED;

		for_all_ports(qnodep->quarantinedNode, portp) {
			if(!sm_valid_port(portp)) {	
				if(portp && sm_dynamic_port_alloc()) {
					if ((portp->portData = sm_alloc_port(NULL, qnodep->quarantinedNode, portp->index, NULL)) == NULL) {
						IB_LOG_ERROR_FMT(__func__, "cannot create  port %d for quarantined node %s",
						 portp->index, sm_nodeDescString(qnodep->quarantinedNode));
						continue;
					}
				} else if (!portp) continue;
			}
			sm_set_linkinit_reason(qnodep->quarantinedNode, portp, linkInitReason);
			sm_enable_port_led(qnodep->quarantinedNode, portp, TRUE);
		}

		//set linkInit reason for connected authentic port
		if(sm_valid_port(qnodep->authenticNodePort)) {
			sm_set_linkinit_reason(qnodep->authenticNode, qnodep->authenticNodePort, linkInitReason);
			sm_enable_port_led(qnodep->authenticNode, qnodep->authenticNodePort, TRUE);
		}
	}

	// Print out a pre-defined topology verification summary message (if feature is enabled)
	if(sm_config.preDefTopo.enabled) {
		char buf[255];
		snprintf(buf, sizeof(buf), "SM: Pre-Defined Topology: Warning Counts: NodeDesc: %d, NodeGUID: %d, PortGUID: %d, Undefined Link: %d",
				sm_topop->preDefLogCounts.nodeDescWarn, sm_topop->preDefLogCounts.nodeGuidWarn,
				sm_topop->preDefLogCounts.portGuidWarn, sm_topop->preDefLogCounts.undefinedLinkWarn);
		vs_log_output_message(buf, FALSE);

		snprintf(buf, sizeof(buf), "SM: Pre-Defined Topology: Quarantine Counts: NodeDesc: %d, NodeGUID: %d, PortGUID: %d, Undefined Link: %d",
				sm_topop->preDefLogCounts.nodeDescQuarantined, 
				sm_topop->preDefLogCounts.nodeGuidQuarantined,
				sm_topop->preDefLogCounts.portGuidQuarantined, 
				sm_topop->preDefLogCounts.undefinedLinkQuarantined);
		vs_log_output_message(buf, FALSE);
	}

	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

Status_t
topology_multicast(void)
{
	Status_t	status=VSTATUS_OK;
    uint64_t    sTime, eTime;

	IB_ENTER(__func__, 0, 0, 0, 0);

    /*
     * If we aren't the master SM, then we go no further.
     */
	if (sm_state != SM_STATE_MASTER) {
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

    if (smDebugPerf) {
        vs_time_get(&sTime);
        IB_LOG_INFINI_INFO0("START setup of MFTs");
    }

    /*
     * If the topology didn't change, there is no need to recalculate the core spanning trees.
     * We always want to set it up the first time through when topology_passcount=1
     * PR 105510 - SM crash after reboot if a host had hung previous to reboot;
     * Temporarily hung hosts change the topology index of switch nodes requiring spanning tree 
     * to be recomputed when they go away and when they return
     */
#if 0
	//if (topology_changed || topology_passcount == 0 || old_topology.num_nodes != sm_newTopology.num_nodes) 
    {
        uint32_t i,j;
        //IB_LOG_INFINI_INFO0("Calculating the spanning trees");
        for (i = IB_MTU_256; i <= STL_MTU_MAX; i = getNextMTU(i)) {
            for (j = IB_STATIC_RATE_MIN; j <= IB_STATIC_RATE_MAX; j++) {
                sm_spanning_tree(i, j);
            }
        }
    }
#endif

	sm_topop->routingModule->funcs.build_spanning_trees();

    /*
     * Setup the MFTs of the switches.
    */
    if ((status = vs_lock(&sa_lock)) != VSTATUS_OK) {
        IB_LOG_ERRORRC("Failed to lock sa_lock rc:", status);
    } else {
        if (sm_McGroups_Need_Prog) {
            /* clear the need MFT reprogramming flag while holding sa_lock */
            sm_McGroups_Need_Prog = 0;
            smSendOutMFTs = 1;   /* internal topology flag */
        }
        (void)vs_unlock(&sa_lock);
    }

    if (new_endnodesInUse.nset_m || topology_changed || topology_switch_port_changes || (topology_passcount == 0) ||
		(sm_newTopology.maxMcastRate < old_topology.maxMcastRate) ||
		(sm_newTopology.maxMcastMtu < old_topology.maxMcastMtu)) {
        smSendOutMFTs = 1;   /* internal topology flag */
    }

    if (smSendOutMFTs) {
		status = sm_calculate_mfts();
    } else {
        /* just copy over the switch mfts to new topology */
        (void) sm_multicast_switch_mft_copy();
    }

    if (smDebugPerf) {
        vs_time_get(&eTime);
        IB_LOG_INFINI_INFO("END setup of MFTs; elapsed time(usecs)=", (int)(eTime-sTime));
    }

	IB_EXIT(__func__, status);
	return(status);
}

void
topology_saveLdr(Topology_t * topo, uint64_t guid, Port_t * port)
{
	LdrCacheEntry_t *t;
	int lastLdr = STL_LINKDOWN_REASON_LAST_INDEX(port->portData->LinkDownReasons);

	if (lastLdr < 0)
		return;

	if (!topo->ldrCache) {
		if (vs_pool_alloc(&sm_pool, sizeof(cl_qmap_t),
		  (void*)&topo->ldrCache) != VSTATUS_OK) {
			IB_LOG_ERROR_FMT(__func__, "Failed to allocate memory for ldrCache");
			return;
		}
		cl_qmap_init(topo->ldrCache, LdrCacheKey_cmp);
	}

	if (vs_pool_alloc(&sm_pool, sizeof(LdrCacheEntry_t), (void*)&t) != VSTATUS_OK) {
		IB_LOG_ERROR_FMT(__func__, "Failed to allocate memory for "
		  "LinkDownReasons cache entry for port.");
		return;
	}

	t->key.guid = guid;
	t->key.index = port->index;
	memcpy(&t->ldr, port->portData->LinkDownReasons,
		sizeof(STL_LINKDOWN_REASON) * STL_NUM_LINKDOWN_REASONS);
	cl_qmap_insert(topo->ldrCache, (long int)&t->key, &t->mapItem);
}

void
topology_log_isl_change(Topology_t *old_topo, Node_t *nodep, Port_t *portp, SmCsmMsgCondition_t condition, char *detail)
{
	Port_t *linkedToPortp = NULL;
	SmCsmNodeId_t csmNode, csmConnectedNode;

	if (portp->portData->pLid == 0xC000) {
		smCsmFormatNodeId(&csmNode, (uint8_t *)sm_nodeDescString(nodep), portp->index, nodep->nodeInfo.NodeGUID);

		if (  (linkedToPortp = sm_find_port(old_topo, portp->nodeno, portp->portno)) != NULL
			 && sm_valid_port(linkedToPortp)) {
			smCsmFormatNodeId(&csmConnectedNode, (uint8_t *)sm_nodeDescString(linkedToPortp->portData->nodePtr),
												linkedToPortp->index, (linkedToPortp->portData->nodePtr)->nodeInfo.NodeGUID);
			smCsmLogMessage(CSM_SEV_NOTICE, condition, &csmNode, &csmConnectedNode, detail);
		} else {
			smCsmLogMessage(CSM_SEV_NOTICE, condition, &csmNode, NULL, detail);
		}
	} else {
		if (  (linkedToPortp = sm_find_port(old_topo, portp->nodeno, portp->portno)) != NULL
			 && sm_valid_port(linkedToPortp)) {
			linkedToPortp->portData->pLid = 0xC000;
		}
	}
}

//
//	Find what changed between the two fabrics
//	Send GID In/out service traps
//
Status_t
topology_changes(Topology_t *old_topo, Topology_t *new_topo)
{
	Status_t	status		    = VSTATUS_OK;
	Port_t		*portp          = NULL;
	Port_t		*oldPortp	    = NULL;
	Node_t		*oldNodep	    = NULL;
	Port_t		*newPortp	    = NULL;
	Node_t		*newNodep	    = NULL;
	STL_NOTICE	notice;
	uint8_t     *oldChanges     = old_topo->pad;
	uint8_t		*newChanges     = new_topo->pad;
	const char detailAppearance[]   	= "inter-switch link appeared";
	const char detailDisappearance[]	= "inter-switch link disappeared";

	IB_ENTER(__func__, 0, 0, 0, 0);

	memset(&notice, 0, sizeof(notice));
	notice.Attributes.Generic.u.s.IsGeneric		= 1;
	notice.Attributes.Generic.u.s.Type	= NOTICE_TYPE_INFO;
	notice.Attributes.Generic.u.s.ProducerType		= NOTICE_PRODUCERTYPE_CLASSMANAGER;
	notice.IssuerLID	= sm_lid;
	notice.Stats.s.Toggle	= 0;
	notice.Stats.s.Count	= 0;

	if (!sm_valid_port((portp = sm_get_port(new_topo->node_head,sm_config.port)))) {
		IB_LOG_ERROR0("failed to get SM port");
		IB_EXIT(__func__, VSTATUS_BAD);
		return(VSTATUS_BAD);
	}

	memcpy(notice.IssuerGID.Raw, portp->portData->gid, sizeof(notice.IssuerGID.Raw));

	/*
     * Go through each old node and for each old node look for it in the new topology
	 * If you get a match see if states changed
     */
	memset(oldChanges, 0, old_topo->num_nodes * sizeof(uint8_t));
	memset(newChanges, 0, new_topo->num_nodes * sizeof(uint8_t));
	for_all_nodes(old_topo, oldNodep) {

		if ((newNodep = sm_find_guid(new_topo, oldNodep->nodeInfo.NodeGUID)) != NULL) {
			oldChanges[oldNodep->index] = newChanges[newNodep->index] = 1;
			for_all_end_ports2Nodes(oldNodep, oldPortp, newNodep, newPortp) {
				if (oldPortp->state <= IB_PORT_INIT) {
					if (sm_valid_port(newPortp) && newPortp->state >= IB_PORT_ARMED) {
						/*	New GID added, add to list of things to trap on */
						status = topology_TrapUp(&notice, new_topo, old_topo, newNodep, newPortp);
					}
				} else if (newPortp->state <= IB_PORT_INIT) {
					/* Old GID lost, add to list of things to trap on */
					if (sm_valid_port(oldPortp)) {
						status = topology_TrapDown(&notice, old_topo, new_topo, oldNodep, oldPortp);
						topology_saveLdr(new_topo, oldNodep->nodeInfo.NodeGUID, oldPortp);
					}
				}
			}

			/* Only if we aren't an HFI */
			if (oldNodep->nodeInfo.NodeType != NI_TYPE_CA) {
				/* Cycle through old port list, reporting downed ports in the new topo.
				 * We do this with some serious magic here. First, we detect ports on the
				 * 'oldNode' that are in the old topology and *not* the new topology. If we
				 * detect one of them, we follow the link in the old topology and set the pLid
				 * field of the neighboring node to 0xC000 (an invalid lid) to flag that we
				 * should report the link.
				 *
				 * When we encounter a port with the pLid set to 0xC000, we know that we should
				 * report the downed link. This ensures:
				 *   - That we only report the link once
				 *   - That both nodes that share the link are in both the new & old topology
				 *
				 * TODO:
				 *
				 * The use of the pLid field for this function is valid b/c it is currently unused
				 * *and* we blow away the old topo right after this function gets called. A cleaner
				 * way of doing this may be to used the unused 'flags' variable in the PortData_t
				 * structure. This cleanup should be deferred until we revamp the PortData_t struct
				 * to consume less memory.
				 */
				for_all_ports(oldNodep, oldPortp) {
					newPortp = sm_get_port(newNodep, oldPortp->index);

					if (sm_valid_port(oldPortp) && sm_valid_port(newPortp)) {
						// typically depending on frequency the port might be either intermittenly unstable or flapping
						if ((oldPortp->state > IB_PORT_INIT) && (newPortp->state <= IB_PORT_INIT)) {
							(void)topology_log_isl_change(old_topo, oldNodep, oldPortp, CSM_COND_DISAPPEARANCE, (char *)detailDisappearance);
						} else if ((newPortp->state > IB_PORT_INIT) && (oldPortp->state <= IB_PORT_INIT)) {
							(void)topology_log_isl_change(new_topo, newNodep, newPortp, CSM_COND_APPEARANCE, (char *)detailAppearance);
						}
					} else {
						// typically the port has either been DISABLED or ENABLED
						if (sm_valid_port(oldPortp) && (oldPortp->state > IB_PORT_INIT)) {
							(void)topology_log_isl_change(old_topo, oldNodep, oldPortp, CSM_COND_DISAPPEARANCE, (char *)detailDisappearance);
						} else if (sm_valid_port(newPortp) && (newPortp->state > IB_PORT_INIT)) {
							(void)topology_log_isl_change(new_topo, newNodep, newPortp, CSM_COND_APPEARANCE, (char *)detailAppearance);
						}
					}
				}
			}
		}

		if (!oldChanges[oldNodep->index]) {
			/* Never found old node, add all of its armed or active ports to GID Lost list */
			for_all_end_ports(oldNodep, oldPortp) {
				if (sm_valid_port(oldPortp) && oldPortp->state >= IB_PORT_ARMED) {
					/* port lost send trap */
					status = topology_TrapDown(&notice, old_topo, new_topo, oldNodep, oldPortp);
					topology_saveLdr(new_topo, oldNodep->nodeInfo.NodeGUID, oldPortp);
				}
			}
			oldChanges[oldNodep->index] = 1;
		}
	}	/* end for_all_nodes(&old_topology, oldNodep) */

	/*
	 *	Go through all the new topology nodes looking for nodes
	 *	that were not found in the previous run
     *  don't bother if first time through
     */
    if (old_topo->num_nodes > 0) {
        for_all_nodes(new_topo, newNodep) {
            if (!newChanges[newNodep->index]) {
                /* This is a new node, need to send trap for all ports up */
                for_all_end_ports(newNodep, newPortp) {
                    if (sm_valid_port(newPortp) && newPortp->state >= IB_PORT_ARMED) {
                        /*	New GID added, add to list of things to trap on */
                        status = topology_TrapUp(&notice, new_topo, old_topo, newNodep, newPortp);
                    }
                }
            }
        } /* end for_all_nodes(new_topo, newNodep) */
    }

	IB_EXIT(__func__, status);
	return status;
}

//
// Given two topologies, and a node that is present only in tp_present, figure out the port on
// the node that connected the node to the tp_missing topology. We do this by cycling through
// the nodes connected to the node in question, looking for the first one that still exists
// in both topologies.
//
Status_t
sm_find_missing_linked_ports(Topology_t * tp_present, Topology_t * tp_missing, Node_t * node,
                     Port_t ** _nodePortp, Port_t ** _neighborPortp)
{
	Status_t status = VSTATUS_NOT_FOUND;
	Port_t * portp = NULL, * neighborPortp = NULL;

	// Set to NULL
	*_nodePortp = *_neighborPortp = NULL;

	for_all_physical_ports(node, portp) {
		if (sm_valid_port(portp) && portp->state >= IB_PORT_ARMED)
		{
			neighborPortp = sm_find_port(sm_topop, portp->nodeno, portp->portno);
			
			// If neighbor exists in both topologies, we have a winner!
			if (  sm_valid_port(neighborPortp)
			   && sm_find_port(sm_topop, portp->nodeno, portp->portno) != NULL)
			{
				*_nodePortp = portp;
				*_neighborPortp = neighborPortp;
				status = VSTATUS_OK;
				break;
			}
		}
	} 

	return status;
}

Status_t
topology_TrapUp(STL_NOTICE * noticep, Topology_t * tp_present, Topology_t * tp_missing, Node_t * node, Port_t * port)
{
	STL_NOTICE * trap64 = (STL_NOTICE *)noticep;
	STL_TRAP_GID * trap64DataDetails = (STL_TRAP_GID *)noticep->Data;
	Status_t status;
	SmCsmNodeId_t nodeId, linkedId, * linkedIdPtr = NULL;
	Port_t * p1p, * p2p, * linkedToPortp = NULL;

	IB_ENTER(__func__, 0, 0, 0, 0);

	memcpy(trap64DataDetails->Gid.Raw, port->portData->gid, sizeof(trap64DataDetails->Gid.Raw));
	trap64->Attributes.Generic.TrapNumber = MAD_SMT_PORT_UP;

    /* queue up trap forwarding request to sm_async */
    if ((status = sm_sa_forward_trap(noticep)) != VSTATUS_OK){
        IB_LOG_ERRORRC("unable to queue trap to sm_async rc:", status);
    }
    /* mark topology as changed */
    topology_changed = 1; 

    /* no message on first sweep */
    if (topology_passcount) {
		if (  (sm_config.node_appearance_msg_thresh != 0)
		   && (++sweepNodeChangeMsgCount > sm_config.node_appearance_msg_thresh)) {
			nodeAppearanceSeverity = CSM_SEV_INFO;
			++sweepNodeAppearanceInfoMsgCount;
		}

		if (node->nodeInfo.NodeType == NI_TYPE_SWITCH) {
			if (sm_find_missing_linked_ports(tp_present, tp_missing, node, &p1p, &p2p) == VSTATUS_OK) {
				port = p1p;
				linkedToPortp = p2p;
			} else if (sm_find_missing_linked_ports(tp_present, tp_present, node, &p1p, &p2p) == VSTATUS_OK) {
				linkedToPortp = p2p;
			}
		} else if (  (linkedToPortp = sm_find_port(tp_present, port->nodeno, port->portno)) != NULL
		          && !sm_valid_port(linkedToPortp)) {
			linkedToPortp = NULL;
		}

		if (linkedToPortp != NULL) {
			smCsmFormatNodeId(&linkedId, (uint8_t *)sm_nodeDescString(linkedToPortp->portData->nodePtr),
			                  linkedToPortp->index, linkedToPortp->portData->nodePtr->nodeInfo.NodeGUID);
			linkedIdPtr = &linkedId;
		}

        if (node->nodeInfo.NodeType == NI_TYPE_SWITCH) {
			smCsmFormatNodeId(&nodeId, (uint8_t *)sm_nodeDescString(node), port->index, node->nodeInfo.NodeGUID);
			smCsmLogMessage(nodeAppearanceSeverity, CSM_COND_APPEARANCE, &nodeId, linkedIdPtr, "Node type: switch");
        } else {
			smCsmFormatNodeId(&nodeId, (uint8_t *)sm_nodeDescString(node), port->index, port->portData->guid);
			smCsmLogMessage(nodeAppearanceSeverity, CSM_COND_APPEARANCE, &nodeId, linkedIdPtr, "Node type: hfi");
        }
    }
	       
	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

Status_t
topology_TrapDown(STL_NOTICE * noticep, Topology_t * tp_present, Topology_t * tp_missing, Node_t * node, Port_t * port)
{
	McGroup_t * mcGroup;
	McGroup_t * prevMcGroup;
	McMember_t * mcMember; 
	Guid_t		mcastGid[2], portguid;
    SubscriberKeyp  subsKeyp;
	STL_INFORM_INFO_RECORD *iRecordp = NULL;
	STL_NOTICE * trap67 = (STL_NOTICE *)noticep;
	STL_NOTICE * trap65 = (STL_NOTICE *)noticep;
	STL_TRAP_GID * trapDataDetails = (STL_TRAP_GID *)noticep->Data;
	Status_t status;
    int32_t  moreEntries=1;
    CS_HashTableItr_t itr;
    uint32_t    groupChanges=0;
	SmCsmNodeId_t nodeId, linkedId, * linkedIdPtr = NULL;
	Port_t * extPortp, * p1p, * p2p, * linkedToPortp = NULL;
	
	IB_ENTER(__func__, 0, 0, 0, 0);

    /* mark topology as changed */
    topology_changed = 1;

	if (  (sm_config.node_appearance_msg_thresh != 0)
	   && (++sweepNodeChangeMsgCount > sm_config.node_appearance_msg_thresh)) {
		nodeAppearanceSeverity = CSM_SEV_INFO;
		++sweepNodeDisappearanceInfoMsgCount;
	}

	/* find linked-to port */
	extPortp = linkedToPortp = NULL;
	if (node->nodeInfo.NodeType == NI_TYPE_SWITCH) {
		if (sm_find_missing_linked_ports(tp_present, tp_missing, node, &p1p, &p2p) == VSTATUS_OK) {
			extPortp = p1p;
			linkedToPortp = p2p;
		} else if (sm_find_missing_linked_ports(tp_present, tp_present, node, &p1p, &p2p) == VSTATUS_OK) {
			linkedToPortp = p2p;
		}
	} else if (  (linkedToPortp = sm_find_port(tp_present, port->nodeno, port->portno)) != NULL
	          && !sm_valid_port(linkedToPortp)) {
		linkedToPortp = NULL;
	}

	if (linkedToPortp != NULL) {
		smCsmFormatNodeId(&linkedId, (uint8_t *)sm_nodeDescString(linkedToPortp->portData->nodePtr),
		                  linkedToPortp->index, linkedToPortp->portData->nodePtr->nodeInfo.NodeGUID);
		linkedIdPtr = &linkedId;
	}

    if (node->nodeInfo.NodeType == NI_TYPE_SWITCH) {
		smCsmFormatNodeId(&nodeId, (uint8_t *)sm_nodeDescString(node), extPortp ? extPortp->index : port->index, node->nodeInfo.NodeGUID);
		smCsmLogMessage(nodeAppearanceSeverity, CSM_COND_DISAPPEARANCE, &nodeId, linkedIdPtr, "Node type: switch");
    } else {
		smCsmFormatNodeId(&nodeId, (uint8_t *)sm_nodeDescString(node), port->index, port->portData->guid);
		smCsmLogMessage(nodeAppearanceSeverity, CSM_COND_DISAPPEARANCE, &nodeId, linkedIdPtr, "Node type: hfi");
    }
	
	// if applicable, clean it out of the list of disabled ports
	(void)sm_removedEntities_clearNode(node);

    /* remove down node from subscriber list if neccessary */
	memcpy(trapDataDetails->Gid.Raw, port->portData->gid, sizeof(trapDataDetails->Gid.Raw));
	trap65->Attributes.Generic.TrapNumber = MAD_SMT_PORT_DOWN;

    (void)vs_lock(&saSubscribers.subsLock);
    if (cs_hashtable_count(saSubscribers.subsMap) > 0) {
        cs_hashtable_iterator(saSubscribers.subsMap, &itr);
        do {
            subsKeyp = cs_hashtable_iterator_key(&itr);
            iRecordp = cs_hashtable_iterator_value(&itr);
			if (iRecordp->RID.SubscriberLID==port->portData->lid) {
                /* sync the delete with standby SMs */
                (void)sm_dbsync_syncInform(DBSYNC_TYPE_DELETE, subsKeyp, iRecordp);
                /* remove the entry from hashtable and free the value - key is freed by remove func */
                moreEntries = cs_hashtable_iterator_remove(&itr);
                free(iRecordp);
			} else {
                /* advance the iterator */
            	moreEntries = cs_hashtable_iterator_advance(&itr);
            }

        } while (moreEntries);
    }
    (void)vs_unlock(&saSubscribers.subsLock);

	/* If this node was providing any services, remove the service records */
	if (vs_lock(&saServiceRecords.serviceRecLock) == VSTATUS_OK)
	{
		OpaServiceRecord_t * pOsr = NULL;

		if (cs_hashtable_count(saServiceRecords.serviceRecMap) > 0)
		{
			cs_hashtable_iterator(saServiceRecords.serviceRecMap, &itr);
			do {
				pOsr = cs_hashtable_iterator_value(&itr);
				if (memcmp(&pOsr->serviceRecord.RID.ServiceGID, port->portData->gid, sizeof(IB_GID)) == 0)
				{

					IB_LOG_INFO_FMT(__func__, "Deleting service for GID " FMT_GID " that "
					                           "went out of service", STLGIDPRINTARGS2(port->portData->gid));
					moreEntries = cs_hashtable_iterator_remove(&itr);
                    /* sync the service record deletion to standby SMs if necessary */
                    (void)sm_dbsync_syncService(DBSYNC_TYPE_DELETE, pOsr);
                	/* remove the entry from hashtable and free the value - key is freed by remove func */
					free(pOsr);
				} else
				{
					moreEntries = cs_hashtable_iterator_advance(&itr);
				}
			} while (moreEntries);
		}

		vs_unlock(&saServiceRecords.serviceRecLock);
	}

	
    /* queue up port down trap forwarding request to sm_async */
    if ((status = sm_sa_forward_trap(noticep)) != VSTATUS_OK){
        IB_LOG_ERRORRC("unable to queue trap to sm_async rc:", status);
    }

	/* Remove this node from any multicast groups it was in */
	/* This may actually cause a group to go away and itself generate a trap */
	/* If a group has to be deleted we will use trap 67, setup the number here */
	trap67->Attributes.Generic.TrapNumber = MAD_SMT_MCAST_GRP_DELETED;
	prevMcGroup = NULL;
	(void)vs_lock(&sm_McGroups_lock);
	mcGroup = sm_McGroups;
	while (mcGroup) { /* Walk through all groups and ... */
		mcMember = sm_find_multicast_member(mcGroup, *((IB_GID*)port->portData->gid)); /* ...look for this gid as a member */
		if (!mcMember) { /* Didn't find it in this group, try the next one */
			prevMcGroup = mcGroup;
			mcGroup = mcGroup->next;
			continue;
		}
        /* indicate group table changed */
        groupChanges = 1;
        mcastGid[0] = mcGroup->mGid.AsReg64s.H;
        mcastGid[1] = mcGroup->mGid.AsReg64s.L;

		/* save the portguid before deleting the member */
		portguid = mcMember->portGuid;

		/* Found it, now we must remove it */
		if (!(mcMember->state & MCMEMBER_JOIN_FULL_MEMBER)) {
            IB_LOG_INFO_FMT(__func__, "Non full mcMember "FMT_U64" of multicast group "
                   "GID "FMT_GID" is no longer in fabric", portguid, mcastGid[0], mcastGid[1]);
		} else {
			mcGroup->members_full--;
			IB_LOG_INFO_FMT(__func__, "Full mcMember "FMT_U64" of multicast group "
				"GID "FMT_GID" is no longer in fabric", portguid, mcastGid[0], mcastGid[1]);
		}

		McMember_Delete(mcGroup, mcMember);
		if (mcGroup->members_full == 0) {
			IB_LOG_INFINI_INFO_FMT(__func__, "Last full member of multicast group "
				   "GID "FMT_GID" is no longer in fabric, deleting all members",
				   mcastGid[0], mcastGid[1]);

			while (mcGroup->mcMembers) {
				/* MUST copy head pointer into temp
				 * Passing mcGroup->members directly to the delete macro will corrupt the list
				 */
				mcMember = mcGroup->mcMembers;
				IB_LOG_INFO_FMT(__func__, "Deleting non full mcMember "FMT_U64,
					   mcMember->portGuid);
				McMember_Delete(mcGroup, mcMember);
			}
		} else {
            IB_LOG_INFO_FMT(__func__, "Full member "FMT_U64" of multicast group "
				   "GID "FMT_GID" is no longer in fabric",
				   portguid, mcastGid[0], mcastGid[1]);
        }

		if (mcGroup->mcMembers == NULL) { /* Group is empty, delete it */
			memcpy(trapDataDetails->Gid.Raw, mcGroup->mGid.Raw, sizeof(trapDataDetails->Gid.Raw));
			if (mcGroup == sm_McGroups) {
				McGroup_Delete(mcGroup);
				mcGroup = sm_McGroups;
			} else {
				McGroup_Delete(mcGroup);
				mcGroup = prevMcGroup->next;
			}

			status = sm_sa_forward_trap(noticep);
			if (status != VSTATUS_OK) {
				IB_LOG_ERRORRC("failed to queue up trap 67 (multicast group delete) to sm_async rc:", status);
			}
		} else {
			prevMcGroup = mcGroup;
			mcGroup = mcGroup->next;
		}
	} // end for_all_multicast_groups

	(void)vs_unlock(&sm_McGroups_lock);
    /* ask for a full sync of group table if there were changes */
    if (groupChanges) {
        /* sync the group change with standby SMs */
        (void)sm_dbsync_syncGroup(DBSYNC_TYPE_FULL, NULL);
    }

	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

Status_t
topology_copy(void)
{
	Node_t		*nodep, *oldnodep;
    Port_t      *portp, *oldportp;
    int         delta, nextLid, lid, i;
	uint64_t sTime, eTime;

	IB_ENTER(__func__, 0, 0, 0, 0);

	if (smDebugPerf) {
		vs_time_get(&sTime);
		IB_LOG_INFINI_INFO0("START");
	}
    /*
     *	Save the old topology and copy the new topology to it.  The new
     *	topology can't be bad since only this thread writes it.
     */
    (void)vs_wrlock(&old_topology_lock);
	(void)vs_lock(&saCache.lock);

    (void)memcpy((void *)&save_topology, (void *)&old_topology, sizeof(Topology_t));
    (void)memcpy((void *)&old_topology, (void *)&sm_newTopology, sizeof(Topology_t));


	bitset_copy(&old_switchesInUse, &new_switchesInUse);
	bitset_clear_all(&new_switchesInUse);
	bitset_clear_all(&new_endnodesInUse);

    /* clear out and rebuild the old topology node pointers in lidmap */
    for (i=0; i<= UNICAST_LID_MAX; i++) {
        lidmap[i].oldNodep = NULL;
    }
    /* point node entry pointer in lidmap table to newer old_topology */
    for_all_nodes(&sm_newTopology, nodep) {
		oldnodep = NULL;
		if (sm_mcDosThreshold &&
			nodep->nodeInfo.NodeType == NI_TYPE_CA &&
			nodep->oldExists) {
			oldnodep = nodep->old;
		}

		if ((save_topology.lastNDTrapTime > sm_newTopology.sweepStartTime) &&
			nodep->oldExists) {
			if (!oldnodep)
				oldnodep = nodep->old;
			if (oldnodep)
				nodep->nodeDescChgTrap = oldnodep->nodeDescChgTrap;
		}
			
        for_all_end_ports(nodep, portp) {
            if (!sm_valid_port(portp) || portp->state <= IB_PORT_DOWN) {
                continue;
            } else {
                delta = 1 << portp->portData->lmc;
                nextLid = portp->portData->lid + delta;
                for (lid = portp->portData->lid; lid < nextLid; ++lid) {
                    lidmap[lid].oldNodep = lidmap[lid].newNodep; /* point to new topology now */
                    lidmap[lid].newNodep = NULL;
					lidmap[lid].newPortp = NULL;
                }
				if (oldnodep) {
					/* copy over SA threshold counters from old topology */
					oldportp = sm_get_port(oldnodep, portp->index);
					if (sm_valid_port(oldportp)) {
						portp->portData->mcDeleteStartTime	= oldportp->portData->mcDeleteStartTime;
						portp->portData->mcDeleteCount 		= oldportp->portData->mcDeleteCount;
					}
				}
            }
        }
    }
    /* clear out all new topology node pointers in lidmap */
    for (i=0; i<= UNICAST_LID_MAX; i++) {
        lidmap[i].newNodep = NULL;
        lidmap[i].newPortp = NULL;
    }
	// TBD - can we trust topology_changed?  MFT programming doesn't
	// doesn't seem to get set on HFI going down nor up
	if (topology_changed) {
		++topology_changed_count;
		if (smDebugPerf) {
			IB_LOG_INFINI_INFO("topology_changed:", topology_changed_count);
		}
	}
	/* make the cached SA records active */
	(void)topology_cache_copy();
	(void)vs_unlock(&saCache.lock);

	/* Generate traps for topology changes */
	topology_changes(&save_topology, &old_topology);
	(void)vs_rwunlock(&old_topology_lock);

#ifndef __VXWORKS__
 	/* Not needed for ESM as we can call printLoopPaths on command line whenever required*/
 
 	/* print loop paths, uses old topology. So old topology has to be valid
 	   for printLoopPaths to work. For this to work for the first sweep too,
 	   we need to do this after new topology is copied to old topology */
 
 	if (esmLoopTestOn && smDebugPerf) {
 		printLoopPaths(-1, /* do not buffer */ 0);
 	}
#endif

	if (smDebugPerf) {
		vs_time_get(&eTime);
		IB_LOG_INFINI_INFO("END; elapsed time(usecs)=", (int)(eTime-sTime));
	}

	IB_EXIT(__func__, 0);
	return(VSTATUS_OK);
}

Status_t
topology_free_topology(Topology_t * topop)
{
	LoopPath_t  *loopPath;
	QuarantinedNode_t *qnodep;
	Node_t *nodep;

	if(topop->nodeIdMap){
		cl_qmap_remove_all(topop->nodeIdMap);
		(void)vs_pool_free(&sm_pool, (void *)topop->nodeIdMap);
		topop->nodeIdMap = NULL;
	}

	if(topop->nodeMap){
		cl_qmap_remove_all(topop->nodeMap);
		(void)vs_pool_free(&sm_pool, (void *)topop->nodeMap);
		topop->nodeMap = NULL;
	}

	if(topop->portMap){
		cl_qmap_remove_all(topop->portMap);
		(void)vs_pool_free(&sm_pool, (void *)topop->portMap);
		topop->portMap = NULL;
	}

	if(topop->nodeArray != NULL) {
		(void)vs_pool_free(&sm_pool, (void *)topop->nodeArray);
		topop->nodeArray = NULL;
	}

	if(topop->quarantinedNodeMap){
		cl_qmap_remove_all(topop->quarantinedNodeMap);
		(void)vs_pool_free(&sm_pool, (void *)topop->quarantinedNodeMap);
		topop->quarantinedNodeMap = NULL;
	}

	while ((qnodep = topop->quarantined_node_head) != NULL) {
		topop->quarantined_node_head = qnodep->next;
        Node_Quarantined_Delete(qnodep);
	}

	while ((nodep = topop->node_head) != NULL) {
		topop->node_head = nodep->next;
		Node_Delete(topop, nodep);
	}

	if (topop->cost != NULL) {
		(void)vs_pool_free(&sm_pool, (void *)topop->cost);
		topop->cost = NULL;
	}

	if (topop->smaChanges != NULL) {
		vs_pool_free(&sm_pool, topop->smaChanges);
		topop->smaChanges = NULL;
	}

	// release memory allocated for loop paths
	topop->numLoopPaths = 0;
	while ((loopPath = topop->loopPaths) != NULL) {
		topop->loopPaths = loopPath->next;
		(void)vs_pool_free(&sm_pool, (void *)loopPath);
	}
	
	if (topop->ldrCache) {
		cl_map_item_t *it;
		for (it = cl_qmap_head(topop->ldrCache);
			it != cl_qmap_end(topop->ldrCache);) {
			LdrCacheEntry_t *c = PARENT_STRUCT(it, LdrCacheEntry_t, mapItem);
			it = cl_qmap_next(it);
			vs_pool_free(&sm_pool, c);
		}
		vs_pool_free(&sm_pool, topop->ldrCache);
		topop->ldrCache = NULL;
	}

	return VSTATUS_OK;
}

Status_t
topology_release_saved_topology(void)
{
	Node_t		*nodep;
	Port_t    *portp = NULL;

	/*
	 * old_topology_lock is used here because CableInfo may exist in both
	 * save_topology and old_topology
	 */
	if (vs_wrlock(&old_topology_lock) == VSTATUS_OK) {

		for_all_nodes(&save_topology, nodep) {
			for_all_ports(nodep, portp) {
				if (!sm_valid_port(portp) || !portp->portData->cableInfo)
					continue;
				sm_CableInfo_free(portp->portData->cableInfo);
				portp->portData->cableInfo = NULL;
			}
		}

		vs_rwunlock(&old_topology_lock);
	}

	if (save_topology.routingModule != NULL)
		sm_routing_freeModule(&save_topology.routingModule);

	topology_free_topology(&save_topology);

	return VSTATUS_OK;
}

/*
 * Clear an unreliable new topology view.  This can be caused when 
 * a discovered switch is removed before topology_assigments is called.
 * The window of opportunity for hitting this grows with the size of the 
 * fabric.
 */
Status_t
topology_clearNew(void)
{
	Node_t		*nodep;
    Port_t      *portp;
    int         lid, nextLid, delta, i;

	IB_ENTER(__func__, 0, 0, 0, 0);

//
//	Clear the bad new topology.
//  Lock is already held by topology_main which calls us
//
    /* 
     * clear node entry pointer to new topology and reset lid topology pass count to 
     * previous good topology_count.  Just in case it was modified during invalid sweep. 
     */
    /* clear out new topology node pointers in lid map */
    for (i=0; i<= UNICAST_LID_MAX; i++) {
        lidmap[i].newNodep = NULL;
        lidmap[i].newPortp = NULL;
    }
    for_all_nodes(&old_topology, nodep) {
        for_all_end_ports(nodep, portp) {
            if (sm_valid_port(portp) && (portp->state > IB_PORT_DOWN && portp->portData->lid > 0 && portp->portData->lid <= UNICAST_LID_MAX)) {
                delta = 1 << portp->portData->lmc;
                nextLid = portp->portData->lid + delta;
                for (lid = portp->portData->lid; lid < nextLid; ++lid) {
                    lidmap[portp->portData->lid].pass = topology_passcount;
                }
            }
        }
    }

	/*
	 * old_topology_lock is used here because any CableInfo in
	 * sm_newTopology after a failed sweep came from old_topology,
	 * however, we should only consider the ports in sm_newTopology
	 * since these are the ones we're sure have had their
	 * cableInfo copied.
	 */
	if (vs_wrlock(&old_topology_lock) == VSTATUS_OK) {
		for_all_nodes(&sm_newTopology, nodep) {
			for_all_ports(nodep, portp) {
				if (!sm_valid_port(portp) || !portp->portData->cableInfo)
					continue;
				sm_CableInfo_free(portp->portData->cableInfo);
				portp->portData->cableInfo = NULL;
			}
		}

		vs_rwunlock(&old_topology_lock);
	}

	if (sm_newTopology.routingModule)
		sm_routing_freeModule(&sm_newTopology.routingModule);

	topology_free_topology(&sm_newTopology);

	IB_EXIT(__func__, 0);
	return(VSTATUS_OK);
}

// This is called from under the sm_newTopology lock and only modifies the
// "build" cache, which is the cached data for the new topology.
//
Status_t
topology_cache_build(void)
{
	Status_t  rc;
	int i;
	SACacheEntry_t *cache;
	
	IB_ENTER(__func__, 0, 0, 0, 0);
	
	// allocate/build new cache structures
	for (i = 0; i < SA_NUM_CACHES; i++) {
		rc = vs_pool_alloc(&sm_pool, sizeof(SACacheEntry_t), (void*)&cache);
		if (rc != VSTATUS_OK) {
			IB_LOG_WARNRC("failed to allocate memory for SA cache structure rc:", rc);
			saCache.build[i] = NULL;
		} else {
			memset(cache, 0, sizeof(SACacheEntry_t));
			rc = saCacheBuildFunctions[i](cache, &sm_newTopology);
			if (rc != VSTATUS_OK) {
				IB_LOG_WARN_FMT(__func__,
				       "failed to build cache at index %d, rc: %u", i, rc);
				(void)vs_pool_free(&sm_pool, cache);
				saCache.build[i] = NULL;
			} else {
				saCache.build[i] = cache;
			}
		}
	}
	
	rc = VSTATUS_OK;
	IB_EXIT(__func__, rc);
	return rc;
}


// This is called from under the old_topology lock and copies the built cache
// data into "current".
//
Status_t
topology_cache_copy(void)
{
	Status_t  rc;
	int i, count;
	
	IB_ENTER(__func__, 0, 0, 0, 0);
	
	// save existing caches on the previous list if they're in use.
	// otherwise, free them.  move caches for new topology into "current"
	for (i = 0, count = 0; i < SA_NUM_CACHES; i++) {
		if (saCache.current[i] != NULL) {
			if (saCache.current[i]->valid) {
				if (saCache.current[i]->refCount) {
					saCache.current[i]->next = saCache.previous;
					saCache.previous = saCache.current[i];
					count++;
				} else {
					if (saCache.current[i]->data)
						(void)vs_pool_free(&sm_pool, saCache.current[i]->data);
					(void)vs_pool_free(&sm_pool, saCache.current[i]);
				}
			} else {
				(void)vs_pool_free(&sm_pool, saCache.current[i]);
			}
		}
		saCache.current[i] = saCache.build[i];
		saCache.build[i] = NULL;
	}
	
	IB_LOG_INFO("in-use elements moved into SA cache history:", count);
	
	rc = VSTATUS_OK;
	IB_EXIT(__func__, rc);
	return rc;
}

Status_t topology_congestion(void)
{
	Node_t *nodep;
	uint64_t sTime, eTime;

	IB_ENTER(__func__, 0, 0, 0, 0);

	if (smDebugPerf) {
		vs_time_get(&sTime);
		IB_LOG_INFINI_INFO0("START");
	}

	for_all_nodes(sm_topop, nodep) {
		Status_t status;
		if (AtomicRead(&topology_triggered)) {
			IB_LOG_INFINI_INFO0("sweep triggered; aborting congestion configuration");
			status = VSTATUS_OK;
			break;
		}

		if (sm_config.congestion.enable) {
			Port_t *portp;
			uint32 dlid;
			portp = sm_get_node_end_port(nodep);
			if (!sm_valid_port(portp) || portp->state == IB_PORT_DOWN){
				continue;
			}
			dlid = portp->portData->lid;
			status = SM_Get_CongestionInfo_LR(fd_topology, 0, sm_lid, dlid, &nodep->congestionInfo);
			if (status != VSTATUS_OK) {
				IB_LOG_ERROR_FMT(__func__,
					"Failed to get Congestion Info for NodeGUID "FMT_U64" [%s]; rc: %d",
					nodep->nodeInfo.NodeGUID, sm_nodeDescString(nodep), status);
				continue;
			}
		}

		/* Need to call these functions even if disabled so we can clear CCA attributes */
		if (nodep->nodeInfo.NodeType == STL_NODE_SW)
			status = stl_sm_cca_configure_sw(nodep);
		else
			status = stl_sm_cca_configure_hfi(nodep);

		if (status != VSTATUS_OK)
			IB_LOG_INFO_FMT(__func__, "Failed to configure CCA for NodeGuid "FMT_U64" [%s]; rc: %d",
				nodep->nodeInfo.NodeGUID, sm_nodeDescString(nodep), status);
	}

	if (smDebugPerf) {
		vs_time_get(&eTime);
		IB_LOG_INFINI_INFO("END, elapsed time (usec) - ", (int)(eTime-sTime));
	}

	IB_EXIT(__func__, VSTATUS_OK);
	return VSTATUS_OK;
}


static int s_esmLoopTestOption;
static int s_esmLoopTestOptionSet;

int esmLoopTestOptionSet(int val) {
	s_esmLoopTestOption = val;
	s_esmLoopTestOptionSet = TRUE;
	return 0;
}

int esmLoopTestOptionGet(void) {
#if defined(__VXWORKS__)
	if (!s_esmLoopTestOptionSet) {
		s_esmLoopTestOption = bmGetValueAsInt("esmLoopTestOption", 0xb5);
		s_esmLoopTestOptionSet = TRUE;
		if (s_esmLoopTestOption == 0) {
			sysPrintf("Using historical loop test pattern\n");
		} else
			sysPrintf("Using loop test pattern=0x%02x\n", s_esmLoopTestOption & 0xff);
	}
#endif
	return s_esmLoopTestOption;
}

/* put the lop test message on the wire */
/*static*/ Status_t sendLoopMsg(int lid) {
	int	option = esmLoopTestOptionGet();
	Mai_t		out_mad;
	Status_t	status=VSTATUS_OK;
    uint64_t    tid;

	mai_alloc_tid(fd_loopTest, MAD_CV_SUBN_ADM, &tid);

    // send a mad packet out to the lid
    //IB_LOG_INFINI_INFOX("sending loop test packet out to LID ", lid);
    Mai_Init(&out_mad);
    AddrInfo_Init(&out_mad, sm_lid, lid, 0, getDefaultPKey(), MAI_GSI_QP, MAI_GSI_QP, GSI_WELLKNOWN_QKEY);
    LRMad_Init(&out_mad, MAD_CV_SUBN_ADM, MAD_CM_SEND, (tid), 0x777, 0x0, sm_config.mkey);
	if (option) {
		LRSmp_t *lrp = (LRSmp_t *)&out_mad.data;
		(void)memset((void *)lrp, (option & 0xff), sizeof(*lrp));
	} else {
		char pktData[] = "LOOP TEST DATA PACKET.....";
		LRData_Init(&out_mad, pktData, sizeof(pktData));
	}
	INCREMENT_COUNTER(smCounterSmPacketTransmits);
    status = mai_send(fd_loopTest, &out_mad);
    if (status != VSTATUS_OK) {
        IB_LOG_ERRORRC("can't send loop mad rc:", status);
    }
    return(status);
}

// loop test packet inject
Status_t topology_loopTest () {
    int         i,j;
    LoopPath_t  *loopPath;

	IB_ENTER(__func__, 0, 0, 0, 0);

    if (!esmLoopTestOn) return VSTATUS_OK;


	if (smDebugPerf) {
		IB_LOG_INFINI_INFO_FMT(__func__,
						   "FastMode=%d, FastMode MinISLRedundancy=%d, InjectEachSweep=%d, TotalPktsInjected since start=%d",
							esmLoopTestFast, esmLoopTestMinISLRedundancy, esmLoopTestInjectEachSweep,
							esmLoopTestTotalPktsInjected);	
	}

	if (!esmLoopTestForceInject &&
		!esmLoopTestInjectEachSweep && esmLoopTestTotalPktsInjected > 0)	return VSTATUS_OK;

//
//	Send loop messages.
//
    for (j=1; j<=esmLoopTestNumPkts; j++) {
        if (esmLoopTestInjectNode >= 0) {
            // a specific switch is desired
            for (loopPath=sm_topop->loopPaths; loopPath != NULL; loopPath = loopPath->next) {
                for (i=1; i<=loopPath->nodeIdx[0]; i++) {
                    if (esmLoopTestInjectNode == loopPath->nodeIdx[i]) {
                        sendLoopMsg(loopPath->lid);
                        break;
                    }
                }
            }
        } else {
            // do all switches loops
            for (i=loopPathLidEnd; i>=loopPathLidStart; i--) {
                // send a mad packet out to the lid
                sendLoopMsg(i);
            }
        }
    }
    printf("topology_loopTest: DONE\n");

	if (loopPathLidEnd >= loopPathLidStart) {
		esmLoopTestTotalPktsInjected += esmLoopTestNumPkts;
		IB_LOG_INFINI_INFO_FMT(__func__, "Injected %d packets. Total packets injected since start of loop test = %d",
								esmLoopTestNumPkts, esmLoopTestTotalPktsInjected);
	}

	if (esmLoopTestForceInject)
		esmLoopTestForceInject = 0;

	IB_LOG_INFINI_INFO0("END topology_loopTest");
	return(VSTATUS_OK);
}


/*                                                              
 * Dump topology to console                                                                
 */
static char* dump_topology(char* buf, int buffer) {
	int		i;
	Node_t		*nodep;
	Port_t		*portp;
	int len = 500;

	if (buffer && buf == NULL) {
		if (vs_pool_alloc(&sm_pool, len, (void*)&buf) != VSTATUS_OK) {
			IB_FATAL_ERROR("dump_topology: CAN'T ALLOCATE SPACE.");
			return NULL;
		}
		buf[0] = '\0';
	}

	/* Dump each node and active port */
    if (sm_state == SM_STATE_MASTER) {
	for_all_nodes(&old_topology, nodep) {
		char buf1[32], buf2[32], buf3[32], buf4[32];

		buf = snprintfcat(buf, &len, "-----------------------------------------------------------------------------------------\n");
		buf = snprintfcat(buf, &len, "%s\n", sm_nodeDescString(nodep));
		buf = snprintfcat(buf, &len, "-----------------------------------------------------------------------------------------\n");
		buf = snprintfcat(buf, &len, "Node[%3d] => "FMT_U64"  (%x)  ports=%d, path=",
				(int)nodep->index, nodep->nodeInfo.NodeGUID, nodep->nodeInfo.NodeType, (int)nodep->nodeInfo.NumPorts);

		for (i = 1; i <= (int)nodep->path[0]; i++) {
			buf = snprintfcat(buf, &len, "%2d ", nodep->path[i]);
		}

		buf = snprintfcat(buf, &len, "\n");

            buf = snprintfcat(buf, &len, "        Port ----  GUID  ----  (S) LID       LMC   _VL_   __MTU__  __WIDTH__ _____SPEED_______  CAP_MASK   N#  P#\n");
            for_all_ports(nodep, portp) {
                if (!sm_valid_port(portp) || portp->state <= IB_PORT_DOWN) {
                    continue;
                }

                if (nodep->nodeInfo.NodeType == NI_TYPE_SWITCH && portp->index > 0) {
                    buf = snprintfcat(buf, &len, "%12d "FMT_U64"   %d                 %2d/%2d %3s/%3s %6s/%3s %11s %3s %08x %4d  %2d ",
                        portp->index, portp->portData->guid, (int)portp->state,
                        portp->portData->vl0,portp->portData->vl1,
                        Decode_MTU(portp->portData->mtuSupported), Decode_MTU(portp->portData->maxVlMtu),
                        StlLinkWidthToText(portp->portData->portInfo.LinkWidth.Supported, buf1, sizeof(buf1)), StlLinkWidthToText(portp->portData->portInfo.LinkWidth.Active, buf2, sizeof(buf2)),
                        GetStr_SpeedSupport(&portp->portData->portInfo, buf3, sizeof(buf3)), 
						GetStr_SpeedActive(&portp->portData->portInfo, buf4, sizeof(buf4)),
                        (int)portp->portData->capmask, (int)portp->nodeno, (int)portp->portno);
                } else {
                    buf = snprintfcat(buf, &len, "%12d "FMT_U64"   %d  %08x  %04x  %2d/%2d %3s/%3s %6s/%3s %11s %3s %08x %4d  %2d ",
                        portp->index, portp->portData->guid, (int)portp->state, (int)portp->portData->lid, portp->portData->lmc,
                        portp->portData->vl0, portp->portData->vl1,
                        Decode_MTU(portp->portData->mtuSupported), Decode_MTU(portp->portData->maxVlMtu),
                        StlLinkWidthToText(portp->portData->portInfo.LinkWidth.Supported, buf1, sizeof(buf1)), StlLinkWidthToText(portp->portData->portInfo.LinkWidth.Active, buf2, sizeof(buf2)),
                        GetStr_SpeedSupport(&portp->portData->portInfo, buf3, sizeof(buf3)), 
						GetStr_SpeedActive(&portp->portData->portInfo, buf4, sizeof(buf4)),
                        (int)portp->portData->capmask, (int)portp->nodeno, (int)portp->portno);
                }

                if (portp->path[0] != 0xff) {
                    for (i = 1; i <= (int)portp->path[0]; i++) {
                        buf = snprintfcat(buf, &len, "%2d ", portp->path[i]);
                    }
                }
                buf = snprintfcat(buf, &len, "\n");
            }
            buf = snprintfcat(buf, &len, "\n");
        }
        buf = snprintfcat(buf, &len, "==================================================================================\n");

    } /* if state is Master */
	return buf;
}

Status_t
topology_dump(void)
{
	//int		i;
	//Node_t		*nodep;
	//Port_t		*portp;

	IB_ENTER(__func__, 0, 0, 0, 0);

//
//	If we are NOT debugging, then return.
//
	if (sm_debug == 0) {
		IB_EXIT(__func__, VSTATUS_OK);
		return(VSTATUS_OK);
	}

//
//	Only display if there are changes.
//
	if ((topology_changed == 0) || (topology_once < 0)) {
		IB_EXIT(__func__, VSTATUS_OK);
		return(VSTATUS_OK);
	}

	topology_once++;

	/* Dump the SM state, pass count, MAD count, priority, and MKey */
	printf("\nsm_state = %s   count = %d   LMC = %d, Topology Pass count = %d, Priority = %d, Mkey = "FMT_U64"\n\n", 
           sm_getStateText(sm_state), (int)sm_smInfo.ActCount, (int)sm_newTopology.lmc, (int)topology_passcount, sm_smInfo.u.s.Priority, sm_config.mkey);

    if (sm_state == SM_STATE_MASTER) {
		dump_topology(NULL, /* do not buffer */ 0);
		dump_cost_array((uint16_t *)sm_newTopology.cost);
		(void)fflush(stdout);
		showSmParms();
		(void)fflush(stdout);
		printSwitchLft(-1, 1, 1, /* do not buffer */ 0); /* Print all lfts from new topo and we already have the lock */
		(void)fflush(stdout);
	}

	IB_EXIT(__func__, VSTATUS_OK);
	return(VSTATUS_OK);
}

char*
sm_topology_dump(int buffer)
{
	char * buf = NULL;
	int len = 500;

	IB_ENTER(__func__, 0, 0, 0, 0);

	if (buffer) {
		if (vs_pool_alloc(&sm_pool, len, (void*)&buf) != VSTATUS_OK) {
			IB_FATAL_ERROR("sm_topology_dump: CAN'T ALLOCATE SPACE.");
			return NULL;
		}
		buf[0] = '\0';
	}

	if (topology_passcount < 1) {
        if (sm_state == 0xffffffff) {
            buf = snprintfcat(buf, &len, "Fabric Manager is not running at this time!\n");
        } else {
            buf = snprintfcat(buf, &len, "\nSM is currently in the %s state, count = %d, Priority = %d, Mkey = "FMT_U64"\n\n", 
                   sm_getStateText(sm_state), (int)sm_smInfo.ActCount, sm_smInfo.u.s.Priority, sm_config.mkey);
        }
		return buf;
	}

	(void)vs_rdlock(&old_topology_lock);

//
//	Dump the SM state and MAD count.
//
	snprintfcat(buf, &len, "\nsm_state = %s   count = %d   LMC = %d, Topology Pass count = %d, Priority = %d, Mkey = "FMT_U64"\n\n", 
           sm_getStateText(sm_state), (int)sm_smInfo.ActCount, (int)sm_newTopology.lmc, (int)topology_passcount, sm_smInfo.u.s.Priority, sm_config.mkey);

//
//	Dump each node and active port.
//
    if (sm_state == SM_STATE_MASTER) {
		buf = dump_topology(buf, buffer);
    } /* if state is Master */
	
	(void)vs_rwunlock(&old_topology_lock);

	IB_EXIT(__func__, VSTATUS_OK);
	return buf;
}


void
dump_cost_array(uint16_t *cost)
{
	int	ij;
	int	i, j;
    int num_nodes = sm_topop->max_sws;  

	IB_ENTER(__func__, 0, 0, 0, 0);

	if (cost) {
		printf("\nCOST:\n");
    	printf("     i  j");
    	for (j = 0; j < num_nodes; j++) {
        	printf("%6d", j);
    	}
    	printf("\n");
		for (i = 0; i < sm_topop->num_nodes; i++) {
        	printf("%6d   ", i);
			for (j = 0; j < sm_topop->num_nodes; j++) {
				ij = Index(i, j);
				printf("%6d", (int)cost[ij]);
			}
			printf("\n");
		}
	} else {
		printf("\nCOST MATRIX IS NULL.\n");
	}

	printf("\n************************************************\n\n");

	IB_EXIT(__func__, VSTATUS_OK);
	return;
}

void showSmParms() {
    if (sm_state > SM_STATE_NOTACTIVE) {
        printf("SM priority is set to %d\n", (int)sm_config.priority);
        printf("SM elevated priority is set to %d\n", (int)sm_config.elevated_priority);
        printf("SM LMC is set to %d\n", (int)sm_config.lmc);
        printf("SM E0 LMC is set to %d\n", (int)sm_config.lmc_e0);
        if (sm_config.lmc == 0 && sm_config.topo_lid_offset > 1) {
            printf("SM allocating LIDs in multiples of %d\n", (int)sm_config.topo_lid_offset);
        }
        printf("SM sweep rate is set to %d\n", (int)sm_getSweepRate());
        printf("SM max attempts on receive set to %d \n", (int)sm_config.max_retries);
        printf("SM max receive wait interval set to %d millisecs\n", (int)sm_config.rcv_wait_msec);
        printf("switchLifetime set to %d \n", (int)sm_config.switch_lifetime_n2);
        printf("HoqLife set to %d \n", (int)sm_config.hoqlife_n2);
        printf("VL Stall set to %d \n", (int)sm_config.vlstall);
        printf("packetLifetime constant is set to %d \n", (int)sm_config.sa_packet_lifetime_n2);
        if (sa_dynamicPlt[0] == 1) {
            printf("Dynamic PLT ON using values: 1 hop=%d, 2 hops=%d, 3 hops=%d, 4 hops=%d, 5 hops=%d, 6 hops=%d, 7 hops=%d, 8+hops=%d \n", 
                   sa_dynamicPlt[1],  sa_dynamicPlt[2],  sa_dynamicPlt[3],  sa_dynamicPlt[4],  sa_dynamicPlt[5],  
                   sa_dynamicPlt[6],  sa_dynamicPlt[7], sa_dynamicPlt[9]);
        } else {
            printf("Dynamic PLT is OFF, using constant value of %d \n", (int)sm_config.sa_packet_lifetime_n2);
        }
        printf("SM DBSync interval set to %d \n", (int)sm_config.db_sync_interval);
        printf("SM topology errors threshold set to %d, max retry to %d\n", (int)sm_config.topo_errors_threshold, (int)sm_config.topo_abandon_threshold);
    } else {
        printf("SM is not active on this node!\n");
    }
}

void setLidOffset(uint8_t offset) {
    if (offset > 0) {
        sm_config.topo_lid_offset = offset;
        printf("SM allocating LIDs in multiples of %d\n", (int)sm_config.topo_lid_offset);
    } else {
        printf("SM lid offset should be between 1 and 255, currently = %d\n", (int)sm_config.topo_lid_offset);
    }
}

void setRcvRetries(uint8_t retryCount) {
    if (retryCount > 0 && retryCount <= 10) {
        sm_config.max_retries = retryCount;
        printf("SM max attempts on receive set to %d\n", (int)sm_config.max_retries);
    } else {
        printf("SM max attempts on receive should be 1-10; Current value is %d\n", (int)sm_config.max_retries);
    }
}

void setRcvWait(uint32_t rcvwait) {
    if (rcvwait >= 32 && rcvwait <= 10000) {
        sm_config.rcv_wait_msec = rcvwait;
        printf("SM receive wait interval set to %d millisecs\n", (int)sm_config.rcv_wait_msec);
    } else {
        printf("SM receive wait interval should be between 32 msecs and 10000 msecs; Current value is %d millisecs\n", (int)sm_config.rcv_wait_msec);
    }
}

void setSwitchLifetime(uint8_t sl) {
    if (sl <= SM_MAX_SWITCH_LIFETIME) {
        sm_config.switch_lifetime_n2 = sl;
        sysPrintf(SYS_PINFO"switchLifetime set to %d; force an SM sweep to make changes go into effect \n", (int)sm_config.switch_lifetime_n2);
    } else {
        printf("switchLifetime should be %d-%d;  current value is %d \n",
		       SM_MIN_SWITCH_LIFETIME, SM_MAX_SWITCH_LIFETIME, (int)sm_config.switch_lifetime_n2);
    }
}

void setHoqLife(uint8_t hoq) {
    if (hoq <= SM_MAX_HOQ_LIFE) {
        sm_config.hoqlife_n2 = hoq;
        sysPrintf(SYS_PINFO"HoqLife set to %d; force an SM sweep to make changes go into effect \n", (int)sm_config.hoqlife_n2);
    } else {
        printf("HoqLife should be (%d-%d); current value is %d \n",
		       SM_MIN_HOQ_LIFE, SM_MAX_HOQ_LIFE, (int)sm_config.hoqlife_n2);
    }
}

void setVlStall(uint8_t stall) {
    if (stall >= SM_MIN_VL_STALL && stall <= SM_MAX_VL_STALL) {
        sm_config.vlstall = stall;
        sysPrintf(SYS_PINFO"VL Stall set to %d; force an SM sweep to make changes go into effect \n", (int)sm_config.vlstall);
    } else {
        printf("VL Stall should be %d-%d;  current value is %d \n",
		       SM_MIN_VL_STALL, SM_MAX_VL_STALL, (int)sm_config.vlstall);
    }
}

void 
topology_main_kill(void){
	topology_main_exit = 1;
}


// TBD - this code is a duplicate VxWorks specific copy of code in other HSM
// tools such as test/smi/sm/smpoolsize.c

char* printTopology(int buffer) {
    return sm_topology_dump(buffer);
}

#ifdef __VXWORKS__
int printMasterLMC() {
	sysPrintf("The master LMC is %d\n", (int)sm_config.lmc);
	return (int)sm_config.lmc;
}

int printSMLid() {

	sysPrintf("The SM LID is 0x%.4X\n", (int)sm_lid);

	return (int)sm_lid;
}

int printMasterSMLid() {
	STL_PORT_INFO  portInfo;
	uint8_t     path[64];
	Status_t	status;

	(void)memset((void *)path, 0, 64);
	if ((status = SM_Get_PortInfo(fd_sminfo, 1<<24, path, &portInfo)) != VSTATUS_OK) {
		sysPrintf("Unable to obtain Master SM LID - status=%d\n", (int)status);
	} else {
		sysPrintf("The Master SM LID is 0x%.4X\n", portInfo.MasterSMLID);
	}

	return (int)portInfo.MasterSMLID;
}

void dumpLidMap() {
	int i;
	int lastSet = 1;
    Port_t *portp=NULL;

    if (topology_passcount >= 1) {
		sysPrintf("----------------------------------------------------------------------------------\n");
        sysPrintf("SM is currently in the %s state, with Topology Pass count = %d\n", 
                  sm_getStateText(sm_state), (int)topology_passcount);
		sysPrintf("----------------------------------------------------------------------------------\n");
        (void)vs_rdlock(&old_topology_lock);
    	for (i = 1; i <= UNICAST_LID_MAX; ++i) {
    		if (lidmap[i].guid == 0) {
    			if (lastSet || i == UNICAST_LID_MAX) {
    				sysPrintf("Lid 0x%.4x: guid = 0x%.8x%.8x, pass = %u\n",
    						  i, (unsigned int)(lidmap[i].guid >> 32),(unsigned int)(lidmap[i].guid & 0xffffffffULL),
    						  (unsigned int)lidmap[i].pass);
    				lastSet = 0;
    			}
    		} else {
                if ((topology_passcount-lidmap[i].pass == 0) && (portp = sm_find_port_guid(&old_topology, lidmap[i].guid)) != NULL) {
                    sysPrintf("Lid 0x%.4x: guid = 0x%.8x%.8x, pass = %u, %s\n",
    						  i, (unsigned int)(lidmap[i].guid >> 32),(unsigned int)(lidmap[i].guid & 0xffffffffULL),
                              (unsigned int)lidmap[i].pass, sm_nodeDescString((Node_t *)(portp->portData->nodePtr)));
                } else {
                    sysPrintf("Lid 0x%.4x: guid = 0x%.8x%.8x, pass = %u\n",
    						  i, (unsigned int)(lidmap[i].guid >> 32),(unsigned int)(lidmap[i].guid & 0xffffffffULL),
                              (unsigned int)lidmap[i].pass);
                }
                lastSet = 1;
    		}
    	}
        (void)vs_rwunlock(&old_topology_lock);
    } else {
        if (sm_state == 0xffffffff) {
            sysPrintf("\nSM is not running at this time!\n\n");
        } else {
            sysPrintf("\nSM is currently in the %s state, count = %d\n\n", sm_getStateText(sm_state), (int)sm_smInfo.ActCount);
        }
    }
}

int printMaxLid() {

    if (topology_passcount >= 1) {
		sysPrintf("The maximum LID is 0x%.4X\n", (int)old_topology.maxLid);
	} else {
        if (sm_state == 0xffffffff) {
            sysPrintf("\nSM is not running at this time!\n\n");
        } else {
            sysPrintf("\nSM is currently in the %s state, count = %d\n\n", sm_getStateText(sm_state), (int)sm_smInfo.ActCount);
		}
	}
	return old_topology.maxLid;
}

int smCASize(void) {
	size_t size = sizeof(Node_t) + (3 * sizeof(Port_t)) + sizeof(PortData_t);
	sysPrintf("The size of one FI in the topology is %d bytes\n", size);
	return size;
}

int smSwitchSize(int numSwitches, int numPorts, int numActivePorts, int numSwitches2, int numFIs, int lmc, int mlidTableCap) {
	size_t size;
    int    sizelft;

		//@todo: probably not the right size since we round-up now instead of doing '+ 64' for LFT allocations
    sizelft = (numSwitches + numSwitches2 + numFIs) * (1 << lmc) + 64;
	//sysPrintf("The size of a Switch LFT table is %d bytes\n", sizelft);
    size = sizeof(Node_t);
    size += ((numPorts + 1) * (sizeof(Port_t))) + ((numActivePorts+1) * sizeof(PortData_t));
    //size += ((numPorts2 + 1) * (sizeof(Port_t))) + ((numActivePorts2+1) * sizeof(PortData_t))	
    size += 16 + sizelft + sizeof(uint16_t * ) * mlidTableCap + sizeof(uint16_t) * mlidTableCap * 16;
	sysPrintf("The average size of one Switch in the topology is %d bytes\n", size);
	return size;
}

int smPortSize(void) {
	size_t size = sizeof(Port_t) + sizeof(PortData_t);
	sysPrintf("The size of one port in the topology is %d bytes\n", size);
	return size;
}

int smLidmapSize(void) {
	size_t size = sizeof(LidMap_t) * SIZE_LFT;
	sysPrintf("The size LidMap in the topology is %d bytes\n", size);
	return size;
}

void smFabricSizeUsage(void) {
	fprintf(stderr, "smFabricSize #FIs #SwType1 #PortsSwType1 #SwType2 #PortsSwType2 #ActivePortsSwType2 -l lmc\n");
	fprintf(stderr, "           must enter #FIs, #SwType1, and #PortsSwType1 at minimum\n");
	fprintf(stderr, "           all other entries default to zero if not entered\n");
	return;
}

uint64_t smFabricSize(int numFIs, int numSwitches, int numSwitchPorts,
                      int numSwitches2, int numSwitchPorts2, int numSwitchActivePorts2, int nsp2, int lmc) {
    size_t numTopologies = 2;
    size_t numMcGroups = 4;
    size_t numCaSubscriptions = 4;
    size_t numManagers = 4; // PM, BM, FE, PSM
    size_t numSmSubs = 7; // PM, BM, FE all do GID IN/OUT, FE does PortState
    size_t numStandbys = 2;
    size_t numHostServices = 2;

    if (!numFIs || !numSwitches || !numSwitchPorts) {
        smFabricSizeUsage();
        return 0;
    }
		// probably not the right size since we do 'ROUNDUP' instead of '+ 64'; also should change to size_t to guard against overflow
    int      sizelft = (numSwitches + numSwitches2 + numFIs) * (1 << lmc) + 64;
	sysPrintf("The size of a Switch LFT table is %d bytes\n", sizelft);
    uint64_t caSize = smCASize() * numTopologies;
    uint64_t switchSize = smSwitchSize(numSwitches, numSwitchPorts, numSwitchPorts, numSwitches2, numFIs, lmc, DEFAULT_SW_MLID_TABLE_CAP) * numTopologies;
    uint64_t switchSize2 = smSwitchSize(numSwitches2, numSwitchPorts2, numSwitchActivePorts2, numSwitches, numFIs, lmc, DEFAULT_SW_MLID_TABLE_CAP) * numTopologies;
    uint64_t subscriberSize = saSubscriberSize();
    uint64_t serviceRecordSize = saServiceRecordSize();
    uint64_t groupSize = saMcGroupSize();
    uint64_t memberSize = saMcMemberSize();
    uint64_t pathArraySize = sizeof(uint16_t) * numTopologies;
    uint64_t costArraySize = sizeof(uint16_t) * numTopologies;
    uint64_t maxResponseSize = saMaxResponseSize();
    uint64_t nodeRecordSize = saNodeRecordSize();
    uint64_t lidMapSize = smLidmapSize();
    uint64_t smSize = numSmSubs * subscriberSize + numManagers * serviceRecordSize;
    uint64_t standbySize = smSize * numStandbys;
    uint64_t size = 0;

    caSize += numHostServices * serviceRecordSize + numCaSubscriptions * subscriberSize + numMcGroups * memberSize;
    caSize *= numFIs;
 
    switchSize *= numSwitches;
    switchSize2 *= numSwitches2;
    /* now for total switch size */
    switchSize += switchSize2;

    groupSize *= numMcGroups;
    
    costArraySize *= (numSwitches+numSwitches2) * (numSwitches+numSwitches2);
    pathArraySize *= (numSwitches+numSwitches2) * (numSwitches+numSwitches2);

    /* lets assume max 4 sweeps worth of SA noderecord caching */
    nodeRecordSize *= (numFIs+numSwitches+numSwitches2) * 4;

    size = caSize + switchSize + smSize + standbySize + groupSize +
           costArraySize + pathArraySize + maxResponseSize +
           nodeRecordSize + lidMapSize;

    // add in 10% fudge factor
    size += size/10;

    sysPrintf("The total for FIs in the fabric is %"CS64"d bytes\n", caSize);
    sysPrintf("The total for switches in the fabric is %"CS64"d bytes\n", switchSize);
    sysPrintf("The total for the master SM is %"CS64"d bytes\n", smSize);
    sysPrintf("The total for %d standby SMs is %"CS64"d bytes\n", numStandbys, standbySize);
    sysPrintf("The total for %d multicast groups is %"CS64"d bytes\n", numMcGroups, groupSize);
    sysPrintf("The total for the cost array is %"CS64"d bytes\n", costArraySize);
    sysPrintf("The total for the path array is %"CS64"d bytes\n", pathArraySize);
    sysPrintf("The total for the LidMap is %"CS64"d bytes\n", lidMapSize);
    sysPrintf("The total for SA max response is %"CS64"d bytes\n", maxResponseSize);
    sysPrintf("The total for sending all node records to 1/4 the nodes at once is %"CS64"d bytes\n", nodeRecordSize);

	sysPrintf("The topology size of a fabric with %d FIs and %d %d port switches and %d %d port (%d active) switches is %"CS64"d bytes\n",
			  numFIs, numSwitches, numSwitchPorts, numSwitches2, numSwitchPorts2, numSwitchActivePorts2, size);
	return size;	
}

#endif


void printDgVfMemberships(void) {

#if 0
	(void)vs_rdlock(&old_topology_lock);
	VirtualFabrics_t *VirtualFabrics = old_topology.vfs_ptr;

	//Print DG and VF memberships for every port on all nodes
	int idx;
	int dgIdx;
	int numPorts;
	Node_t *nodep;
	Port_t* portPtr;
	DGConfig_t*dgp;

	IB_LOG_INFINI_INFO_FMT(__func__, "Device Group Info:");

	for (dgIdx=0; dgIdx<dg_config.number_of_dgs; ++dgIdx) {
		dgp = dg_config.dg[dgIdx];
		if (dgp->name != NULL)
			IB_LOG_INFINI_INFO_FMT(__func__, "     DG: %s   Idx: %d", dgp->name, dgIdx);
	}

	//Print all defined VFs
	int vfIdx = 0;
	int vf;
	if (VirtualFabrics)  {

		VF_t* vfp;

		IB_LOG_INFINI_INFO_FMT("topology_main", "Number of Active VFs: %d", VirtualFabrics->number_of_vfs);
		for (vf=0; vf < VirtualFabrics->number_of_vfs; vf++) {
			vfp = &VirtualFabrics->v_fabric[vf];
			IB_LOG_INFINI_INFO_FMT("topology_main", "     VF: %s   Idx: %d", vfp->name, vfp->index);
		}

		int numStandbyVfs = VirtualFabrics->number_of_vfs_all - VirtualFabrics->number_of_vfs;

		IB_LOG_INFINI_INFO_FMT("topology_main", "Number of Standby VFs: %d", numStandbyVfs);
		for (vf=0; vf < VirtualFabrics->number_of_vfs_all; vf++) {
			if (VirtualFabrics->v_fabric_all[vf].standby) {
				vfp = &VirtualFabrics->v_fabric_all[vf];
				IB_LOG_INFINI_INFO_FMT("topology_main", "     VF: %s   Idx: %d", vfp->name, vfp->index);
			}
		}
	}

	char dbgBuf[500];
	int len = 500;

	for (idx = 0, nodep = sm_topop->node_head; nodep != NULL; ++idx, nodep = nodep->next) {
		numPorts = nodep->nodeInfo.NumPorts;

		int numDgMemberships = 0;
		int numVfMemberships = 0;

		for_all_ports(nodep,portPtr) {
			if (!sm_valid_port(portPtr) || portPtr->state <= IB_PORT_DOWN)
				continue;
			else {

				snprintf(dbgBuf, len, "%s:%d  DG: ", nodep->nodeDesc.NodeString, portPtr->index);

				//loop and print all dg indexes for this port
				numDgMemberships = 0;
				numVfMemberships = 0;

				int printedFirst = 0;
				for (dgIdx=0; dgIdx<MAX_DEVGROUPS; ++dgIdx) {
					int index = portPtr->portData->dgMemberList[dgIdx];
					if (index != 65535) {
						numDgMemberships++;
						if (printedFirst) {
							snprintfcat(dbgBuf, &len, ", %d", index);
						}
						else {
							snprintfcat(dbgBuf, &len, "%d", index);
							printedFirst = 1;
						}
					}
				}
				snprintfcat(dbgBuf, &len, "  VF: ");

				printedFirst = 0;
				for (vfIdx=0; vfIdx<MAX_VFABRICS; ++vfIdx) {
					if (bitset_test(&portPtr->portData->vfMember, vfIdx)) {
						numVfMemberships++;

						if (printedFirst) {
							snprintfcat(dbgBuf, &len, ", %d", vfIdx);
						}
						else {
							snprintfcat(dbgBuf, &len, "%d", vfIdx);
							printedFirst = 1;
						}								
					}
				}

				if ((numDgMemberships == 0) && (numVfMemberships == 0) )
					snprintfcat(dbgBuf, &len, " -- No DG or VF Memberships");

				IB_LOG_INFINI_INFO_FMT(__func__, "%s", dbgBuf);
			}
		}
	}
	(void)vs_rwunlock(&old_topology_lock);
#endif

}
