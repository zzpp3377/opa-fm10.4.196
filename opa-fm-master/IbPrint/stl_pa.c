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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <iba/ibt.h>
#include <iba/stl_pa.h>
#include <iba/ib_helper.h>
#include <stdarg.h>
#include <time.h>
#include "ibprint.h"

void PrintStlPAImageId(PrintDest_t *dest, int indent, const STL_PA_IMAGE_ID_DATA *pImageId)
{
	time_t absTime = (time_t)pImageId->imageTime.absoluteTime;

	PrintFunc(dest, "%*sImageNumber: 0x%"PRIx64" Offset: %d\n",
			indent, "", pImageId->imageNumber, pImageId->imageOffset);
	if (absTime) {
		char buf[80];
		snprintf(buf, sizeof(buf), "%s", ctime((const time_t *)&absTime));
		if ((strlen(buf)>0) && (buf[strlen(buf)-1] == '\n'))
			buf[strlen(buf)-1] = '\0';

		PrintFunc(dest, "%*sImageTime: %s\n", indent, "", buf);
	}
	return;
}

void PrintStlPAGroupList(PrintDest_t *dest, int indent, const int numRecords, const STL_PA_GROUP_LIST *pGroupList)
{
	int i;

	PrintFunc(dest, "%*sNumber of Groups: %u\n", indent, "", numRecords);
	for (i = 0; i < numRecords; i++)
		PrintFunc(dest, "%*sGroup %u: %s\n", indent, "", i+1, pGroupList[i].groupName);
	return;
}

void PrintStlPAGroupUtilStats(PrintDest_t *dest, int indent, const STL_PA_PM_UTIL_STATS *pUtilStat)
{
	int i;

	PrintFunc(dest, "%*sUtil: Tot %6"PRIu64" Max %6u Min %6u Avg %6u MiB/s\n",
				indent, "", pUtilStat->totalMBps, pUtilStat->maxMBps, pUtilStat->minMBps, pUtilStat->avgMBps);
	PrintFunc(dest, "%*sUtil: ", indent, "");
	for (i = 0; i < STL_PM_UTIL_BUCKETS; i++)
		PrintFunc(dest, " %4d", pUtilStat->BWBuckets[i]);
	PrintFunc(dest, "%*s\n", indent, "");
	PrintFunc(dest, "%*sPkts: Tot %6"PRIu64" Max %6u Min %6u Avg %6u KiPps/s\n",
				indent, "", pUtilStat->totalKPps, pUtilStat->maxKPps, pUtilStat->minKPps, pUtilStat->avgKPps);
	PrintFunc(dest, "%*s Failed Ports: PMA: %u  Topo: %u\n",
				indent, "", pUtilStat->pmaFailedPorts, pUtilStat->topoFailedPorts);

	return;
}

void PrintStlPAGroupErrorStats(PrintDest_t *dest, int indent, const STL_PMERRSTAT_T *pErrStat)
{
	int i;

	PrintFunc(dest, "%*sIntegrity     Max %6u     Buckets: ",
		indent, "", pErrStat->errorMaximums.integrityErrors);
	for (i = 0; i < PM_ERR_BUCKETS; i++)
		PrintFunc(dest, " %4u ", pErrStat->ports[i].integrityErrors);
	PrintFunc(dest, "\n");

	PrintFunc(dest, "%*sCongestion    Max %6u     Buckets: ",
		indent, "", pErrStat->errorMaximums.congestionErrors);
	for (i = 0; i < PM_ERR_BUCKETS; i++)
		PrintFunc(dest, " %4u ", pErrStat->ports[i].congestionErrors);
	PrintFunc(dest, "\n");

	PrintFunc(dest, "%*sSmaCongestion Max %6u     Buckets: ",
		indent, "", pErrStat->errorMaximums.smaCongestionErrors);
	for (i = 0; i < PM_ERR_BUCKETS; i++)
		PrintFunc(dest, " %4u ", pErrStat->ports[i].smaCongestionErrors);
	PrintFunc(dest, "\n");

	PrintFunc(dest, "%*sBubble        Max %6u     Buckets: ",
			  indent, "", pErrStat->errorMaximums.bubbleErrors);
	for (i = 0; i < PM_ERR_BUCKETS; i++)
		PrintFunc(dest, " %4u ", pErrStat->ports[i].bubbleErrors);
	PrintFunc(dest, "\n");

	PrintFunc(dest, "%*sSecurity      Max %6u     Buckets: ",
		indent, "", pErrStat->errorMaximums.securityErrors);
	for (i = 0; i < PM_ERR_BUCKETS; i++)
		PrintFunc(dest, " %4u ", pErrStat->ports[i].securityErrors);
	PrintFunc(dest, "\n");

	PrintFunc(dest, "%*sRouting       Max %6u     Buckets: ",
		indent, "", pErrStat->errorMaximums.routingErrors);
	for (i = 0; i < PM_ERR_BUCKETS; i++)
		PrintFunc(dest, " %4u ", pErrStat->ports[i].routingErrors);
	PrintFunc(dest, "\n");


	PrintFunc(dest, "%*sUtilization:    %3u.%1u%%\n", indent, "",
		   pErrStat->errorMaximums.utilizationPct10 / 10,
		   pErrStat->errorMaximums.utilizationPct10 % 10);
	PrintFunc(dest, "%*sDiscards:       %3u.%1u%%\n", indent, "",
		   pErrStat->errorMaximums.discardsPct10 / 10,
		   pErrStat->errorMaximums.discardsPct10 % 10);

	return;
}

