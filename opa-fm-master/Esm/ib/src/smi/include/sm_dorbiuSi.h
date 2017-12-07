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

//===========================================================================//
//			
// FILE NAME
//    sm_dor.h
//
// DESCRIPTION
//    This file contains the SM Dor/Torus Routing SM definitions.
//
//===========================================================================//

#ifndef	_SM_DORBIUSI_H_
#define	_SM_DORBIUSI_H_

/*
#define SM_DOR_MAX_TOROIDAL_DIMENSIONS 6

// DorBitMaps are indexed by ij using switch indexes.
//
// When switches disappear from the fabric and sm_compactSwitchSpace() runs
// it changes switch indices and the max switch count. This compaction is
// done after the closure bit maps were allocated and set based on
// the old max switch count. Though the closure bit maps are altered
// to take into account the change in switch indices (done in  process_swIdx_change),
// the bit map space isn't reallocated, so any index calculations should be based
// on the old count of switches when the closure was computed

#define	DorBitMapsIndex(X,Y)	(((X) * (((DorTopology_t *)(sm_topop->routingModule->data))->closure_max_sws)) + (Y))

#define DEFAULT_IJ_BITMAP 0xffffffff
static __inline__ void ijBiuSet(uint32* ijBitmap, int ij,int value) {
//	ijBitmap[ij >> 5] |= 1 << (ij & 0x1f);
	ijBitmap[ij]=value;
}

static __inline__ void ijBiuClear(uint32* ijBitmap, int ij) {
//	ijBitmap[ij >> 5] &= ~((uint32_t)(1 << (ij & 0x1f)));
	ijBitmap[ij]=DEFAULT_IJ_BITMAP;
}

static __inline__ int ijBiuTest(uint32* ijBitmap, int ij) {
//	return ((ijBitmap[ij >> 5] & (1 << (ij & 0x1f))) ? 1 : 0);
	return (ijBitmap[ij]!=DEFAULT_IJ_BITMAP?1:0);
}

static __inline__ int ijBiuGet(uint32* ijBitmap, int ij){
	return ijBitmap[ij];
}

// DOR Topology Information
*/
typedef struct _DorBiuSiNode {
	int8_t			coords[SM_DOR_MAX_DIMENSIONS];
	Node_t			*node;
	int			multipleBrokenDims;
	struct _DorBiuNode *left[SM_DOR_MAX_DIMENSIONS];
	struct _DorBiuNode *right[SM_DOR_MAX_DIMENSIONS];
//--------------------zp start----------------//
	Node_t 			*brother;     //the node connected by biu link
	Node_t			*comNode;		//the communication node
//--------------------zp stop-----------------//
} DorBiuSiNode_t;

/*
typedef struct  _DorTopology {
	// mesh sizing
	uint8_t numDimensions;
	uint8_t dimensionLength[SM_DOR_MAX_DIMENSIONS];
	// per-dimension coordinate bounds.  coordinates are assumed
	// contiguous within these bounds
	int8_t coordMaximums[SM_DOR_MAX_DIMENSIONS];
	int8_t coordMinimums[SM_DOR_MAX_DIMENSIONS];
	// for each dimension, true if toroidal
	uint8_t toroidal[SM_DOR_MAX_DIMENSIONS];
	// the configured number of toroidal dimensions
	uint8_t numToroidal;
	// count of toroidal dimensions found till now
	uint8_t toroidal_count;
	// minimum number of SCs required to route the fabric correctly
	uint8_t minReqScs;
	// maps the index of all dimensions to an index into only the
	// toroidal dimensions
	uint8_t toroidalMap[SM_DOR_MAX_DIMENSIONS];
	// flat 2d bitfield indexed on switch indices marking whether
	// the cycle-free DOR path is realizable between nodes
	size_t dorClosureSize;
	uint32_t *dorLeft;
	uint32_t *dorRight;
	uint32_t *dorBroken;
	uint32_t closure_max_sws;   //the switch count when the closures were computed
	DorNode_t *datelineSwitch;
} DorTopology_t;

typedef enum {
	DorAny   = 0,
	DorLeft  = 1,
	DorRight = 2,
} DorDirection;

static __inline__ int dorBiuClosure(DorTopology_t* dorTop, int i, int j) {
	int ij = DorBitMapsIndex(i, j);
	if (i == j) return 1;
	return (ijBiuTest(dorTop->dorLeft, ij) || ijBiuTest(dorTop->dorRight, ij));
}
/*
//===========================================================================//
// DOR COORDINATE ASSIGNMENT
//
typedef struct _detected_dim {
	uint8_t			dim;
	uint64_t		neighbor_nodeGuid;
	uint8_t			port;
	uint8_t			neighbor_port;
	int				pos;
	Node_t			*neighbor_nodep;
} detected_dim_t;

#define PORT_PAIR_WARN_ARR_SIZE		40
#define PORT_PAIR_WARN_IDX(X,Y)		(((X) * PORT_PAIR_WARN_ARR_SIZE) + (Y))

typedef enum {
	DIM_LOOKUP_RVAL_FOUND,
	DIM_LOOKUP_RVAL_NOTFOUND,
	DIM_LOOKUP_RVAL_INVALID
} DimLookupRval_t;

typedef struct _DorDimension {
	uint8_t			ingressPort;
	uint8_t			dimension;
	int8_t			direction;
	uint8_t			hyperlink;
	cl_map_obj_t	portObj;
} DorDimension_t;
*/
typedef struct _DorBiuSiDiscoveryState {
	uint8_t			nextDimension;
	uint8_t			toroidalOverflow;
	uint8_t			scsAvailable; // min of SC support of all fabric ISLs
	DorDimension_t	*dimensionMap[256]; // indexed by egress port
	//-----------------zp start----------------------//
	uint8_t         portbiu;
	uint8_t 		numBiuInSi;
	//-----------------zp stop-----------------------//
} DorBiuSiDiscoveryState_t;
/*
//----------------------zp start-------------------//
#define DORBIU_COORDINATE_STRING_LEN 200
//----------------------zp stop--------------------//
*/

#endif
