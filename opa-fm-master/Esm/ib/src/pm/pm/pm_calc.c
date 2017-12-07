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

// low level computational functions
// except where noted, caller of these routines must hold imageLock for write
// for image being computed

#include "pm_topology.h"
#include <limits.h>
#include <iba/stl_helper.h>

extern Pm_t g_pmSweepData;
extern CounterSelectMask_t LinkDownIgnoreMask;

static void ClearUtilStats(PmUtilStats_t *utilp)
{
	utilp->TotMBps = 0;
	utilp->TotKPps = 0;

	utilp->MaxMBps = 0;
	utilp->MinMBps = IB_UINT32_MAX;

	utilp->MaxKPps = 0;
	utilp->MinKPps = IB_UINT32_MAX;
	MemoryClear(&utilp->BwPorts[0], sizeof(utilp->BwPorts[0]) * PM_UTIL_BUCKETS);
}

static void ClearErrStats(PmErrStats_t *errp)
{
	MemoryClear(errp, sizeof(*errp));
}

// clear stats for a group
void ClearGroupStats(PmGroupImage_t *groupImage)
{
	ClearUtilStats(&groupImage->IntUtil);
	ClearUtilStats(&groupImage->SendUtil);
	ClearUtilStats(&groupImage->RecvUtil);
	ClearErrStats(&groupImage->IntErr);
	ClearErrStats(&groupImage->ExtErr);
	groupImage->MinIntRate = IB_STATIC_RATE_MAX;
	groupImage->MaxIntRate = IB_STATIC_RATE_14G; // = 12.5g for STL;
	groupImage->MinExtRate = IB_STATIC_RATE_MAX;
	groupImage->MaxExtRate = IB_STATIC_RATE_14G; // = 12.5g for STL;
}

// clear stats for a VF
void ClearVFStats(PmVFImage_t *vfImage)
{
	ClearUtilStats(&vfImage->IntUtil);
	ClearErrStats(&vfImage->IntErr);
	vfImage->MinIntRate = IB_STATIC_RATE_MAX;
	vfImage->MaxIntRate = IB_STATIC_RATE_14G; // = 12.5g for STL;
}

// The Update routines take a portSweep argument to reduce overhead
// by letting caller index the PortSweepData once and pass pointer
static void UpdateUtilStats(PmUtilStats_t *utilp, PmPortImage_t *portImage)
{
	utilp->TotMBps += portImage->SendMBps;
	utilp->TotKPps += portImage->SendKPps;

	utilp->BwPorts[portImage->UtilBucket]++;

	UPDATE_MAX(utilp->MaxMBps, portImage->SendMBps);
	UPDATE_MIN(utilp->MinMBps, portImage->SendMBps);

	UPDATE_MAX(utilp->MaxKPps, portImage->SendKPps);
	UPDATE_MIN(utilp->MinKPps, portImage->SendKPps);
}

// called when both ports in a link are in the given group
static void UpdateErrStatsInGroup(PmErrStats_t *errp, PmPortImage_t *portImage)
{
#define UPDATE_MAX_ERROR(stat) do { \
		UPDATE_MAX(errp->Max.stat, portImage->Errors.stat); \
	} while (0)
#define UPDATE_ERROR(stat) do { \
		UPDATE_MAX(errp->Max.stat, portImage->Errors.stat); \
		errp->Ports[portImage->stat##Bucket].stat++; \
	} while (0)
	UPDATE_ERROR(Integrity);
	UPDATE_ERROR(Congestion);
	UPDATE_ERROR(SmaCongestion);
	UPDATE_ERROR(Bubble);
	UPDATE_ERROR(Security);
	UPDATE_ERROR(Routing);
	UPDATE_MAX_ERROR(DiscardsPct10);
	UPDATE_MAX_ERROR(UtilizationPct10);
#undef UPDATE_ERROR
#undef UPDATE_MAX_ERROR
}

// called when only one of ports in a link are in the given group
static void UpdateErrStatsExtGroup(PmErrStats_t *errp,
			   	PmPortImage_t *portImage, PmPortImage_t *portImage2)
{
	// for between-group counters we use the worst of the 2 ports
#define UPDATE_MAX_ERROR(stat) do { \
		UPDATE_MAX(errp->Max.stat, portImage->Errors.stat); \
		UPDATE_MAX(errp->Max.stat, portImage2->Errors.stat); \
	} while (0)
#define UPDATE_ERROR(stat) do { \
		UPDATE_MAX(errp->Max.stat, portImage->Errors.stat); \
		UPDATE_MAX(errp->Max.stat, portImage2->Errors.stat); \
		errp->Ports[MAX(portImage->stat##Bucket, portImage2->stat##Bucket)].stat++; \
	} while (0)
	UPDATE_ERROR(Integrity);
	UPDATE_ERROR(Congestion);
	UPDATE_ERROR(SmaCongestion);
	UPDATE_ERROR(Bubble);
	UPDATE_ERROR(Security);
	UPDATE_ERROR(Routing);
	UPDATE_MAX_ERROR(DiscardsPct10);
	UPDATE_MAX_ERROR(UtilizationPct10);
#undef UPDATE_ERROR
#undef UPDATE_MAX_ERROR
}

uint32_t PmCalculateRate(uint32_t speed, uint32_t width) {
	switch (speed) {
		case STL_LINK_SPEED_12_5G:
			switch (width) {
			case STL_LINK_WIDTH_1X: return IB_STATIC_RATE_14G; // STL_STATIC_RATE_12_5G;
			case STL_LINK_WIDTH_2X: return IB_STATIC_RATE_25G;
			case STL_LINK_WIDTH_3X: return IB_STATIC_RATE_40G; // STL_STATIC_RATE_37_5G;
			case STL_LINK_WIDTH_4X: return IB_STATIC_RATE_56G; // STL_STATIC_RATE_50G
			default: return IB_STATIC_RATE_1GB; // Invalid!
		}
		case STL_LINK_SPEED_25G:
			switch (width) {
			case STL_LINK_WIDTH_1X: return IB_STATIC_RATE_25G;
			case STL_LINK_WIDTH_2X: return IB_STATIC_RATE_56G; // STL_STATIC_RATE_50G;
			case STL_LINK_WIDTH_3X: return IB_STATIC_RATE_80G; // STL_STATIC_RATE_75G;
			case STL_LINK_WIDTH_4X: return IB_STATIC_RATE_100G;
			default: return IB_STATIC_RATE_1GB; // Invalid!
		}
		default:return IB_STATIC_RATE_1GB; // Invalid!
	}
}

// update stats for a port whose neighbor is also in the group
void UpdateInGroupStats(Pm_t *pm, PmGroupImage_t *groupImage,
				PmPortImage_t *portImage)
{
	UpdateUtilStats(&groupImage->IntUtil, portImage);
	UpdateErrStatsInGroup(&groupImage->IntErr, portImage);
	uint32 rxRate = PmCalculateRate(portImage->u.s.activeSpeed, portImage->u.s.rxActiveWidth);
	if (s_StaticRateToMBps[rxRate] >= s_StaticRateToMBps[groupImage->MaxIntRate])
		groupImage->MaxIntRate = rxRate;
	if (s_StaticRateToMBps[rxRate] <= s_StaticRateToMBps[groupImage->MinIntRate])
		groupImage->MinIntRate = rxRate;
}

// update stats for a port whose neighbor is not also in the group
void UpdateExtGroupStats(Pm_t *pm, PmGroupImage_t *groupImage,
			   	PmPortImage_t *portImage, PmPortImage_t *portImage2)
{
	ASSERT(portImage2); // only ports with neighbors could be Ext
	// our stats are what portp Sent
	UpdateUtilStats(&groupImage->SendUtil, portImage);
	// neighbor's stats are what it sent (and portp received)
	UpdateUtilStats(&groupImage->RecvUtil, portImage2);
	UpdateErrStatsExtGroup(&groupImage->ExtErr, portImage, portImage2);
	uint32 rxRate = PmCalculateRate(portImage->u.s.activeSpeed, portImage->u.s.rxActiveWidth);
	if (s_StaticRateToMBps[rxRate] >= s_StaticRateToMBps[groupImage->MaxExtRate])
		groupImage->MaxExtRate = rxRate;
	if (s_StaticRateToMBps[rxRate] <= s_StaticRateToMBps[groupImage->MinExtRate])
		groupImage->MinExtRate = rxRate;
}

// update stats for a port whose neighbor is also in the group
void UpdateVFStats(Pm_t *pm, PmVFImage_t *vfImage,
				PmPortImage_t *portImage)
{
	UpdateUtilStats(&vfImage->IntUtil, portImage);
	UpdateErrStatsInGroup(&vfImage->IntErr, portImage);
	uint32 rxRate = PmCalculateRate(portImage->u.s.activeSpeed, portImage->u.s.rxActiveWidth);
	if (s_StaticRateToMBps[rxRate] >= s_StaticRateToMBps[vfImage->MaxIntRate])
		vfImage->MaxIntRate = rxRate;
	if (s_StaticRateToMBps[rxRate] <= s_StaticRateToMBps[vfImage->MinIntRate])
		vfImage->MinIntRate = rxRate;
}

// compute averages
void FinalizeGroupStats(PmGroupImage_t *groupImage)
{
	if (groupImage->NumIntPorts) {
		groupImage->IntUtil.AvgMBps = groupImage->IntUtil.TotMBps/groupImage->NumIntPorts;
		groupImage->IntUtil.AvgKPps = groupImage->IntUtil.TotKPps/groupImage->NumIntPorts;
	} else {
		// avoid any possible confusion, remove UINT_MAX value
		groupImage->IntUtil.MinMBps = 0;
		groupImage->IntUtil.MinKPps = 0;
		groupImage->MinIntRate = IB_STATIC_RATE_DONTCARE;
		groupImage->MaxIntRate = IB_STATIC_RATE_DONTCARE;
	}
	if (groupImage->NumExtPorts) {
		groupImage->SendUtil.AvgMBps = groupImage->SendUtil.TotMBps/groupImage->NumExtPorts;
		groupImage->SendUtil.AvgKPps = groupImage->SendUtil.TotKPps/groupImage->NumExtPorts;
		groupImage->RecvUtil.AvgMBps = groupImage->RecvUtil.TotMBps/groupImage->NumExtPorts;
		groupImage->RecvUtil.AvgKPps = groupImage->RecvUtil.TotKPps/groupImage->NumExtPorts;
	} else {
		// avoid any possible confusion, remove UINT_MAX value
		groupImage->SendUtil.MinMBps = 0;
		groupImage->SendUtil.MinKPps = 0;
		groupImage->RecvUtil.MinMBps = 0;
		groupImage->RecvUtil.MinKPps = 0;
		groupImage->MinExtRate = IB_STATIC_RATE_DONTCARE;
		groupImage->MaxExtRate = IB_STATIC_RATE_DONTCARE;
	}
}