void PrintStlPAGroupInfo(PrintDest_t *dest, int indent, const STL_PA_PM_GROUP_INFO_DATA *pGroupInfo)
{

	PrintFunc(dest, "%*sGroup name: %s\n",
				indent, "", pGroupInfo->groupName);
	if (pGroupInfo->minInternalRate != IB_STATIC_RATE_DONTCARE
				|| pGroupInfo->maxInternalRate != IB_STATIC_RATE_DONTCARE)
		PrintFunc(dest, "%*sNum internal ports: %u MinRate: %4s MaxRate: %4s MaxMiBps: %u\n",
				indent, "", pGroupInfo->numInternalPorts,
				StlStaticRateToText(pGroupInfo->minInternalRate),
				StlStaticRateToText(pGroupInfo->maxInternalRate),
				pGroupInfo->maxInternalMBps);
	else
		PrintFunc(dest, "%*sNum internal ports: %u\n",
				indent, "", pGroupInfo->numInternalPorts);

	if (pGroupInfo->minExternalRate != IB_STATIC_RATE_DONTCARE
				|| pGroupInfo->maxExternalRate != IB_STATIC_RATE_DONTCARE)
		PrintFunc(dest, "%*sNum external ports: %u MinRate: %4s MaxRate: %4s MaxMiBps: %u\n",
				indent, "", pGroupInfo->numExternalPorts,
				StlStaticRateToText(pGroupInfo->minExternalRate),
				StlStaticRateToText(pGroupInfo->maxExternalRate),
				pGroupInfo->maxExternalMBps);
	else
		PrintFunc(dest, "%*sNum external ports: %u\n",
				indent, "", pGroupInfo->numExternalPorts);

	PrintFunc(dest, "%*sInternal utilization statistics:\n",
				indent, "");
	PrintStlPAGroupUtilStats(dest, indent+2, &pGroupInfo->internalUtilStats);
	PrintFunc(dest, "%*sSend utilization statistics:\n",
				indent, "");
	PrintStlPAGroupUtilStats(dest, indent+2, &pGroupInfo->sendUtilStats);
	PrintFunc(dest, "%*sReceive utilization statistics:\n",
				indent, "");
	PrintStlPAGroupUtilStats(dest, indent+2, &pGroupInfo->recvUtilStats);
	PrintFunc(dest, "%*sInternal Error Summary:\n",
				indent, "");
	PrintStlPAGroupErrorStats(dest, indent+2, &pGroupInfo->internalErrors);
	PrintFunc(dest, "%*sExternal Error Summary:\n",
				indent, "");
	PrintStlPAGroupErrorStats(dest, indent+2, &pGroupInfo->externalErrors);
	PrintFunc(dest, "%*sImageID:\n",
				indent, "");
	PrintStlPAImageId(dest, indent+2, &pGroupInfo->imageId);
	return;
}

void PrintStlPAPortCounters(PrintDest_t *dest, int indent, const STL_PORT_COUNTERS_DATA *pPortCounters, const uint32 nodeLid, const uint32 portNumber, const uint32 flags)
{
	PrintFunc(dest, "%*s%s controlled Port Counters (%s) for LID 0x%04x, port number %u%s:\n",
				indent, "", (flags & STL_PA_PC_FLAG_USER_COUNTERS) ? "User" : "PM",
				(flags & STL_PA_PC_FLAG_DELTA) ? "delta" : "total",
			   	nodeLid, portNumber,
				(flags&STL_PA_PC_FLAG_UNEXPECTED_CLEAR)?" (Unexpected Clear)":"");
	PrintFunc(dest, "%*sPerformance: Transmit\n", indent, "");
	PrintFunc(dest, "%*s    Xmit Data             %20"PRIu64" MB (%"PRIu64" Flits)\n",
			indent, "",
 			pPortCounters->portXmitData/FLITS_PER_MB,
			pPortCounters->portXmitData);
	PrintFunc(dest, "%*s    Xmit Pkts             %20"PRIu64"\n",
			indent, "",
			pPortCounters->portXmitPkts);
	PrintFunc(dest, "%*s    MC Xmit Pkts          %20"PRIu64"\n",
			indent, "",
			pPortCounters->portMulticastXmitPkts);
	PrintFunc(dest, "%*sPerformance: Receive\n",
			indent, "");
	PrintFunc(dest, "%*s    Rcv Data              %20"PRIu64" MB (%"PRIu64" Flits)\n",
			indent, "",
 			pPortCounters->portRcvData/FLITS_PER_MB,
			pPortCounters->portRcvData);
	PrintFunc(dest, "%*s    Rcv Pkts              %20"PRIu64"\n",
			indent, "",
			pPortCounters->portRcvPkts);
	PrintFunc(dest, "%*s    MC Rcv Pkts           %20"PRIu64"\n",
			indent, "",
			pPortCounters->portMulticastRcvPkts);
	PrintFunc(dest, "%*sSignal Integrity Errors:             \n",
			indent, "");
	PrintFunc(dest, "%*s    Link Quality Ind      %10u\n",
			indent, "",
		   	pPortCounters->lq.s.linkQualityIndicator);
	PrintFunc(dest, "%*s    Uncorrectable Err     %10u\n", // 8-bit
			indent, "",
		   	pPortCounters->uncorrectableErrors);
	PrintFunc(dest, "%*s    Link Downed           %10u\n", // 32-bit
			indent, "",
		   	pPortCounters->linkDowned);
	PrintFunc(dest, "%*s    Num Lanes Down        %10u\n",
			indent, "",
			pPortCounters->lq.s.numLanesDown);
	PrintFunc(dest, "%*s    Rcv Errors            %10"PRIu64"\n",
			indent, "",
	   		pPortCounters->portRcvErrors);
	PrintFunc(dest, "%*s    Exc. Buffer Overrun   %10"PRIu64"\n",
			indent, "",
	   		pPortCounters->excessiveBufferOverruns);
	PrintFunc(dest, "%*s    FM Config             %10"PRIu64"\n",
			indent, "",
	   		pPortCounters->fmConfigErrors);
	PrintFunc(dest, "%*s    Link Error Recovery   %10u\n", // 32-bit
			indent, "",
		   	pPortCounters->linkErrorRecovery);
	PrintFunc(dest, "%*s    Local Link Integrity  %10"PRIu64"\n",
			indent, "",
	   		pPortCounters->localLinkIntegrityErrors);
	PrintFunc(dest, "%*s    Rcv Rmt Phys Err      %10"PRIu64"\n",
			indent, "",
	   		pPortCounters->portRcvRemotePhysicalErrors);
	PrintFunc(dest, "%*sSecurity Errors:              \n",
			indent, "");
	PrintFunc(dest, "%*s    Xmit Constraint       %10"PRIu64"\n",
			indent, "",
	   		pPortCounters->portXmitConstraintErrors);
	PrintFunc(dest, "%*s    Rcv Constraint        %10"PRIu64"\n",
			indent, "",
	   		pPortCounters->portRcvConstraintErrors);
	PrintFunc(dest, "%*sRouting and Other Errors:     \n",
			indent, "");
	PrintFunc(dest, "%*s    Rcv Sw Relay Err      %10"PRIu64"\n",
			indent, "",
	   		pPortCounters->portRcvSwitchRelayErrors);
	PrintFunc(dest, "%*s    Xmit Discards         %10"PRIu64"\n",
			indent, "",
	   		pPortCounters->portXmitDiscards);
	PrintFunc(dest, "%*sCongestion:             \n",
			indent, "");
	PrintFunc(dest, "%*s    Cong Discards         %10"PRIu64"\n",
			indent, "",
	   		pPortCounters->swPortCongestion);
	PrintFunc(dest, "%*s    Rcv FECN              %10"PRIu64"\n",
			indent, "",
		   	pPortCounters->portRcvFECN);
	PrintFunc(dest, "%*s    Rcv BECN              %10"PRIu64"\n",
			indent, "",
		   	pPortCounters->portRcvBECN);
	PrintFunc(dest, "%*s    Mark FECN             %10"PRIu64"\n",
			indent, "",
		   	pPortCounters->portMarkFECN);
	PrintFunc(dest, "%*s    Xmit Time Cong        %10"PRIu64"\n",
			indent, "",
		   	pPortCounters->portXmitTimeCong);
	PrintFunc(dest, "%*s    Xmit Wait             %10"PRIu64"\n",
			indent, "",
		   	pPortCounters->portXmitWait);
	PrintFunc(dest, "%*sBubbles:	             \n",
			indent, "");
	PrintFunc(dest, "%*s    Xmit Wasted BW        %10"PRIu64"\n",
			indent, "",
		   	pPortCounters->portXmitWastedBW);
	PrintFunc(dest, "%*s    Xmit Wait Data        %10"PRIu64"\n",
			indent, "",
		   	pPortCounters->portXmitWaitData);
	PrintFunc(dest, "%*s    Rcv Bubble            %10"PRIu64"\n",
			indent, "",
		   	pPortCounters->portRcvBubble);
	PrintStlPAImageId(dest, indent+2, &pPortCounters->imageId);
#if 0
	if (flags & STL_PA_PC_FLAG_DELTA) {
		if (pPortCounters->PortCheckRate) {
			PrintFunc(dest, "%*s    %s  %20"PRIu64" (%3u.%1u%%) (Inefficiency %3u.%1u%%)\n",
				indent, "",
		   		IbPortCongestionNameToText(pPortCounters->PortCheckRate),
				pPortCounters->PortXmitCongestion,
				pPortCounters->CongestionPct10/10,
				pPortCounters->CongestionPct10%10,
				pPortCounters->InefficiencyPct10/10,
				pPortCounters->InefficiencyPct10%10);
			PrintFunc(dest, "%*s    %-22s%10u %s\n",
				indent, "",
		   		IbPortCheckRateTypeToText(pPortCounters->PortCheckRate),
		   		pPortCounters->PortCheckRate & PM_VENDOR_PORT_COUNTERS_RATE_VALUE_MASK,
		   		IbPortCheckRateTypeSuffixToText(pPortCounters->PortCheckRate)
				);
		} else {
			PrintFunc(dest, "%*s    Port Xmit Congestion Unavailable\n",
				indent, "");
		}
		if (pPortCounters->PortCheckRate
			&& (pPortCounters->PortCheckRate & PM_VENDOR_PORT_COUNTERS_RATE_TYPE_MASK)
	   				== PM_VENDOR_PORT_COUNTERS_RATE_TYPE_CONG) {
			PrintFunc(dest, "%*s    Port Adaptive Routing %20"PRIu64"\n",
				indent, "",
				pPortCounters->PortAdaptiveRouting);
		} else {
			PrintFunc(dest, "%*s    Port Adaptive Routing        N/A\n",
				indent, "");
		}
	}
#endif
}