// compute averages
void FinalizeVFStats(PmVFImage_t *vfImage)
{
	if (vfImage->NumPorts) {
		vfImage->IntUtil.AvgMBps = vfImage->IntUtil.TotMBps/vfImage->NumPorts;
		vfImage->IntUtil.AvgKPps = vfImage->IntUtil.TotKPps/vfImage->NumPorts;
	} else {
		// avoid any possible confusion, remove UINT_MAX value
		vfImage->IntUtil.MinMBps = 0;
		vfImage->IntUtil.MinKPps = 0;
		vfImage->MinIntRate = IB_STATIC_RATE_DONTCARE;
		vfImage->MaxIntRate = IB_STATIC_RATE_DONTCARE;
	}
}

// given a MBps transfer rate and a theoretical maxMBps, compute
// the utilization bucket number from 0 to PM_UTIL_BUCKETS-1
static __inline uint8 ComputeUtilBucket(uint32 SendMBps, uint32 maxMBps)
{
	// MaxMBps at 120g (QDR 12x) is 11444, so no risk of uint32 overflow
	if (maxMBps) {
		// directly compute bucket to reduce overflow chances
		uint8 utilBucket = (SendMBps * PM_UTIL_BUCKETS) / maxMBps;
		if (utilBucket >= PM_UTIL_BUCKETS)
			return PM_UTIL_BUCKETS-1;
		else
			return utilBucket;
	} else {
		return 0;
	}
}

// given a error count and a threshold, compute
// the err bucket number from 0 to PM_ERR_BUCKETS-1
static __inline uint8 ComputeErrBucket(uint32 errCnt, uint32 errThreshold)
{
	uint8 errBucket;

	// directly compute bucket to reduce overflow chances
	if (! errThreshold)	// ignore class of errors
		return 0;
	errBucket = (errCnt * (PM_ERR_BUCKETS-1)) / errThreshold;
	if (errBucket >= PM_ERR_BUCKETS)
		 return PM_ERR_BUCKETS-1;
	else
		 return errBucket;
}

void ComputeBuckets(Pm_t *pm, PmPortImage_t *portImage)
{
#define COMPUTE_ERR_BUCKET(stat) do { \
		portImage->stat##Bucket = ComputeErrBucket( portImage->Errors.stat, \
				   							pm->Thresholds.stat); \
	} while (0)

	if (! portImage->u.s.bucketComputed) {
		uint32 rate = PmCalculateRate(portImage->u.s.activeSpeed, portImage->u.s.rxActiveWidth);
		portImage->UtilBucket = ComputeUtilBucket(portImage->SendMBps,
				   					s_StaticRateToMBps[rate]);
		COMPUTE_ERR_BUCKET(Integrity);
		COMPUTE_ERR_BUCKET(Congestion);
		COMPUTE_ERR_BUCKET(SmaCongestion);
		COMPUTE_ERR_BUCKET(Bubble);
		COMPUTE_ERR_BUCKET(Security);
		COMPUTE_ERR_BUCKET(Routing);
		portImage->u.s.bucketComputed = 1;
	}
#undef COMPUTE_ERR_BUCKET
}

void PmPrintExceededPort(PmPort_t *pmportp, uint32 index,
				const char *statistic, uint32 threshold, uint32 value)
{
	PmPort_t *pmportp2 = pmportp->Image[index].neighbor;

	if (pmportp2) {
		IB_LOG_WARN_FMT(NULL, "%s of %u Exceeded Threshold of %u. %.*s Guid "FMT_U64" LID 0x%x Port %u"
			"  Neighbor: %.*s Guid "FMT_U64" LID 0x%x Port %u",
			statistic, value, threshold,
			(int)sizeof(pmportp->pmnodep->nodeDesc.NodeString), pmportp->pmnodep->nodeDesc.NodeString,
			pmportp->pmnodep->guid, pmportp->pmnodep->Image[index].lid, pmportp->portNum,
			(int)sizeof(pmportp2->pmnodep->nodeDesc.NodeString), pmportp2->pmnodep->nodeDesc.NodeString,
			pmportp2->pmnodep->guid, pmportp2->pmnodep->Image[index].lid, pmportp2->portNum);
	} else {
		IB_LOG_WARN_FMT(NULL, "%s of %u Exceeded Threshold of %u. %.*s Guid "FMT_U64" LID 0x%x Port %u",
			statistic, value, threshold,
			(int)sizeof(pmportp->pmnodep->nodeDesc.NodeString), pmportp->pmnodep->nodeDesc.NodeString,
			pmportp->pmnodep->guid, pmportp->pmnodep->Image[index].lid, pmportp->portNum);
	}
}

#define GET_DELTA_COUNTER(cntr) \
	(portImage->StlPortCounters.cntr - (portImagePrev ? \
		(portImagePrev->clearSelectMask.s.cntr ? 0 : \
			portImagePrev->StlPortCounters.cntr) : 0))
#define GET_NEIGHBOR_DELTA_COUNTER(cntr) (portImageNeighbor ? \
	(portImageNeighbor->StlPortCounters.cntr - \
		(portImageNeighborPrev ? \
			(portImageNeighborPrev->clearSelectMask.s.cntr ? 0 : \
				portImageNeighborPrev->StlPortCounters.cntr) : 0)) : 0)
#define GET_DELTA_VLCOUNTER(vlcntr, vl, cntr) \
	(portImage->StlVLPortCounters[vl].vlcntr - (portImagePrev ? \
		(portImagePrev->clearSelectMask.s.cntr ? 0 : \
			portImagePrev->StlVLPortCounters[vl].vlcntr) : 0))
#define GET_NEIGHBOR_DELTA_VLCOUNTER(vlcntr, vl, cntr) \
	(portImageNeighbor ? (portImageNeighbor->StlVLPortCounters[vl].vlcntr - \
		(portImageNeighborPrev ? \
			(portImageNeighborPrev->clearSelectMask.s.cntr ? 0 : \
				portImageNeighborPrev->StlVLPortCounters[vl].vlcntr) : 0)) : 0)