void PrintStlPAGroupConfig(PrintDest_t *dest, int indent, const char *groupName, const int numRecords, const STL_PA_PM_GROUP_CFG_RSP *pGroupConfig)
{
	int i;

	PrintFunc(dest, "%*sGroup name: %s\n",
				indent, "", groupName);
	PrintFunc(dest, "%*sNumber ports: %u\n",
				indent, "", numRecords);
	for (i = 0; i < numRecords; i++) {
		PrintFunc(dest, "%*s%u:LID:0x%04x Port:%u  GUID:0x%016"PRIx64"  NodeDesc: %.*s\n",
				indent, "", i+1, pGroupConfig[i].nodeLid, pGroupConfig[i].portNumber,
				pGroupConfig[i].nodeGUID,
			   	(int)sizeof(pGroupConfig[i].nodeDesc),
			   	pGroupConfig[i].nodeDesc);
	}
	PrintStlPAImageId(dest, indent, &pGroupConfig->imageId);

	return;
}

void PrintStlPMConfig(PrintDest_t *dest, int indent, const STL_PA_PM_CFG_DATA *pPMConfig)
{
	char buf[80];

	PrintFunc(dest, "%*sSweep Interval: %u sec  PM Flags (0x%X):\n",
			  indent, "", pPMConfig->sweepInterval, pPMConfig->pmFlags);
	StlFormatPmFlags(buf, pPMConfig->pmFlags);
	PrintFunc(dest, "%*s  %s\n", indent, "", buf);
	StlFormatPmFlags2(buf, pPMConfig->pmFlags);
	PrintFunc(dest, "%*s  %s\n", indent, "", buf);
	PrintFunc(dest, "%*sMax Clients: %u\n",
				indent, "", pPMConfig->maxClients);
	PrintFunc(dest, "%*sTotal Images: %-7u   Freeze Images: %-7u   Freeze Lease: %-7u sec\n",
				indent, "", pPMConfig->sizeHistory, pPMConfig->sizeFreeze,
				pPMConfig->lease);
	PrintFunc(dest, "%*sErr Thresholds: Integrity: %-7u        Congestion: %-7u\n",
				indent, "", pPMConfig->errorThresholds.integrityErrors,
				pPMConfig->errorThresholds.congestionErrors );
	PrintFunc(dest, "%*s                SMA Congest: %-7u      Bubble: %-7u\n",
				indent, "",pPMConfig->errorThresholds.smaCongestionErrors,
				pPMConfig->errorThresholds.bubbleErrors);
	PrintFunc(dest, "%*s                Security: %-7u         Routing: %-7u\n",
				indent, "", pPMConfig->errorThresholds.securityErrors,
				pPMConfig->errorThresholds.routingErrors );
	PrintFunc(dest, "%*s Integrity Wts: Lnk Wdth Dngd: %-7u    Link Qual: %-7u\n",
				indent, "", pPMConfig->integrityWeights.LinkWidthDowngrade,
				pPMConfig->integrityWeights.LinkQualityIndicator );
	PrintFunc(dest, "%*s                Uncorrectable: %-7u    Link Downed: %-7u\n",
				indent, "", pPMConfig->integrityWeights.UncorrectableErrors,
				pPMConfig->integrityWeights.LinkDowned);
	PrintFunc(dest, "%*s                Rcv Errors: %-7u       Excs Bfr Ovrn: %-7u\n",
				indent, "", pPMConfig->integrityWeights.PortRcvErrors,
				pPMConfig->integrityWeights.ExcessiveBufferOverruns);
	PrintFunc(dest, "%*s                FM Config Err: %-7u    Link Err Recov: %-7u\n",
			    indent, "", pPMConfig->integrityWeights.FMConfigErrors,
				pPMConfig->integrityWeights.LinkErrorRecovery);
	PrintFunc(dest, "%*s                Loc Link Integ: %-7u \n",
				indent, "", pPMConfig->integrityWeights.LocalLinkIntegrityErrors );
	PrintFunc(dest, "%*s Congest Wts:   Cong Discards: %-7u    Rcv FECN: %-7u\n",
				indent, "", pPMConfig->congestionWeights.SwPortCongestion,
				pPMConfig->congestionWeights.PortRcvFECN);
	PrintFunc(dest, "%*s                Rcv BECN: %-7u         Mark FECN: %-7u\n",
				indent, "", pPMConfig->congestionWeights.PortRcvBECN,
				pPMConfig->congestionWeights.PortMarkFECN);
	PrintFunc(dest, "%*s                Tx Time Cong: %-7u     Tx Wait: %-7u\n",
				indent, "", pPMConfig->congestionWeights.PortXmitTimeCong,
				pPMConfig->congestionWeights.PortXmitWait);
	PrintFunc(dest, "%*sPM Memory Size: %"PRIu64" MiB (%" PRIu64 " bytes)\n",
				indent, "", pPMConfig->memoryFootprint/(1024*1024),
				pPMConfig->memoryFootprint );
	PrintFunc(dest, "%*sPMA MADs: MaxAttempts: %-6u MinRespTimeout: %-6u RespTimeout: %-6u\n",
				indent, "", pPMConfig->maxAttempts, pPMConfig->minRespTimeout,
			   	pPMConfig->respTimeout );
	PrintFunc(dest, "%*sSweep: MaxParallelNodes: %-6u PmaBatchSize: %-6u ErrorClear: %1u\n",
				indent, "", pPMConfig->maxParallelNodes,
			   	pPMConfig->pmaBatchSize, pPMConfig->errorClear);

	return;
}

void PrintStlPAFocusPorts(PrintDest_t *dest, int indent, const char *groupName, const int numRecords, const uint32 select, const uint32 start, const uint32 range,
	const STL_FOCUS_PORTS_RSP *pFocusPorts)
{
	int i;

	PrintFunc(dest, "%*sGroup name: %s\n", indent, "", groupName);
	PrintFunc(dest, "%*sNumber links: %u\n", indent, "", numRecords);
	PrintFunc(dest, "%*sFocus select: 0x%x\n", indent, "", select);
	PrintFunc(dest, "%*sFocus start:  %u\n", indent, "", start);
	PrintFunc(dest, "%*sFocus range:  %u\n", indent, "", range);
	for (i = 0; i < numRecords; i++) {
		PrintFunc(dest, "%*s%u:LID:0x%04x  Port:%u  Rate: %4s MTU: %5s nbrLID:0x%04x  nbrPort:%u\n",
				indent, "", i+1, pFocusPorts[i].nodeLid, pFocusPorts[i].portNumber,
				StlStaticRateToText(pFocusPorts[i].rate), IbMTUToText(pFocusPorts[i].mtu),
				pFocusPorts[i].neighborLid, pFocusPorts[i].neighborPortNumber);
		PrintFunc(dest, "%*s   Value:  %16"PRIu64"   nbrValue:  %16"PRIu64"\n",
				indent, "", pFocusPorts[i].value, pFocusPorts[i].neighborValue);
		PrintFunc(dest, "%*s   GUID: 0x%016"PRIx64"   nbrGuid: 0x%016"PRIx64"\n",
				indent, "", pFocusPorts[i].nodeGUID, pFocusPorts[i].neighborGuid);
		PrintFunc(dest, "%*s   Status: %s Name: %.*s\n", indent, "",
				StlFocusFlagToText(pFocusPorts[i].localFlags),
				(int)sizeof(pFocusPorts[i].nodeDesc), pFocusPorts[i].nodeDesc);
		PrintFunc(dest, "%*s   Status: %s Neighbor Name: %.*s\n", indent, "",
				StlFocusFlagToText(pFocusPorts[i].neighborFlags),
				(int)sizeof(pFocusPorts[i].neighborNodeDesc), pFocusPorts[i].neighborNodeDesc);
	}
	PrintStlPAImageId(dest, indent, &pFocusPorts[0].imageId);

	return;
}