void PmPrintExceededPortDetailsIntegrity(PmPort_t *pmportp, PmPort_t *pmportneighborp, uint32 imageIndex, uint32 lastImageIndex)
{
	PmPortImage_t *portImage = &pmportp->Image[imageIndex];
	PmPortImage_t *portImageNeighbor = (pmportneighborp ? &pmportneighborp->Image[imageIndex] : NULL);
	PmPortImage_t *portImagePrev = NULL;
	PmPortImage_t *portImageNeighborPrev = NULL;

	if (lastImageIndex != PM_IMAGE_INDEX_INVALID) {
		portImagePrev = &pmportp->Image[lastImageIndex];
		portImageNeighborPrev = (pmportneighborp ? &pmportneighborp->Image[lastImageIndex] : NULL);
	}

	char message[256] = {0};
	char * logMessage = message;
	int buffSpace = sizeof(message);

	uint64 LocalLinkIntegrityErrors = GET_DELTA_COUNTER(LocalLinkIntegrityErrors);
	uint64 PortRcvErrors            = GET_DELTA_COUNTER(PortRcvErrors);
	uint32 LinkErrorRecovery        = GET_DELTA_COUNTER(LinkErrorRecovery);
	uint32 LinkDowned               = GET_DELTA_COUNTER(LinkDowned);
	uint8  LinkQualityIndicator     = portImage->StlPortCounters.lq.s.LinkQualityIndicator;
	uint32 LwdTx                    = StlLinkWidthToInt(portImage->u.s.txActiveWidth);
	uint32 LwdRx                    = StlLinkWidthToInt(portImage->u.s.rxActiveWidth);
	uint8  UncorrectableErrors      = GET_DELTA_COUNTER(UncorrectableErrors);
	uint64 FMConfigErrors           = GET_DELTA_COUNTER(FMConfigErrors);
	uint64 ExcessiveBufferOverruns  = GET_NEIGHBOR_DELTA_COUNTER(ExcessiveBufferOverruns);

	//for each counter, determine if it contributed to error condition
	if (LocalLinkIntegrityErrors && pm_config.integrityWeights.LocalLinkIntegrityErrors) {
		snprintf(logMessage, buffSpace, "LLI=%"PRIu64" ", LocalLinkIntegrityErrors);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (PortRcvErrors && pm_config.integrityWeights.PortRcvErrors) {
		snprintf(logMessage, buffSpace, "RxE=%"PRIu64" ", PortRcvErrors);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (LinkErrorRecovery && pm_config.integrityWeights.LinkErrorRecovery) {
		snprintf(logMessage, buffSpace, "LER=%u ", LinkErrorRecovery);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (LinkDowned && pm_config.integrityWeights.LinkDowned) {
		snprintf(logMessage, buffSpace, "LD=%u ", LinkDowned);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (LinkQualityIndicator < STL_LINKQUALITY_EXCELLENT && pm_config.integrityWeights.LinkQualityIndicator) {
		snprintf(logMessage, buffSpace, "LQI=%u ", LinkQualityIndicator);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if ((LwdTx < StlLinkWidthToInt(STL_LINK_WIDTH_4X) || LwdRx < StlLinkWidthToInt(STL_LINK_WIDTH_4X)) && pm_config.integrityWeights.LinkWidthDowngrade) {
		snprintf(logMessage, buffSpace, "LWD.Tx=%ux Rx=%ux ", LwdTx, LwdRx);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (UncorrectableErrors && pm_config.integrityWeights.UncorrectableErrors) {
		snprintf(logMessage, buffSpace, "Unc=%u ", UncorrectableErrors);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (FMConfigErrors && pm_config.integrityWeights.FMConfigErrors) {
		snprintf(logMessage, buffSpace, "FMC=%"PRIu64" ", FMConfigErrors);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (ExcessiveBufferOverruns && pm_config.integrityWeights.ExcessiveBufferOverruns) {
		snprintf(logMessage, buffSpace, "neighbor EBO=%"PRIu64" ", ExcessiveBufferOverruns);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}

	IB_LOG_WARN_FMT(NULL, message);
}
void PmPrintExceededPortDetailsCongestion(PmPort_t *pmportp, PmPort_t *pmportneighborp, uint32 imageIndex, uint32 lastImageIndex)
{
	PmPortImage_t *portImage = &pmportp->Image[imageIndex];
	PmPortImage_t *portImageNeighbor = (pmportneighborp ? &pmportneighborp->Image[imageIndex] : NULL);
	PmPortImage_t *portImagePrev = NULL;
	PmPortImage_t *portImageNeighborPrev = NULL;

	char message[256] = {0};
	char * logMessage = message;
	int buffSpace = sizeof(message);

	if (lastImageIndex != PM_IMAGE_INDEX_INVALID) {
		portImagePrev = &pmportp->Image[lastImageIndex];
		portImageNeighborPrev = (pmportneighborp ? &pmportneighborp->Image[lastImageIndex] : NULL);
	}

	uint64 DeltaXmitData = GET_DELTA_COUNTER(PortXmitData);
	uint64 DeltaXmitPkts = GET_DELTA_COUNTER(PortXmitPkts);
	uint64 DeltaRcvPkts = GET_DELTA_COUNTER(PortRcvPkts);
	uint64 DeltaXmitPkts_N = GET_NEIGHBOR_DELTA_COUNTER(PortXmitPkts);

	uint64 DeltaXmitWait = GET_DELTA_COUNTER(PortXmitWait);
	uint64 DeltaXmitTimeCong = GET_DELTA_COUNTER(PortXmitTimeCong);
	uint64 DeltaRcvBECN = GET_DELTA_COUNTER(PortRcvBECN);
	uint64 DeltaMarkFECN = GET_DELTA_COUNTER(PortMarkFECN);
	uint64 DeltaRcvFECN_N = GET_NEIGHBOR_DELTA_COUNTER(PortRcvFECN);
	uint64 DeltaSwPortCong = GET_DELTA_COUNTER(SwPortCongestion);

	if (pm_config.process_vl_counters && DeltaXmitWait) {
		uint64 MaxDeltaVLXmitWait = 0;
		uint32 NumVLs, VLSelectMask, i;
		for (i = 0, NumVLs = 0, VLSelectMask = portImage->vlSelectMask; i < MAX_PM_VLS && VLSelectMask; i++, VLSelectMask >>= 1) {
			UPDATE_MAX(MaxDeltaVLXmitWait, GET_DELTA_VLCOUNTER(PortVLXmitWait, i, PortXmitWait));
			NumVLs += (VLSelectMask & 0x1);
		}
		DeltaXmitWait = (MaxDeltaVLXmitWait * MaxDeltaVLXmitWait * NumVLs) / DeltaXmitWait;
	}

	uint32 XmitWaitPct = (uint32)(DeltaXmitWait ?
		(DeltaXmitWait * 10000) / (DeltaXmitWait + DeltaXmitData) : 0);
	uint32 RcvFECNPct = (uint32)(DeltaXmitPkts_N ?
		(DeltaRcvFECN_N * 1000) / (DeltaXmitPkts_N) : 0);

	uint32 RcvBECNPct = (uint32)(DeltaRcvPkts ?
		(DeltaRcvBECN * 1000 * (pmportp->pmnodep->nodeType & STL_NODE_FI)) / (DeltaRcvPkts) : 0);
	uint32 XmitTimeCongPct = (uint32)(DeltaXmitTimeCong ?
		(DeltaXmitTimeCong * 1000) / (DeltaXmitTimeCong + DeltaXmitData) : 0);
	uint32 MarkFECNPct = (uint32)(DeltaXmitPkts ?
		(DeltaMarkFECN * 1000) / (DeltaXmitPkts) : 0);

	if (DeltaXmitWait && pm_config.congestionWeights.PortXmitWait){
		snprintf(logMessage, buffSpace, "TxW=%"PRIu64" TxWPct=%u ", DeltaXmitWait, XmitWaitPct);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (DeltaSwPortCong && pm_config.congestionWeights.SwPortCongestion){
		snprintf(logMessage, buffSpace, "CD=%"PRIu64" ", DeltaSwPortCong);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (DeltaRcvBECN && pm_config.congestionWeights.PortRcvBECN){
		snprintf(logMessage, buffSpace, "RxB=%"PRIu64" RxBPct=%u ", DeltaRcvBECN, RcvBECNPct);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (DeltaXmitTimeCong && pm_config.congestionWeights.PortXmitTimeCong){
		snprintf(logMessage, buffSpace, "TxTC=%"PRIu64" TxTCPct=%u ", DeltaXmitTimeCong, XmitTimeCongPct);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (DeltaMarkFECN && pm_config.congestionWeights.PortMarkFECN){
		snprintf(logMessage, buffSpace, "MkF=%"PRIu64" MkFPct=%u ", DeltaMarkFECN, MarkFECNPct);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (DeltaRcvFECN_N && pm_config.congestionWeights.PortRcvFECN){
		snprintf(logMessage, buffSpace, "neighbor RxF=%"PRIu64" neighbor RxFPct=%u ", DeltaRcvFECN_N, RcvFECNPct);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}

	IB_LOG_WARN_FMT(NULL, message);
}
void PmPrintExceededPortDetailsSmaCongestion(PmPort_t *pmportp, PmPort_t *pmportneighborp, uint32 imageIndex, uint32 lastImageIndex)
{
	PmPortImage_t *portImage = &pmportp->Image[imageIndex];
	PmPortImage_t *portImageNeighbor = (pmportneighborp ? &pmportneighborp->Image[imageIndex] : NULL);
	PmPortImage_t *portImagePrev = NULL;
	PmPortImage_t *portImageNeighborPrev = NULL;

	char message[256] = {0};
	char * logMessage = message;
	int buffSpace = sizeof(message);

	if (lastImageIndex != PM_IMAGE_INDEX_INVALID) {
		portImagePrev = &pmportp->Image[lastImageIndex];
		portImageNeighborPrev = (pmportneighborp ? &pmportneighborp->Image[lastImageIndex] : NULL);
	}

	uint64 DeltaVLXmitData = GET_DELTA_VLCOUNTER(PortVLXmitData, 15, PortXmitData);
	uint64 DeltaVLXmitPkts = GET_DELTA_VLCOUNTER(PortVLXmitPkts, 15, PortXmitPkts);
	uint64 DeltaVLRcvPkts = GET_DELTA_VLCOUNTER(PortVLRcvPkts, 15, PortRcvPkts);
	uint64 DeltaVLXmitPkts_N = GET_NEIGHBOR_DELTA_VLCOUNTER(PortVLXmitPkts, 15, PortXmitPkts);

	uint64 DeltaVLXmitWait = GET_DELTA_VLCOUNTER(PortVLXmitWait, 15, PortXmitWait);
	uint64 DeltaVLXmitTimeCong = GET_DELTA_VLCOUNTER(PortVLXmitTimeCong, 15, PortXmitTimeCong);
	uint64 DeltaVLRcvBECN = GET_DELTA_VLCOUNTER(PortVLRcvBECN, 15, PortRcvBECN);
	uint64 DeltaVLMarkFECN = GET_DELTA_VLCOUNTER(PortVLMarkFECN, 15, PortMarkFECN);
	uint64 DeltaVLRcvFECN_N = GET_NEIGHBOR_DELTA_VLCOUNTER(PortVLRcvFECN, 15, PortRcvFECN);
	uint64 DeltaVLSwPortCong = GET_DELTA_VLCOUNTER(SwPortVLCongestion, 15, SwPortCongestion);

	uint32 VLXmitWaitPct = (uint32)(DeltaVLXmitWait ?
		(DeltaVLXmitWait * 10000) / (DeltaVLXmitWait + DeltaVLXmitData) : 0);
	uint32 VLRcvFECNPct = (uint32)(DeltaVLXmitPkts_N ?
		(DeltaVLRcvFECN_N * 1000) / (DeltaVLXmitPkts_N) : 0);

	uint32 VLRcvBECNPct = (uint32)(DeltaVLRcvPkts ?
		(DeltaVLRcvBECN * 1000 * (pmportp->pmnodep->nodeType & STL_NODE_FI)) / (DeltaVLRcvPkts) : 0);
	uint32 VLXmitTimeCongPct = (uint32)(DeltaVLXmitTimeCong ?
		(DeltaVLXmitTimeCong * 1000) / (DeltaVLXmitTimeCong + DeltaVLXmitData) : 0);
	uint32 VLMarkFECNPct = (uint32)(DeltaVLXmitPkts ?
		(DeltaVLMarkFECN * 1000) / (DeltaVLXmitPkts) : 0);

	if (DeltaVLXmitWait && pm_config.congestionWeights.PortXmitWait){
		snprintf(logMessage, buffSpace, "VLTxW[15]=%"PRIu64" VLTxWPct[15]=%u ", DeltaVLXmitWait, VLXmitWaitPct);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (DeltaVLSwPortCong && pm_config.congestionWeights.SwPortCongestion){
		snprintf(logMessage, buffSpace, "VLCD[15]=%"PRIu64" ", DeltaVLSwPortCong);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (DeltaVLRcvBECN && pm_config.congestionWeights.PortRcvBECN){
		snprintf(logMessage, buffSpace, "VLRxB[15]=%"PRIu64" VLRxBPct[15]=%u ", DeltaVLRcvBECN, VLRcvBECNPct);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (DeltaVLXmitTimeCong && pm_config.congestionWeights.PortXmitTimeCong){
		snprintf(logMessage, buffSpace, "VLTxTC[15]=%"PRIu64" VLTxTCPct[15]=%u ", DeltaVLXmitTimeCong, VLXmitTimeCongPct);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (DeltaVLMarkFECN && pm_config.congestionWeights.PortMarkFECN){
		snprintf(logMessage, buffSpace, "VLMkF[15]=%"PRIu64" VLMkFPct[15]=%u ", DeltaVLMarkFECN, VLMarkFECNPct);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}
	if (DeltaVLRcvFECN_N && pm_config.congestionWeights.PortRcvFECN){
		snprintf(logMessage, buffSpace, "neighbor VLRxF[15]=%"PRIu64" neighbor VLRxFPct[15]=%u ", DeltaVLRcvFECN_N, VLRcvFECNPct);
		buffSpace -= strlen(logMessage);
		logMessage += strlen(logMessage);
	}

	IB_LOG_WARN_FMT(NULL, message);
}
void PmPrintExceededPortDetailsBubble(PmPort_t *pmportp, PmPort_t *pmportneighborp, uint32 imageIndex, uint32 lastImageIndex)
{
	PmPortImage_t *portImage = &pmportp->Image[imageIndex];
	PmPortImage_t *portImageNeighbor = (pmportneighborp ? &pmportneighborp->Image[imageIndex] : NULL);
	PmPortImage_t *portImagePrev = NULL;
	PmPortImage_t *portImageNeighborPrev = NULL;

	if (lastImageIndex != PM_IMAGE_INDEX_INVALID) {
		portImagePrev = &pmportp->Image[lastImageIndex];
		portImageNeighborPrev = (pmportneighborp ? &pmportneighborp->Image[lastImageIndex] : NULL);
	}

	uint64 DeltaXmitData = GET_DELTA_COUNTER(PortXmitData);
	uint64 DeltaRcvData_N = GET_NEIGHBOR_DELTA_COUNTER(PortRcvData);

	uint64 DeltaXmitWastedBW = GET_DELTA_COUNTER(PortXmitWastedBW);
	uint64 DeltaXmitWaitData = GET_DELTA_COUNTER(PortXmitWaitData);
	uint64 DeltaRcvBubble_N = GET_NEIGHBOR_DELTA_COUNTER(PortRcvBubble);
	uint64 DeltaXmitBubble = DeltaXmitWastedBW + DeltaXmitWaitData;

	uint32 XmitBubblePct = (uint32)(DeltaXmitBubble ?
		(DeltaXmitBubble * 10000) / (DeltaXmitData + DeltaXmitBubble): 0);
	uint32 RcvBubblePct = (uint32)(DeltaRcvBubble_N ?
		(DeltaRcvBubble_N * 10000) / (DeltaRcvData_N + DeltaRcvBubble_N): 0);

	IB_LOG_WARN_FMT(NULL, "WBW=%"PRIu64", TxWD=%"PRIu64", TxBbPct=%u, neighbor RxBb=%"PRIu64" neighbor RxBbPct=%u ",
		DeltaXmitWastedBW, DeltaXmitWaitData, XmitBubblePct, DeltaRcvBubble_N, RcvBubblePct);
}
void PmPrintExceededPortDetailsSecurity(PmPort_t *pmportp, PmPort_t *pmportneighborp, uint32 imageIndex, uint32 lastImageIndex)
{
	PmPortImage_t *portImage = &pmportp->Image[imageIndex];
	PmPortImage_t *portImageNeighbor = (pmportneighborp ? &pmportneighborp->Image[imageIndex] : NULL);
	PmPortImage_t *portImagePrev = NULL;
	PmPortImage_t *portImageNeighborPrev = NULL;

	if (lastImageIndex != PM_IMAGE_INDEX_INVALID) {
		portImagePrev = &pmportp->Image[lastImageIndex];
		portImageNeighborPrev = (pmportneighborp ? &pmportneighborp->Image[lastImageIndex] : NULL);
	}

	IB_LOG_WARN_FMT(NULL, "TxCE=%"PRIu64", neighbor RxCE=%"PRIu64" ",
		GET_DELTA_COUNTER(PortXmitConstraintErrors),
		GET_NEIGHBOR_DELTA_COUNTER(PortRcvConstraintErrors));
}
void PmPrintExceededPortDetailsRouting(PmPort_t *pmportp, PmPort_t *pmportneighborp, uint32 imageIndex, uint32 lastImageIndex)
{
	PmPortImage_t *portImage = &pmportp->Image[imageIndex];
	/*PmPortImage_t *portImageNeighbor = (pmportneighborp ? &pmportneighborp->Image[imageIndex] : NULL); */
	PmPortImage_t *portImagePrev = NULL;
	/*PmPortImage_t *portImageNeighborPrev = NULL; */

	if (lastImageIndex != PM_IMAGE_INDEX_INVALID) {
		portImagePrev = &pmportp->Image[lastImageIndex];
		/*portImageNeighborPrev = (pmportneighborp ? &pmportneighborp->Image[lastImageIndex] : NULL); */
	}

	IB_LOG_WARN_FMT(NULL, "RxSR=%"PRIu64" ",
		GET_DELTA_COUNTER(PortRcvSwitchRelayErrors));
}
#undef GET_DELTA_COUNTER
#undef GET_NEIGHBOR_DELTA_COUNTER
#undef GET_DELTA_VLCOUNTER
#undef GET_NEIGHBOR_DELTA_VLCOUNTER

static void PmUnexpectedClear(Pm_t *pm, PmPort_t *pmportp, uint32 imageIndex,
	CounterSelectMask_t unexpectedClear)
{
	PmImage_t *pmimagep = &pm->Image[imageIndex];
	PmNode_t *pmnodep = pmportp->pmnodep;
	char *detail = "";
	detail=": Make sure no other tools are clearing fabric counters";
	char CounterNameBuffer[128];
	FormatStlCounterSelectMask(CounterNameBuffer, unexpectedClear);

	if (pmimagep->FailedNodes + pmimagep->FailedPorts
				   	+ pmimagep->UnexpectedClearPorts < pm_config.SweepErrorsLogThreshold)
	{
		IB_LOG_WARN_FMT(NULL, "Unexpected counter clear for %.*s Guid "FMT_U64" LID 0x%x Port %u%s (Mask 0x%08x: %s)",
			(int)sizeof(pmnodep->nodeDesc.NodeString), pmnodep->nodeDesc.NodeString,
			pmnodep->guid, pmnodep->Image[imageIndex].lid, pmportp->portNum, detail,
			unexpectedClear.AsReg32, CounterNameBuffer);
	} else {
		IB_LOG_INFO_FMT(NULL, "Unexpected counter clear for %.*s Guid "FMT_U64" LID 0x%x Port %u%s (Mask 0x%08x: %s)",
			(int)sizeof(pmnodep->nodeDesc.NodeString), pmnodep->nodeDesc.NodeString,
			pmnodep->guid, pmnodep->Image[imageIndex].lid, pmportp->portNum, detail,
			unexpectedClear.AsReg32, CounterNameBuffer);
	}
	pmimagep->UnexpectedClearPorts++;
	INCREMENT_PM_COUNTER(pmCounterPmUnexpectedClearPorts);
}
// After all individual ports have been tabulated, we tabulate totals for
// all groups.  We must do this after port tabulation because some counters
// need to look at both sides of a link to pick the max or combine error
// counters.  Hence we could not accurately indicate buckets nor totals for a
// group until first pass through all ports has been completed.
// We also compute RunningTotals here because caller will have the appropriate
// locks.
//
// caller must hold imageLock for write on this image (index)
// and totalsLock for write and imageLock held for write
void PmFinalizePortStats(Pm_t *pm, PmPort_t *pmportp, uint32 index)
{
	PmPortImage_t *portImage = &pmportp->Image[index];

	PmCompositePortCounters_t *pRunning = &pmportp->StlPortCountersTotal;
	PmCompositeVLCounters_t *pVLRunning = &pmportp->StlVLPortCountersTotal[0];

	PmCompositePortCounters_t *pImgPortCounters = &portImage->StlPortCounters;
	PmCompositeVLCounters_t *pImgPortVLCounters = &portImage->StlVLPortCounters[0];

	if (pm->LastSweepIndex == PM_IMAGE_INDEX_INVALID) {
		// Initialize LQI
		pRunning->lq.s.LinkQualityIndicator = pImgPortCounters->lq.s.LinkQualityIndicator;
		return;
	}
	int i;
	// Local Ports Previous Image
	uint32 imageIndexPrevious = pm->LastSweepIndex;
	PmPortImage_t *portImagePrev = &pmportp->Image[imageIndexPrevious];
	PmCompositePortCounters_t *pImgPortCountersPrev = &portImagePrev->StlPortCounters;
	PmCompositeVLCounters_t *pImgPortVLCountersPrev = &portImagePrev->StlVLPortCounters[0];
	// Counter Select Mask of Counter cleared by the PM during LastSweep
	CounterSelectMask_t prevPortClrSelMask = portImagePrev->clearSelectMask;

	// Neighbor's Current PortImage
	PmPortImage_t *portImageNeighbor = (portImage->neighbor ? &portImage->neighbor->Image[index] : NULL);

	// Delta's used for Category Calulations and Running Counters
	PmCompositePortCounters_t DeltaPortCounters = {0};
	PmCompositeVLCounters_t DeltaVLCounters[MAX_PM_VLS] = {{0}};

	CounterSelectMask_t unexpectedClear = {0};
	uint64 maxPortVLXmitWait = 0;
	uint32 DeltaLinkQualityIndicator = 0;

	IB_LOG_DEBUG3_FMT(__func__, "%.*s Guid "FMT_U64" LID 0x%x Port %u",
		(int)sizeof(pmportp->pmnodep->nodeDesc.NodeString), pmportp->pmnodep->nodeDesc.NodeString,
		pmportp->pmnodep->guid, pmportp->pmnodep->dlid, pmportp->portNum);

        // If LinkWidth.Active is greater than LinkWidthDowngrade.txActive then port is downgraded
        if (pImgPortCounters->lq.s.NumLanesDown) {
            pm->Image[index].DowngradedPorts++;
        }
	if (portImage->u.s.gotDataCntrs) {

#define GET_DELTA_PORTCOUNTERS(cntr) do { \
		if (pImgPortCounters->cntr < pImgPortCountersPrev->cntr || prevPortClrSelMask.s.cntr) { \
			unexpectedClear.s.cntr = !prevPortClrSelMask.s.cntr; \
			DeltaPortCounters.cntr = pImgPortCounters->cntr; \
		} else { \
			DeltaPortCounters.cntr = pImgPortCounters->cntr - pImgPortCountersPrev->cntr; \
		} } while (0)

#define GET_DELTA_VLCOUNTERS(vlcntr, vl, cntr) do { \
		if (pImgPortVLCounters[vl].vlcntr < pImgPortVLCountersPrev[vl].vlcntr || prevPortClrSelMask.s.cntr) { \
			unexpectedClear.s.cntr = !prevPortClrSelMask.s.cntr; \
			DeltaVLCounters[vl].vlcntr = pImgPortVLCounters[vl].vlcntr; \
		} else { \
			DeltaVLCounters[vl].vlcntr = pImgPortVLCounters[vl].vlcntr - pImgPortVLCountersPrev[vl].vlcntr; \
		} } while (0)

		GET_DELTA_PORTCOUNTERS(PortXmitData);
		GET_DELTA_PORTCOUNTERS(PortXmitPkts);
		GET_DELTA_PORTCOUNTERS(PortRcvData);
		GET_DELTA_PORTCOUNTERS(PortRcvPkts);
		GET_DELTA_PORTCOUNTERS(PortMulticastXmitPkts);
		GET_DELTA_PORTCOUNTERS(PortMulticastRcvPkts);
		GET_DELTA_PORTCOUNTERS(PortXmitWait);
		GET_DELTA_PORTCOUNTERS(SwPortCongestion);
		GET_DELTA_PORTCOUNTERS(PortRcvFECN);
		GET_DELTA_PORTCOUNTERS(PortRcvBECN);
		GET_DELTA_PORTCOUNTERS(PortXmitTimeCong);
		GET_DELTA_PORTCOUNTERS(PortXmitWastedBW);
		GET_DELTA_PORTCOUNTERS(PortXmitWaitData);
		GET_DELTA_PORTCOUNTERS(PortRcvBubble);
		GET_DELTA_PORTCOUNTERS(PortMarkFECN);
		if (pm_config.process_vl_counters) {
			for (i = 0; i < MAX_PM_VLS; i++) {
				GET_DELTA_VLCOUNTERS(PortVLXmitData,     i, PortXmitData);
				GET_DELTA_VLCOUNTERS(PortVLXmitPkts,     i, PortXmitPkts);
				GET_DELTA_VLCOUNTERS(PortVLRcvData,      i, PortRcvData);
				GET_DELTA_VLCOUNTERS(PortVLRcvPkts,      i, PortRcvPkts);
				GET_DELTA_VLCOUNTERS(PortVLXmitWait,     i, PortXmitWait);
				UPDATE_MAX(maxPortVLXmitWait, DeltaVLCounters[i].PortVLXmitWait); // for tracking most congested VL.
				GET_DELTA_VLCOUNTERS(SwPortVLCongestion, i, SwPortCongestion);
				GET_DELTA_VLCOUNTERS(PortVLRcvFECN,      i, PortRcvFECN);
				GET_DELTA_VLCOUNTERS(PortVLRcvBECN,      i, PortRcvBECN);
				GET_DELTA_VLCOUNTERS(PortVLXmitTimeCong, i, PortXmitTimeCong);
				GET_DELTA_VLCOUNTERS(PortVLXmitWastedBW, i, PortXmitWastedBW);
				GET_DELTA_VLCOUNTERS(PortVLXmitWaitData, i, PortXmitWaitData);
				GET_DELTA_VLCOUNTERS(PortVLRcvBubble,    i, PortRcvBubble);
				GET_DELTA_VLCOUNTERS(PortVLMarkFECN,     i, PortMarkFECN);
			}
		}

                // 2^(5-LQI) - 1 = (1<<(5-LQI))-1 = {0,1,3,7,15,31}
                DeltaLinkQualityIndicator =
                    (pImgPortCounters->lq.s.LinkQualityIndicator <= STL_LINKQUALITY_EXCELLENT ?
                    ((1 << (STL_LINKQUALITY_EXCELLENT - pImgPortCounters->lq.s.LinkQualityIndicator) ) - 1) : 0);

		if (portImage->u.s.gotErrorCntrs) {
			GET_DELTA_PORTCOUNTERS(PortRcvConstraintErrors);
			GET_DELTA_PORTCOUNTERS(PortXmitDiscards);
			GET_DELTA_PORTCOUNTERS(PortXmitConstraintErrors);
			GET_DELTA_PORTCOUNTERS(PortRcvSwitchRelayErrors);
			GET_DELTA_PORTCOUNTERS(PortRcvRemotePhysicalErrors);
			GET_DELTA_PORTCOUNTERS(LocalLinkIntegrityErrors);
			GET_DELTA_PORTCOUNTERS(PortRcvErrors);
			GET_DELTA_PORTCOUNTERS(ExcessiveBufferOverruns);
			GET_DELTA_PORTCOUNTERS(FMConfigErrors);
			GET_DELTA_PORTCOUNTERS(LinkErrorRecovery);
			GET_DELTA_PORTCOUNTERS(LinkDowned);
			GET_DELTA_PORTCOUNTERS(UncorrectableErrors);
			if (pm_config.process_vl_counters) {
				for (i = 0; i < MAX_PM_VLS; i++) {
					GET_DELTA_VLCOUNTERS(PortVLXmitDiscards, i, PortXmitDiscards);
				}
			}
		} else {
			// Copy Previous Image Error Counters if Error Counters were not queried this sweep
#define GET_ERROR_PORTCOUNTERS(cntr) \
	pImgPortCounters->cntr = pImgPortCountersPrev->cntr
#define GET_ERROR_VLCOUNTERS(vlcntr, vl) \
	pImgPortVLCounters[vl].vlcntr = pImgPortVLCountersPrev[vl].vlcntr

			GET_ERROR_PORTCOUNTERS(PortRcvConstraintErrors);
			GET_ERROR_PORTCOUNTERS(PortXmitDiscards);
			GET_ERROR_PORTCOUNTERS(PortXmitConstraintErrors);
			GET_ERROR_PORTCOUNTERS(PortRcvSwitchRelayErrors);
			GET_ERROR_PORTCOUNTERS(PortRcvRemotePhysicalErrors);
			GET_ERROR_PORTCOUNTERS(LocalLinkIntegrityErrors);
			GET_ERROR_PORTCOUNTERS(PortRcvErrors);
			GET_ERROR_PORTCOUNTERS(ExcessiveBufferOverruns);
			GET_ERROR_PORTCOUNTERS(FMConfigErrors);
			GET_ERROR_PORTCOUNTERS(LinkErrorRecovery);
			GET_ERROR_PORTCOUNTERS(LinkDowned);
			GET_ERROR_PORTCOUNTERS(UncorrectableErrors);
			if (pm_config.process_vl_counters) {
				for (i = 0; i < MAX_PM_VLS; i++) {
					GET_ERROR_VLCOUNTERS(PortVLXmitDiscards, i);
				}
			}
#undef GET_ERROR_VLCOUNTERS
#undef GET_ERROR_PORTCOUNTERS
		}
#undef GET_DELTA_VLCOUNTERS
#undef GET_DELTA_PORTCOUNTERS
	}

	if (DeltaPortCounters.LinkDowned) {
		if (unexpectedClear.AsReg32 & ~LinkDownIgnoreMask.AsReg32) {
			CounterSelectMask_t tempMask;
			tempMask.AsReg32 = unexpectedClear.AsReg32 & ~LinkDownIgnoreMask.AsReg32 ;
			portImage->u.s.UnexpectedClear = 1;
			PmUnexpectedClear(pm, pmportp, index, tempMask);
		}
	} else {
		if (unexpectedClear.AsReg32) {
			portImage->u.s.UnexpectedClear = 1;
			PmUnexpectedClear(pm, pmportp, index, unexpectedClear);
		}
	}
	//Copy In the Unexpected Clears to the previous image so the PA can handle them correctly
	portImagePrev->clearSelectMask.AsReg32 |= unexpectedClear.AsReg32;


	// Calulate Port Utilization
	UPDATE_MAX(portImage->SendMBps, (DeltaPortCounters.PortXmitData) / FLITS_PER_MB / pm->interval);
	UPDATE_MAX(portImage->SendKPps, (DeltaPortCounters.PortXmitPkts) / 1000 / pm->interval);

	// Calulate Port Errors from local counters
	portImage->Errors.Integrity += (uint32)(
		DeltaPortCounters.LocalLinkIntegrityErrors  * pm->integrityWeights.LocalLinkIntegrityErrors +
		DeltaPortCounters.PortRcvErrors             * pm->integrityWeights.PortRcvErrors +
		DeltaPortCounters.LinkErrorRecovery         * pm->integrityWeights.LinkErrorRecovery +
		DeltaPortCounters.LinkDowned                * pm->integrityWeights.LinkDowned +
		DeltaPortCounters.UncorrectableErrors       * pm->integrityWeights.UncorrectableErrors +
		DeltaPortCounters.FMConfigErrors            * pm->integrityWeights.FMConfigErrors +
		DeltaLinkQualityIndicator                   * pm->integrityWeights.LinkQualityIndicator +
		pImgPortCounters->lq.s.NumLanesDown         * pm->integrityWeights.LinkWidthDowngrade);

	if (pm_config.process_vl_counters) {
		if (DeltaPortCounters.PortXmitWait) {
			uint8 i, numVLs=0;
			for (i=0; i<MAX_PM_VLS; i++) {
				numVLs += (portImage->vlSelectMask >> i) & 0x1;
			}
			// PortXmitWait counter is incremented once if any VLs experienced congestion.
			// It is possible that minimal congestion in sequence could be treated as severe port congestion.
			// So modify PortXmitWait taking into consideration the VL xmit wait counter data.
			DeltaPortCounters.PortXmitWait = (maxPortVLXmitWait * maxPortVLXmitWait * numVLs)/DeltaPortCounters.PortXmitWait;
		}
	}

	portImage->Errors.Congestion += (uint32)(
		(DeltaPortCounters.PortXmitWait ? (DeltaPortCounters.PortXmitWait *
			pm->congestionWeights.PortXmitWait * 10000) /
			(DeltaPortCounters.PortXmitData + DeltaPortCounters.PortXmitWait) : 0) +
		(DeltaPortCounters.PortXmitTimeCong ? (DeltaPortCounters.PortXmitTimeCong *
			pm->congestionWeights.PortXmitTimeCong * 1000) /
			(DeltaPortCounters.PortXmitData + DeltaPortCounters.PortXmitTimeCong) : 0) +
		(DeltaPortCounters.PortRcvPkts ? (DeltaPortCounters.PortRcvBECN *
			pm->congestionWeights.PortRcvBECN * 1000 *
			(pmportp->pmnodep->nodeType == STL_NODE_FI ? 1 : 0) ) /
			(DeltaPortCounters.PortRcvPkts) : 0) +
		(DeltaPortCounters.PortXmitPkts ? (DeltaPortCounters.PortMarkFECN *
			pm->congestionWeights.PortMarkFECN * 1000) /
			(DeltaPortCounters.PortXmitPkts) : 0) +
		(DeltaPortCounters.SwPortCongestion * pm->congestionWeights.SwPortCongestion) );

	// Bubble uses MAX between PortXmitWastedBW + PortXmitWaitData and neighbor's PortRcvBubble
        uint64 PortXmitBubble = DeltaPortCounters.PortXmitWastedBW + DeltaPortCounters.PortXmitWaitData;
        UPDATE_MAX(portImage->Errors.Bubble,
            (uint32)(PortXmitBubble ? (PortXmitBubble * 10000) /
                (DeltaPortCounters.PortXmitData + PortXmitBubble): 0) );

	portImage->Errors.Security += (uint32)(DeltaPortCounters.PortXmitConstraintErrors);

	// There is no neighbor counter associated with routing.
	portImage->Errors.Routing = (uint32)DeltaPortCounters.PortRcvSwitchRelayErrors;

	if (pm->flags & STL_PM_PROCESS_VL_COUNTERS) {
		// SmaCongestion uses congestion weigting on VL15 counters associated with congestion
		portImage->Errors.SmaCongestion += (uint32)(
			(DeltaVLCounters[15].PortVLXmitWait ? (DeltaVLCounters[15].PortVLXmitWait *
				pm->congestionWeights.PortXmitWait * 10000) /
				(DeltaVLCounters[15].PortVLXmitData + DeltaVLCounters[15].PortVLXmitWait) : 0) +
			(DeltaVLCounters[15].PortVLXmitTimeCong ? (DeltaVLCounters[15].PortVLXmitTimeCong *
				pm->congestionWeights.PortXmitTimeCong * 1000) /
				(DeltaVLCounters[15].PortVLXmitData + DeltaVLCounters[15].PortVLXmitTimeCong) : 0) +
			(DeltaVLCounters[15].PortVLRcvPkts ? (DeltaVLCounters[15].PortVLRcvBECN	*
				pm->congestionWeights.PortRcvBECN * 1000 *
				(pmportp->pmnodep->nodeType == STL_NODE_FI ? 1 : 0) ) /
				(DeltaVLCounters[15].PortVLRcvPkts) : 0) +
			(DeltaVLCounters[15].PortVLXmitPkts ? (DeltaVLCounters[15].PortVLMarkFECN *
				pm->congestionWeights.PortMarkFECN * 1000) /
				(DeltaVLCounters[15].PortVLXmitPkts) : 0) +
			(DeltaVLCounters[15].SwPortVLCongestion * pm->congestionWeights.SwPortCongestion) );

		// Calculate VF/VL Utilization and Errors
		for (i = 0; i < portImage->numVFs; i++) {
			int j;
			for (j = 0; j < MAX_PM_VLS; j++) {
				if (portImage->vfvlmap[i].vlmask & (1 << j)) {
					// Utilization
					UPDATE_MAX(portImage->VFSendMBps[i],
                                            (uint32)(DeltaVLCounters[j].PortVLXmitData / FLITS_PER_MB / pm->interval));
					UPDATE_MAX(portImage->VFSendKPps[i],
                                            (uint32)(DeltaVLCounters[j].PortVLXmitPkts / 1000 / pm->interval));

					// Errors
					//portImage->VFErrors[i].Integrity = 0;   // no contributing error counters

					portImage->VFErrors[i].Congestion += (uint32)(
						(DeltaVLCounters[j].PortVLXmitWait ? (DeltaVLCounters[j].PortVLXmitWait *
							pm->congestionWeights.PortXmitWait * 10000) /
							(DeltaVLCounters[j].PortVLXmitData + DeltaVLCounters[j].PortVLXmitWait) : 0) +
						(DeltaVLCounters[j].PortVLXmitTimeCong ? (DeltaVLCounters[j].PortVLXmitTimeCong *
							pm->congestionWeights.PortXmitTimeCong * 1000) /
							(DeltaVLCounters[j].PortVLXmitData + DeltaVLCounters[j].PortVLXmitTimeCong) : 0) +
						(DeltaVLCounters[j].PortVLRcvPkts ? (DeltaVLCounters[j].PortVLRcvBECN *
							pm->congestionWeights.PortRcvBECN * 1000 *
							(pmportp->pmnodep->nodeType == STL_NODE_FI ? 1 : 0) ) /
							(DeltaVLCounters[j].PortVLRcvPkts) : 0) +
						(DeltaVLCounters[j].PortVLXmitPkts ? (DeltaVLCounters[j].PortVLMarkFECN *
							pm->congestionWeights.PortMarkFECN * 1000) /
							(DeltaVLCounters[j].PortVLXmitPkts) : 0) +
						(DeltaVLCounters[j].SwPortVLCongestion * pm->congestionWeights.SwPortCongestion) );

					if (j == 15) {
						// Use Port SmaCongestion only for VFs that include VL15
						UPDATE_MAX(portImage->VFErrors[i].SmaCongestion, portImage->Errors.SmaCongestion);
					}

					uint64 PortVLXmitBubble = DeltaVLCounters[j].PortVLXmitWastedBW + DeltaVLCounters[j].PortVLXmitWaitData;
					UPDATE_MAX(portImage->VFErrors[i].Bubble, (uint32)(PortVLXmitBubble ?
						(PortVLXmitBubble * 10000) / (DeltaVLCounters[j].PortVLXmitData + PortVLXmitBubble) : 0) );

					//portImage->VFErrors[i].Security = 0;    // no contributing error counters

					//portImage->VFErrors[i].Routing = 0;     // no contributing error counters
				}
			}
		}
	}

	// Calculate Neighbor's Utilization and Errors using receive side counters
	if (portImageNeighbor) {

		// Calulate Port Utilization
		UPDATE_MAX(portImageNeighbor->SendMBps, (uint32)(DeltaPortCounters.PortRcvData / FLITS_PER_MB / pm->interval));
		UPDATE_MAX(portImageNeighbor->SendKPps, (uint32)(DeltaPortCounters.PortRcvPkts / 1000 / pm->interval));

		portImageNeighbor->Errors.Integrity += (uint32)(
			DeltaPortCounters.ExcessiveBufferOverruns	* pm->integrityWeights.ExcessiveBufferOverruns);

		portImageNeighbor->Errors.Congestion += (uint32)(DeltaPortCounters.PortXmitPkts ?
			(DeltaPortCounters.PortRcvFECN * pm->congestionWeights.PortRcvFECN * 1000) /
				DeltaPortCounters.PortXmitPkts : 0);

		// Bubble uses MAX between PortXmitWastedBW + PortXmitWaitData and neighbor's PortRcvBubble
		UPDATE_MAX(portImageNeighbor->Errors.Bubble, (uint32)(DeltaPortCounters.PortRcvBubble ?
			(DeltaPortCounters.PortRcvBubble * 10000) /
			(DeltaPortCounters.PortRcvData + DeltaPortCounters.PortRcvBubble): 0) );

		//portImage2->Errors.Routing += 0;		// no contributing error counters

		portImageNeighbor->Errors.Security += (uint32)(DeltaPortCounters.PortRcvConstraintErrors);

		if (pm->flags & STL_PM_PROCESS_VL_COUNTERS) {
			// SmaCongestion uses congestion weigting on VL15 counters associated with congestion
			portImageNeighbor->Errors.SmaCongestion += (uint32)(DeltaVLCounters[15].PortVLRcvPkts ?
				(DeltaVLCounters[15].PortVLRcvFECN * pm->congestionWeights.PortRcvFECN * 1000) /
					DeltaVLCounters[15].PortVLRcvPkts : 0);

			// Calculate VF/VL Utilization and Errors
			for (i = 0; i < portImageNeighbor->numVFs; i++) {
				int j;
				for (j = 0; j < MAX_PM_VLS; j++) {
					if (portImageNeighbor->vfvlmap[i].vlmask & (1 << j)) {
						// Utilization
						UPDATE_MAX(portImageNeighbor->VFSendMBps[i],
                                                    (uint32)(DeltaVLCounters[j].PortVLRcvData / FLITS_PER_MB / pm->interval) );
						UPDATE_MAX(portImageNeighbor->VFSendKPps[i],
                                                    (uint32)(DeltaVLCounters[j].PortVLRcvPkts / 1000 / pm->interval) );

						// Errors
						//portImage2->VFErrors[i].Integrity = 0;	// no contributing error counters

						portImageNeighbor->VFErrors[i].Congestion += (uint32)(DeltaVLCounters[j].PortVLRcvPkts ?
							(DeltaVLCounters[j].PortVLRcvFECN * pm->congestionWeights.PortRcvFECN * 1000) /
								DeltaVLCounters[j].PortVLRcvPkts : 0);
						if (j == 15) {
							// Use Port SmaCongestion only for VFs that include VL15
							UPDATE_MAX(portImageNeighbor->VFErrors[i].SmaCongestion, portImageNeighbor->Errors.SmaCongestion);
						}

						UPDATE_MAX(portImageNeighbor->VFErrors[i].Bubble, (uint32)(DeltaVLCounters[j].PortVLRcvBubble ?
							(DeltaVLCounters[j].PortVLRcvBubble * 10000) /
							(DeltaVLCounters[j].PortVLRcvData + DeltaVLCounters[j].PortVLRcvBubble): 0) );

						//portImage2->VFErrors[i].Security = 0;		// no contributing error counters

						//portImage2->VFErrors[i].Routing = 0;		// no contributing error counters

					}
				}
			}
		}

		// Calculation of Pct10 counters for remote port
		uint32 rate = PmCalculateRate(portImageNeighbor->u.s.activeSpeed, portImageNeighbor->u.s.rxActiveWidth);
#define OVERFLOW_CHECK_MAX_PCT10(pct10, value) UPDATE_MAX(portImageNeighbor->Errors.pct10, (value > IB_UINT16_MAX ? IB_UINT16_MAX : (uint16)(value)))

		OVERFLOW_CHECK_MAX_PCT10(UtilizationPct10, (uint32)(
			(DeltaPortCounters.PortRcvData * 1000) /
			(s_StaticRateToMBps[rate] * FLITS_PER_MB * pm->interval)) );

#undef OVERFLOW_CHECK_MAX_PCT10
	}	// End of 'if (pmportp2) {'

	// Calculation of Pct10 counters for local port
	uint32 rate = PmCalculateRate(portImage->u.s.activeSpeed, portImage->u.s.txActiveWidth);

#define OVERFLOW_CHECK_SET_PCT10(pct10, value) portImage->Errors.pct10 = (value > IB_UINT16_MAX ? IB_UINT16_MAX : (uint16)(value))
#define OVERFLOW_CHECK_MAX_PCT10(pct10, value) UPDATE_MAX(portImage->Errors.pct10, (value > IB_UINT16_MAX ? IB_UINT16_MAX : (uint16)(value)))

	OVERFLOW_CHECK_MAX_PCT10(UtilizationPct10, (uint32)(
		(DeltaPortCounters.PortXmitData * 1000) /
		(s_StaticRateToMBps[rate] * FLITS_PER_MB * pm->interval)) );

	OVERFLOW_CHECK_SET_PCT10(DiscardsPct10, (uint32)(
		(DeltaPortCounters.PortXmitPkts + DeltaPortCounters.PortXmitDiscards) ?
			(DeltaPortCounters.PortXmitDiscards * 1000) /
			(DeltaPortCounters.PortXmitPkts + DeltaPortCounters.PortXmitDiscards) : 0) );

#undef OVERFLOW_CHECK_SET_PCT10
#undef OVERFLOW_CHECK_MAX_PCT10

#define INC_RUNNING(cntr, max) do { \
		if (pRunning->cntr >= (max - DeltaPortCounters.cntr)) { \
			pRunning->cntr = max; \
		} else { \
			pRunning->cntr += DeltaPortCounters.cntr; \
		} } while (0)

	// running totals for this port
	INC_RUNNING(PortXmitWait, IB_UINT64_MAX);
	INC_RUNNING(PortXmitData, IB_UINT64_MAX);
	INC_RUNNING(PortRcvData, IB_UINT64_MAX);
	INC_RUNNING(PortXmitPkts, IB_UINT64_MAX);
	INC_RUNNING(PortRcvPkts, IB_UINT64_MAX);
	INC_RUNNING(PortMulticastXmitPkts, IB_UINT64_MAX);
	INC_RUNNING(PortMulticastRcvPkts, IB_UINT64_MAX);
	INC_RUNNING(SwPortCongestion, IB_UINT64_MAX);
	INC_RUNNING(PortRcvFECN, IB_UINT64_MAX);
	INC_RUNNING(PortRcvBECN, IB_UINT64_MAX);
	INC_RUNNING(PortXmitTimeCong, IB_UINT64_MAX);
	INC_RUNNING(PortXmitWastedBW, IB_UINT64_MAX);
	INC_RUNNING(PortXmitWaitData, IB_UINT64_MAX);
	INC_RUNNING(PortRcvBubble, IB_UINT64_MAX);
	INC_RUNNING(PortMarkFECN, IB_UINT64_MAX);
	if(portImage->u.s.gotErrorCntrs) {
		INC_RUNNING(PortXmitDiscards, IB_UINT64_MAX);
		INC_RUNNING(PortXmitConstraintErrors, IB_UINT64_MAX);
		INC_RUNNING(PortRcvConstraintErrors, IB_UINT64_MAX);
		INC_RUNNING(PortRcvSwitchRelayErrors, IB_UINT64_MAX);
		INC_RUNNING(PortRcvRemotePhysicalErrors, IB_UINT64_MAX);
		INC_RUNNING(LocalLinkIntegrityErrors, IB_UINT64_MAX);
		INC_RUNNING(PortRcvErrors, IB_UINT64_MAX);
		INC_RUNNING(ExcessiveBufferOverruns, IB_UINT64_MAX);
		INC_RUNNING(FMConfigErrors, IB_UINT64_MAX);

		INC_RUNNING(LinkErrorRecovery, IB_UINT32_MAX);
		INC_RUNNING(LinkDowned, IB_UINT32_MAX);
		INC_RUNNING(UncorrectableErrors, IB_UINT8_MAX);
	}
#undef INC_RUNNING
	pRunning->lq.s.LinkQualityIndicator = pImgPortCounters->lq.s.LinkQualityIndicator;
	pRunning->lq.s.NumLanesDown = pImgPortCounters->lq.s.NumLanesDown;

	if (pm_config.process_vl_counters) {
#define INC_VLRUNNING(vlcntr, vl, max) do { \
		if (pVLRunning[vl].vlcntr >= (max - DeltaVLCounters[vl].vlcntr)) { \
			pVLRunning[vl].vlcntr = max; \
		} else { \
			pVLRunning[vl].vlcntr += DeltaVLCounters[vl].vlcntr; \
		} } while (0)
		for (i = 0; i < MAX_PM_VLS; i++) {
			INC_VLRUNNING(PortVLXmitData,	  i, IB_UINT64_MAX);
			INC_VLRUNNING(PortVLRcvData,	  i, IB_UINT64_MAX);
			INC_VLRUNNING(PortVLXmitPkts,	  i, IB_UINT64_MAX);
			INC_VLRUNNING(PortVLRcvPkts,	  i, IB_UINT64_MAX);
			INC_VLRUNNING(PortVLXmitWait,	  i, IB_UINT64_MAX);
			INC_VLRUNNING(SwPortVLCongestion, i, IB_UINT64_MAX);
			INC_VLRUNNING(PortVLRcvFECN,	  i, IB_UINT64_MAX);
			INC_VLRUNNING(PortVLRcvBECN,	  i, IB_UINT64_MAX);
			INC_VLRUNNING(PortVLXmitTimeCong, i, IB_UINT64_MAX);
			INC_VLRUNNING(PortVLXmitWastedBW, i, IB_UINT64_MAX);
			INC_VLRUNNING(PortVLXmitWaitData, i, IB_UINT64_MAX);
			INC_VLRUNNING(PortVLRcvBubble,	  i, IB_UINT64_MAX);
			INC_VLRUNNING(PortVLMarkFECN,	  i, IB_UINT64_MAX);
			if(portImage->u.s.gotErrorCntrs) {
				INC_VLRUNNING(PortVLXmitDiscards, i, IB_UINT64_MAX);
			}
		}
#undef INC_VLRUNNING
	}
}

// for a port clear counters which can tabulate information from both
// sides of a link
void PmClearPortImage(PmPortImage_t *portImage)
{
	portImage->u.s.bucketComputed = 0;
	portImage->u.s.queryStatus = PM_QUERY_STATUS_OK;
	portImage->u.s.UnexpectedClear = 0;
	portImage->u.s.gotDataCntrs = 0;
	portImage->u.s.gotErrorCntrs = 0;
	portImage->u.s.ClearAll = 0;
	portImage->u.s.ClearSome = 0;
	portImage->SendMBps = 0;
	portImage->SendKPps = 0;

	memset(&portImage->Errors, 0, sizeof(ErrorSummary_t));
	memset(portImage->VFSendMBps, 0, sizeof(uint32) * MAX_VFABRICS);
	memset(portImage->VFSendKPps, 0, sizeof(uint32) * MAX_VFABRICS);
	memset(portImage->VFErrors, 0, sizeof(ErrorSummary_t) * MAX_VFABRICS);
}

// Returns TRUE if need to Clear some counters for Port, FALSE if not
// TRUE means *counterSelect indicates counters to clear
// FALSE means no counters need to be cleared and *counterSelect is 0
//
// When possible we will return the same counterSelect for all switch ports.
// Caller can use AllPortSelect if all active ports need to be cleared with the
// same counterSelect.
//
// caller must have imageLock held

boolean PmTabulatePort(Pm_t *pm, PmPort_t *pmportp, uint32 imageIndex, uint32 *counterSelect) {

	PmPortImage_t *portImage = &pmportp->Image[imageIndex];
	PmCompositePortCounters_t *pImgPortCounters = &portImage->StlPortCounters;

	// Thresholds are calulated base upon MAX_UINT## * (ErrorClear/8)
	*counterSelect = 0;
	if (pm->clearCounterSelect.AsReg32) {
		CounterSelectMask_t select;

		select.AsReg32 = 0;
#define IF_EXCEED_CLEARTHRESHOLD(cntr) \
		do { if (IB_EXPECT_FALSE(pImgPortCounters->cntr \
									 > pm->ClearThresholds.cntr)) \
				select.s.cntr = pm->clearCounterSelect.s.cntr; } while (0)

		IF_EXCEED_CLEARTHRESHOLD(PortXmitData);
		IF_EXCEED_CLEARTHRESHOLD(PortRcvData);
		IF_EXCEED_CLEARTHRESHOLD(PortXmitPkts);
		IF_EXCEED_CLEARTHRESHOLD(PortRcvPkts);
		IF_EXCEED_CLEARTHRESHOLD(PortMulticastXmitPkts);
		IF_EXCEED_CLEARTHRESHOLD(PortMulticastRcvPkts);
		IF_EXCEED_CLEARTHRESHOLD(PortXmitWait);
		IF_EXCEED_CLEARTHRESHOLD(SwPortCongestion);
		IF_EXCEED_CLEARTHRESHOLD(PortRcvFECN);
		IF_EXCEED_CLEARTHRESHOLD(PortRcvBECN);
		IF_EXCEED_CLEARTHRESHOLD(PortXmitTimeCong);
		IF_EXCEED_CLEARTHRESHOLD(PortXmitWastedBW);
		IF_EXCEED_CLEARTHRESHOLD(PortXmitWaitData);
		IF_EXCEED_CLEARTHRESHOLD(PortRcvBubble);
		IF_EXCEED_CLEARTHRESHOLD(PortMarkFECN);
		IF_EXCEED_CLEARTHRESHOLD(PortRcvConstraintErrors);
		IF_EXCEED_CLEARTHRESHOLD(PortRcvSwitchRelayErrors);
		IF_EXCEED_CLEARTHRESHOLD(PortXmitDiscards);
		IF_EXCEED_CLEARTHRESHOLD(PortXmitConstraintErrors);
		IF_EXCEED_CLEARTHRESHOLD(PortRcvRemotePhysicalErrors);
		IF_EXCEED_CLEARTHRESHOLD(LocalLinkIntegrityErrors);
		IF_EXCEED_CLEARTHRESHOLD(PortRcvErrors);
		IF_EXCEED_CLEARTHRESHOLD(ExcessiveBufferOverruns);
		IF_EXCEED_CLEARTHRESHOLD(FMConfigErrors);
		IF_EXCEED_CLEARTHRESHOLD(LinkErrorRecovery);
		IF_EXCEED_CLEARTHRESHOLD(LinkDowned);
		IF_EXCEED_CLEARTHRESHOLD(UncorrectableErrors);
#undef IF_EXCEED_CLEARTHRESHOLD

		portImage->u.s.ClearSome = (select.AsReg32 ? 1 : 0);
		portImage->u.s.ClearAll = (select.AsReg32 == pm->clearCounterSelect.AsReg32 ? 1 : 0);
		*counterSelect |= select.AsReg32;
	}
	portImage->clearSelectMask.AsReg32 = *counterSelect;

	return (*counterSelect != 0);
} // End of PmTabulatePort
 
// build counter select to use when clearing counters
void PM_BuildClearCounterSelect(CounterSelectMask_t *select, boolean clearXfer, boolean clear64bit, boolean clear32bit, boolean clear8bit)
{
	// Set CounterSelect for use during Clear of counters.

	// Data Xfer Counters - Do not check for clear
	select->s.PortXmitData = clearXfer;
	select->s.PortRcvData = clearXfer;
	select->s.PortXmitPkts = clearXfer;
	select->s.PortRcvPkts = clearXfer;
	select->s.PortMulticastXmitPkts = clearXfer;
	select->s.PortMulticastRcvPkts = clearXfer;

	// Error Counters - 64-bit
	select->s.PortXmitWait = clear64bit;
	select->s.SwPortCongestion = clear64bit;
	select->s.PortRcvFECN = clear64bit;
	select->s.PortRcvBECN = clear64bit;
	select->s.PortXmitTimeCong = clear64bit;
	select->s.PortXmitWastedBW = clear64bit;
	select->s.PortXmitWaitData = clear64bit;
	select->s.PortRcvBubble = clear64bit;
	select->s.PortMarkFECN = clear64bit;
	select->s.PortRcvConstraintErrors = clear64bit;
	select->s.PortRcvSwitchRelayErrors = clear64bit;
	select->s.PortXmitDiscards = clear64bit;
	select->s.PortXmitConstraintErrors = clear64bit;
	select->s.PortRcvRemotePhysicalErrors = clear64bit;
	select->s.LocalLinkIntegrityErrors = clear64bit;
	select->s.PortRcvErrors = clear64bit;
	select->s.ExcessiveBufferOverruns = clear64bit;
	select->s.FMConfigErrors = clear64bit;

	// Error Counters - 32-bit
	select->s.LinkErrorRecovery = clear32bit;
	select->s.LinkDowned = clear32bit;

	// Error Counters - 8-bit
	select->s.UncorrectableErrors = clear8bit;
}

// compute reasonable clearThresholds based on given threshold and weights
// This can be used to initialize clearThreshold and then override just
// a few of the computed defaults in the even user wanted to control just a few
// and default the rest
// used during startup, no lock needed
void PmComputeClearThresholds(PmCompositePortCounters_t *clearThresholds, CounterSelectMask_t *select, uint8 errorClear)
{
	if (errorClear > 7)  errorClear = 7;

	MemoryClear(clearThresholds, sizeof(clearThresholds));	// be safe

#define COMPUTE_THRESHOLD(counter, max) do { \
		clearThresholds->counter = (max/8)*(select->s.counter?errorClear:8); \
		} while (0)

	COMPUTE_THRESHOLD(PortXmitData, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortRcvData, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortXmitPkts, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortRcvPkts, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortMulticastXmitPkts, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortMulticastRcvPkts, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortXmitWait, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(SwPortCongestion, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortRcvFECN, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortRcvBECN, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortXmitTimeCong, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortXmitWastedBW, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortXmitWaitData, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortRcvBubble, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortMarkFECN, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortRcvConstraintErrors, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortRcvSwitchRelayErrors, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortXmitDiscards, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortXmitConstraintErrors, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortRcvRemotePhysicalErrors, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(LocalLinkIntegrityErrors, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(PortRcvErrors, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(ExcessiveBufferOverruns, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(FMConfigErrors, IB_UINT64_MAX);
	COMPUTE_THRESHOLD(LinkErrorRecovery, IB_UINT32_MAX);
	COMPUTE_THRESHOLD(LinkDowned, IB_UINT32_MAX);
	COMPUTE_THRESHOLD(UncorrectableErrors, IB_UINT8_MAX);
#undef COMPUTE_THRESHOLD
}

// Clear Running totals for a given Port.  This simulates a PMA clear so
// that tools like opareport can work against the Running totals until we
// have a history feature.  Only counters selected are cleared.
// caller must have totalsLock held for write and imageLock held for read
FSTATUS PmClearPortRunningCounters(PmPort_t *pmportp, CounterSelectMask_t select)
{
	PmCompositePortCounters_t *pRunning;

	pRunning = &pmportp->StlPortCountersTotal;
#define CLEAR_SELECTED(counter) \
		do { if (select.s.counter) pRunning->counter = 0; } while (0)

	CLEAR_SELECTED(PortXmitData);
	CLEAR_SELECTED(PortRcvData);
	CLEAR_SELECTED(PortXmitPkts);
	CLEAR_SELECTED(PortRcvPkts);
	CLEAR_SELECTED(PortMulticastXmitPkts);
	CLEAR_SELECTED(PortMulticastRcvPkts);
	CLEAR_SELECTED(PortXmitWait);
	CLEAR_SELECTED(SwPortCongestion);
	CLEAR_SELECTED(PortRcvFECN);
	CLEAR_SELECTED(PortRcvBECN);
	CLEAR_SELECTED(PortXmitTimeCong);
	CLEAR_SELECTED(PortXmitWastedBW);
	CLEAR_SELECTED(PortXmitWaitData);
	CLEAR_SELECTED(PortRcvBubble);
	CLEAR_SELECTED(PortMarkFECN);
	CLEAR_SELECTED(PortRcvConstraintErrors);
	CLEAR_SELECTED(PortRcvSwitchRelayErrors);
	CLEAR_SELECTED(PortXmitDiscards);
	CLEAR_SELECTED(PortXmitConstraintErrors);
	CLEAR_SELECTED(PortRcvRemotePhysicalErrors);
	CLEAR_SELECTED(LocalLinkIntegrityErrors);
	CLEAR_SELECTED(PortRcvErrors);
	CLEAR_SELECTED(ExcessiveBufferOverruns);
	CLEAR_SELECTED(FMConfigErrors);
	CLEAR_SELECTED(LinkErrorRecovery);
	CLEAR_SELECTED(LinkDowned);
	CLEAR_SELECTED(UncorrectableErrors);
	pRunning->lq.s.LinkQualityIndicator = STL_LINKQUALITY_EXCELLENT;
#undef CLEAR_SELECTED

	return FSUCCESS;
}

// Clear Running totals for a given Node.  This simulates a PMA clear so
// that tools like opareport can work against the Running totals until we
// have a history feature.
// caller must have totalsLock held for write and imageLock held for read
FSTATUS PmClearNodeRunningCounters(PmNode_t *pmnodep, CounterSelectMask_t select)
{
	FSTATUS status = FSUCCESS;
	if (pmnodep->nodeType == STL_NODE_SW) {
		uint8_t i;

		for (i=0; i<=pmnodep->numPorts; ++i) {
			PmPort_t *pmportp = pmnodep->up.swPorts[i];
			if (pmportp)
				status = PmClearPortRunningCounters(pmportp, select);
			if (IB_EXPECT_FALSE(status != FSUCCESS)){
				IB_LOG_WARN_FMT(__func__,"Failed to Clear Counters on Port: %u", i);
				break;
			}
		}
		return status;
	} else {
		return PmClearPortRunningCounters(pmnodep->up.caPortp, select);
	}
}

FSTATUS PmClearPortRunningVFCounters(PmPort_t *pmportp, 
					STLVlCounterSelectMask select, char *vfName)
{
	PmCompositeVLCounters_t *pVLRunning;
	PmPortImage_t *portImage = &pmportp->Image[g_pmSweepData.SweepIndex];
	int vl = 0;
	boolean useHiddenVF = !strcmp(HIDDEN_VL15_VF, vfName);
	int i = (useHiddenVF ? -1 : 0);
	FSTATUS status = FNOT_FOUND | STL_MAD_STATUS_STL_PA_NO_VF;

	pVLRunning = &pmportp->StlVLPortCountersTotal[0];

#define CLEAR_SELECTED(counter) \
	do { if (select.s.counter) pVLRunning[vl].counter = 0; } while (0)

	for (; i < (int)portImage->numVFs; i++) {
		if ((i == -1) || (portImage->vfvlmap[i].pVF && !strcmp(portImage->vfvlmap[i].pVF->Name, vfName)) ) {
			uint32 vlmask = (i == -1) ? 0x8000 : portImage->vfvlmap[i].vlmask;
			for (vl = 0; vl < MAX_PM_VLS && vlmask; vl++, vlmask >>= 1) {
				if ((vlmask & 0x1) == 0) continue;
				CLEAR_SELECTED(PortVLXmitData);
				CLEAR_SELECTED(PortVLRcvData);
				CLEAR_SELECTED(PortVLXmitPkts);
				CLEAR_SELECTED(PortVLRcvPkts);
				CLEAR_SELECTED(PortVLXmitDiscards);
				CLEAR_SELECTED(SwPortVLCongestion);
				CLEAR_SELECTED(PortVLXmitWait);
				CLEAR_SELECTED(PortVLRcvFECN);
				CLEAR_SELECTED(PortVLRcvBECN);
				CLEAR_SELECTED(PortVLXmitTimeCong);
				CLEAR_SELECTED(PortVLXmitWastedBW);
				CLEAR_SELECTED(PortVLXmitWaitData);
				CLEAR_SELECTED(PortVLRcvBubble);
				CLEAR_SELECTED(PortVLMarkFECN);
				status = FSUCCESS;
			}
			break;
		}
	}
#undef CLEAR_SELECTED
	return status;
}

// Clear Running totals for a given Node.  This simulates a PMA clear so
// that tools like opareport can work against the Running totals until we
// have a history feature.
// caller must have totalsLock held for write and imageLock held for read
FSTATUS PmClearNodeRunningVFCounters(PmNode_t *pmnodep,
					STLVlCounterSelectMask select, char *vfName)
{
	FSTATUS status = FNOT_FOUND | STL_MAD_STATUS_STL_PA_NO_PORT;
	if (pmnodep->nodeType == STL_NODE_SW) {
		uint8_t i;

		for (i=0; i<=pmnodep->numPorts; ++i) {
			PmPort_t *pmportp = pmnodep->up.swPorts[i];
			if (pmportp)
				status |= PmClearPortRunningVFCounters(pmportp, select, vfName);
		}
		return status;
	} else {
		return PmClearPortRunningVFCounters(pmnodep->up.caPortp, select, vfName);
	}
}