void PrintStlPAImageInfo(PrintDest_t *dest, int indent, const STL_PA_IMAGE_INFO_DATA *pImageInfo)
{
	int i;
	time_t sweepStart = (time_t)pImageInfo->sweepStart;
	char buf[80];

	ctime_r((const time_t *)&sweepStart, buf);
	if ((strlen(buf)>0) && (buf[strlen(buf)-1] == '\n'))
		buf[strlen(buf)-1] = '\0';
	PrintStlPAImageId(dest, indent, &pImageInfo->imageId);
	PrintFunc(dest, "%*sImageSweepStart: %s  Duration: %u.%03u Seconds\n",
				indent, "", buf,
				pImageInfo->sweepDuration/1000000,
				(pImageInfo->sweepDuration%1000000)/1000);
	if (pImageInfo->imageInterval) {
		PrintFunc(dest, "%*sImage Interval: %u Seconds\n",
					indent, "", pImageInfo->imageInterval);
	}
	PrintFunc(dest, "%*sNum SW Ports: %u  HFI Ports: %u\n",
				indent, "", pImageInfo->numSwitchPorts,
			   	pImageInfo->numHFIPorts);
	PrintFunc(dest, "%*sNum SWs: %u  Links: %u  SMs: %u\n",
				indent, "", pImageInfo->numSwitchNodes,
				pImageInfo->numLinks, pImageInfo->numSMs);
	PrintFunc(dest, "%*sNum Failed Nodes: %u  Failed Ports: %u  Unexpected Clear Ports: %u\n",
				indent, "", pImageInfo->numFailedNodes,
				pImageInfo->numFailedPorts,
			   	pImageInfo->numUnexpectedClearPorts);
	PrintFunc(dest, "%*sNum Skipped Nodes: %u  Skipped Ports: %u\n",
				indent, "", pImageInfo->numSkippedNodes,
				pImageInfo->numSkippedPorts);
	for (i = 0; i < 2; i++) {
		if (i != 0 && ! pImageInfo->SMInfo[i].lid)
			continue;
		PrintFunc(dest, "%*s%s: LID: 0x%04x  Port: %3u  Priority: %2u  State: %s\n",
				indent, "", (i==0)?"   Master SM":"Secondary SM",
				pImageInfo->SMInfo[i].lid,
				pImageInfo->SMInfo[i].portNumber,
				pImageInfo->SMInfo[i].priority,
				IbSMStateToText(pImageInfo->SMInfo[i].state));
		PrintFunc(dest, "%*s              PortGuid: %016"PRIx64"\n",
				indent, "", pImageInfo->SMInfo[i].smPortGuid);
		PrintFunc(dest, "%*s              Name: %.*s\n",
				indent, "", (int)sizeof(pImageInfo->SMInfo[i].smNodeDesc),
				pImageInfo->SMInfo[i].smNodeDesc);
	}

	return;
}

void PrintStlPAMoveFreeze(PrintDest_t *dest, int indent, const STL_MOVE_FREEZE_DATA *pMoveFreeze)
{
	PrintFunc(dest, "%*sOld Freeze Image\n", indent, "");
	PrintStlPAImageId(dest, indent, &pMoveFreeze->oldFreezeImage);
	PrintFunc(dest, "%*sNew Freeze Image\n", indent, "");
	PrintStlPAImageId(dest, indent, &pMoveFreeze->newFreezeImage);

	return;
}

void PrintStlPAVFList(PrintDest_t *dest, int indent, const int numRecords, const STL_PA_VF_LIST *pVFList)
{
	int i;

	PrintFunc(dest, "%*sNumber of VFs: %u\n", indent, "", numRecords);
	for (i = 0; i < numRecords; i++) {
		PrintFunc(dest, "%*sVF %u: %s\n", indent, "", i+1, pVFList[i].vfName);
	}
	return;
}

void PrintStlPAVFInfo(PrintDest_t *dest, int indent, const STL_PA_VF_INFO_DATA *pVFInfo)
{

	PrintFunc(dest, "%*sVF name: %s\n",
				indent, "", pVFInfo->vfName);
	if (pVFInfo->minInternalRate != IB_STATIC_RATE_DONTCARE
				|| pVFInfo->maxInternalRate != IB_STATIC_RATE_DONTCARE)
		PrintFunc(dest, "%*sNumPorts: %u MinRate: %4s MaxRate: %4s MaxMiBps: %u\n",
				indent, "", pVFInfo->numPorts,
				StlStaticRateToText(pVFInfo->minInternalRate),
				StlStaticRateToText(pVFInfo->maxInternalRate),
				pVFInfo->maxInternalMBps);
	else
		PrintFunc(dest, "%*sNum ports: %u\n",
				indent, "", pVFInfo->numPorts);

	PrintFunc(dest, "%*sInternal utilization statistics:\n",
				indent, "");
	PrintStlPAGroupUtilStats(dest, indent+2, &pVFInfo->internalUtilStats);
	PrintFunc(dest, "%*sInternal Error Summary:\n",
				indent, "");
	PrintStlPAGroupErrorStats(dest, indent+2, &pVFInfo->internalErrors);
	PrintFunc(dest, "%*sImage Id:\n",
				indent, "");
	PrintStlPAImageId(dest, indent+2, &pVFInfo->imageId);

	return;
}

void PrintStlPAVFConfig(PrintDest_t *dest, int indent, const char *vfName, const int numRecords, const STL_PA_VF_CFG_RSP *pVFConfig)
{
	int i;

	PrintFunc(dest, "%*sVF name: %s\n",
				indent, "", vfName);
	PrintFunc(dest, "%*sNumber ports: %u\n",
				indent, "", numRecords);
	for (i = 0; i < numRecords; i++) {
		PrintFunc(dest, "%*s%u:LID:0x%04x Port:%u  GUID:0x%016"PRIx64"  NodeDesc: %.*s\n",
				indent, "", i+1, pVFConfig[i].nodeLid, pVFConfig[i].portNumber,
				pVFConfig[i].nodeGUID,
			   	(int)sizeof(pVFConfig[i].nodeDesc),
			   	pVFConfig[i].nodeDesc);
	}
	PrintStlPAImageId(dest, indent, &pVFConfig->imageId);

	return;
}

void PrintStlPAVFPortCounters(PrintDest_t *dest, int indent, const STL_PA_VF_PORT_COUNTERS_DATA *pVFPortCounters, const uint32 nodeLid, const uint32 portNumber, const uint32 flags)
{
	PrintFunc(dest, "%*s%s Controlled VF Port Counters (%s) for node LID 0x%04x, port number %u%s:\n", indent, "",
			  (flags & STL_PA_PC_FLAG_USER_COUNTERS) ? "User" : "PM",
			  (flags & STL_PA_PC_FLAG_DELTA) ? "delta" : "total",
			  nodeLid, portNumber,
			  (flags & STL_PA_PC_FLAG_UNEXPECTED_CLEAR) ? " (Unexpected Clear)" : "");
	PrintFunc(dest, "%*sVF name: %s\n",
				indent, "", pVFPortCounters->vfName);
	PrintFunc(dest, "%*sPerformance: Transmit\n", indent, "");
	PrintFunc(dest, "%*s    Xmit Data             %20"PRIu64" MB (%"PRIu64" Flits)\n",
			indent, "",
 			pVFPortCounters->portVFXmitData/FLITS_PER_MB,
			pVFPortCounters->portVFXmitData);
	PrintFunc(dest, "%*s    Xmit Pkts             %20"PRIu64"\n",
			indent, "",
			pVFPortCounters->portVFXmitPkts);
	PrintFunc(dest, "%*sPerformance: Receive\n",
			indent, "");
	PrintFunc(dest, "%*s    Rcv Data              %20"PRIu64" MB (%"PRIu64" Flits)\n",
			indent, "",
 			pVFPortCounters->portVFRcvData/FLITS_PER_MB,
			pVFPortCounters->portVFRcvData);
	PrintFunc(dest, "%*s    Rcv Pkts              %20"PRIu64"\n",
			indent, "",
			pVFPortCounters->portVFRcvPkts);
	PrintFunc(dest, "%*sRouting and Other Errors:   \n",
			indent, "");
	PrintFunc(dest, "%*s    Xmit Discards         %20"PRIu64"\n",
			indent, "",
	   		pVFPortCounters->portVFXmitDiscards);
	PrintFunc(dest, "%*sCongestion:                          \n",
			indent, "");
	PrintFunc(dest, "%*s    Congestion Discards   %20"PRIu64"\n",
			indent, "",
	   		pVFPortCounters->swPortVFCongestion);
	PrintFunc(dest, "%*s    Rcv FECN              %20"PRIu64"\n",
			indent, "",
	   		pVFPortCounters->portVFRcvFECN);
	PrintFunc(dest, "%*s    Rcv BECN              %20"PRIu64"\n",
			indent, "",
	   		pVFPortCounters->portVFRcvBECN);
	PrintFunc(dest, "%*s    Mark FECN             %20"PRIu64"\n",
			indent, "",
	   		pVFPortCounters->portVFMarkFECN);
	PrintFunc(dest, "%*s    Xmit Time Cong        %20"PRIu64"\n",
			indent, "",
	   		pVFPortCounters->portVFXmitTimeCong);
	PrintFunc(dest, "%*s    Xmit Wait             %20"PRIu64"\n",
			indent, "",
	   		pVFPortCounters->portVFXmitWait);
	PrintFunc(dest, "%*sBubbles:                            \n",
			indent, "");
	PrintFunc(dest, "%*s    Xmit Wasted BW        %20"PRIu64"\n",
			indent, "",
	   		pVFPortCounters->portVFXmitWastedBW);
	PrintFunc(dest, "%*s    Xmit Wait Data        %20"PRIu64"\n",
			indent, "",
	   		pVFPortCounters->portVFXmitWaitData);
	PrintFunc(dest, "%*s    Rcv Bubble            %20"PRIu64"\n",
			indent, "",
	   		pVFPortCounters->portVFRcvBubble);
	if (flags & STL_PA_PC_FLAG_SHARED_VL) {
		PrintFunc(dest, "\nCounters may be shared between Virtual Fabrics\n\n");
	}
	PrintStlPAImageId(dest, indent, &pVFPortCounters->imageId);

	return;
}

void PrintStlPAVFFocusPorts(PrintDest_t *dest, int indent, const char *vfName, const int numRecords, const uint32 select, const uint32 start, const uint32 range,
	const STL_PA_VF_FOCUS_PORTS_RSP *pVFFocusPorts)
{
	int i;

	PrintFunc(dest, "%*sVF name: %s\n", indent, "", vfName);
	PrintFunc(dest, "%*sNumber links: %u\n", indent, "", numRecords);
	PrintFunc(dest, "%*sFocus select: 0x%x\n", indent, "", select);
	PrintFunc(dest, "%*sFocus start:  %u\n", indent, "", start);
	PrintFunc(dest, "%*sFocus range:  %u\n", indent, "", range);
	for (i = 0; i < numRecords; i++) {
		PrintFunc(dest, "%*s%u:LID:0x%04x  Port:%u  Rate: %4s MTU: %5s nbrLID:0x%04x  nbrPort:%u\n",
				indent, "", i+1, pVFFocusPorts[i].nodeLid, pVFFocusPorts[i].portNumber,
				StlStaticRateToText(pVFFocusPorts[i].rate), IbMTUToText(pVFFocusPorts[i].mtu),
				pVFFocusPorts[i].neighborLid, pVFFocusPorts[i].neighborPortNumber);
		PrintFunc(dest, "%*s   Value:   %16"PRIu64"   nbrValue:  %16"PRIu64"\n",
				indent, "", pVFFocusPorts[i].value, pVFFocusPorts[i].neighborValue);
		PrintFunc(dest, "%*s   GUID:  0x%016"PRIx64"   nbrGuid: 0x%016"PRIx64"\n",
				indent, "", pVFFocusPorts[i].nodeGUID, pVFFocusPorts[i].neighborGuid);
		PrintFunc(dest, "%*s   Status: %s Name: %.*s\n",
				indent, "",
				StlFocusFlagToText(pVFFocusPorts[i].localFlags),
				(int)sizeof(pVFFocusPorts[i].nodeDesc),
				pVFFocusPorts[i].nodeDesc);
		PrintFunc(dest, "%*s   Status: %s Neighbor Name: %.*s\n",
				indent, "",
				StlFocusFlagToText(pVFFocusPorts[i].neighborFlags),
				(int)sizeof(pVFFocusPorts[i].neighborNodeDesc),
				pVFFocusPorts[i].neighborNodeDesc);
	}
	PrintStlPAImageId(dest, indent, &pVFFocusPorts[0].imageId);

	return;
}
