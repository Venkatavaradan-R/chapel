/*
 * Copyright 2020-2021 Hewlett Packard Enterprise Development LP
 * Copyright 2004-2019 Cray Inc.
 * Other additional copyright holders may be indicated within.
 * 
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 * 
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// OFI-based implementation of Chapel communication interface.
//

#include "chplrt.h"
#include "chpl-env-gen.h"

#include "chpl-comm.h"
#include "chpl-comm-callbacks.h"
#include "chpl-comm-callbacks-internal.h"
#include "chpl-comm-diags.h"
#include "chpl-comm-internal.h"
#include "chpl-comm-strd-xfer.h"
#include "chpl-env.h"
#include "chplexit.h"
#include "chpl-format.h"
#include "chpl-gen-includes.h"
#include "chpl-linefile-support.h"
#include "chpl-mem.h"
#include "chpl-mem-sys.h"
#include "chplsys.h"
#include "chpl-tasks.h"
#include "chpl-topo.h"
#include "chpltypes.h"
#include "error.h"

#include "comm-ofi-internal.h"

// Don't get warning macros for chpl_comm_get etc
#include "chpl-comm-no-warning-macros.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h> /* for struct iovec */
#include <time.h>
#include <unistd.h>

#ifdef CHPL_COMM_DEBUG
#include <ctype.h>
#endif

#include <rdma/fabric.h>
#include <rdma/fi_atomic.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>


////////////////////////////////////////
//
// Data global to all comm-ofi*.c files
//

//
// These are declared extern in comm-ofi-internal.h.
//
#ifdef CHPL_COMM_DEBUG
uint64_t chpl_comm_ofi_dbg_level;
FILE* chpl_comm_ofi_dbg_file;
#endif

int chpl_comm_ofi_abort_on_error;


////////////////////////////////////////
//
// Libfabric API version
//

//
// This is used to check that the libfabric version the runtime is
// linked with in a user program is the same one it was compiled
// against.
//
#define COMM_OFI_FI_VERSION FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION)


////////////////////////////////////////
//
// Types and data global just within this file.
//

static struct fi_info* ofi_info;        // fabric interface info
static struct fid_fabric* ofi_fabric;   // fabric domain
static struct fid_domain* ofi_domain;   // fabric access domain
static int useScalableTxEp;             // use a scalable tx endpoint?
static struct fid_ep* ofi_txEpScal;     // scalable transmit endpoint
static struct fid_poll* ofi_amhPollSet; // poll set for AM handler
static int pollSetSize = 0;             // number of fids in the poll set
static struct fid_wait* ofi_amhWaitSet; // wait set for AM handler

static int haveDeliveryComplete;        // delivery-complete? (vs. msg order)

//
// We direct RMA traffic and AM traffic to different endpoints so we can
// spread the progress load across all the threads when we're doing
// manual progress.
//
static struct fid_ep* ofi_rxEp;         // AM req receive endpoint
static struct fid_cq* ofi_rxCQ;         // AM req receive endpoint CQ
static struct fid_ep* ofi_rxEpRma;      // RMA/AMO target endpoint
static struct fid_cq* ofi_rxCQRma;      // RMA/AMO target endpoint CQ
static struct fid_cntr* ofi_rxCntrRma;  // RMA/AMO target endpoint counter
static struct fid* ofi_rxCmplFidRma;    // rxCQRma or rxCntrRma fid
static void (*checkRxRmaCmplsFn)(void); // fn: check for RMA/AMO EP completions

static struct fid_av* ofi_av;           // address vector
static fi_addr_t* ofi_rxAddrs;          // table of remote endpoint addresses

#define rxMsgAddr(tcip, n) (ofi_rxAddrs[2 * (n)])
#define rxRmaAddr(tcip, n) (ofi_rxAddrs[2 * (n) + 1])

//
// Transmit support.
//
static int numTxCtxs;
static int numRxCtxs;

struct perTxCtxInfo_t {
  atomic_bool allocated;        // true: in use; false: available
  chpl_bool bound;              // true: bound to an owner (usually a thread)
  struct fid_ep* txCtx;         // transmit context (endpoint, if not scalable)
  struct fid_cq* txCQ;          // completion CQ
  struct fid_cntr* txCntr;      // completion counter (AM handler tx ctx only)
  struct fid* txCmplFid;        // CQ or counter fid
                                // fn: check for tx completions
  void (*checkTxCmplsFn)(struct perTxCtxInfo_t*);
                                // fn: ensure progress
  void (*ensureProgressFn)(struct perTxCtxInfo_t*);
  uint64_t numTxnsOut;          // number of transactions in flight now
  uint64_t numTxnsSent;         // number of transactions ever initiated
};

static int tciTabLen;
static struct perTxCtxInfo_t* tciTab;
static chpl_bool tciTabFixedAssignments;

static int txCQLen;

//
// Memory registration support.
//
static chpl_bool scalableMemReg;

#define MAX_MEM_REGIONS 10
static int numMemRegions = 0;

static struct fid_mr* ofiMrTab[MAX_MEM_REGIONS];

struct memEntry {
  void* addr;
  size_t base;
  size_t size;
  void* desc;
  uint64_t key;
};

typedef struct memEntry (memTab_t)[MAX_MEM_REGIONS];

static memTab_t memTab;
static memTab_t* memTabMap;

//
// Messaging (AM) support.
//

#define AM_MAX_EXEC_ON_PAYLOAD_SIZE 1024

struct amRequest_execOn_t {
  chpl_comm_on_bundle_t hdr;
  char space[AM_MAX_EXEC_ON_PAYLOAD_SIZE];
};

struct amRequest_execOnLrg_t {
  chpl_comm_on_bundle_t hdr;
  void* pPayload;                 // addr of arg payload on initiator node
};

static int numAmHandlers = 1;

//
// AM request landing zones.
//
static void* amLZs[2];
static struct iovec ofi_iov_reqs[2];
static struct fi_msg ofi_msg_reqs[2];
static int ofi_msg_i;


////////////////////////////////////////
//
// Forward decls
//

static void emit_delayedFixedHeapMsgs(void);

static inline struct perTxCtxInfo_t* tciAlloc(void);
static inline struct perTxCtxInfo_t* tciAllocForAmHandler(void);
static inline void tciFree(struct perTxCtxInfo_t*);
static inline chpl_comm_nb_handle_t ofi_put(const void*, c_nodeid_t,
                                            void*, size_t);
static inline void ofi_put_ll(const void*, c_nodeid_t,
                              void*, size_t, void*, struct perTxCtxInfo_t*,
                              chpl_bool);
static inline void do_remote_put_buff(void*, c_nodeid_t, void*, size_t);
static inline chpl_comm_nb_handle_t ofi_get(void*, c_nodeid_t,
                                            void*, size_t);
static inline void ofi_get_ll(void*, c_nodeid_t,
                              void*, size_t, void*, struct perTxCtxInfo_t*);
static inline void do_remote_get_buff(void*, c_nodeid_t, void*, size_t);
static inline void do_remote_amo_nf_buff(void*, c_nodeid_t, void*, size_t,
                                         enum fi_op, enum fi_datatype);
static void amEnsureProgress(struct perTxCtxInfo_t*);
static void checkRxRmaCmplsCQ(void);
static void checkRxRmaCmplsCntr(void);
static void checkTxCmplsCQ(struct perTxCtxInfo_t*);
static void checkTxCmplsCntr(struct perTxCtxInfo_t*);
static inline size_t readCQ(struct fid_cq*, void*, size_t);
static void reportCQError(struct fid_cq*);
static inline void waitForTxnComplete(struct perTxCtxInfo_t*, void* ctx);
static inline void waitForPutsVisOneNode(c_nodeid_t, struct perTxCtxInfo_t*,
                                         chpl_comm_taskPrvData_t*);
static inline void waitForPutsVisAllNodes(struct perTxCtxInfo_t*,
                                          chpl_comm_taskPrvData_t*, chpl_bool);
static void* allocBounceBuf(size_t);
static void freeBounceBuf(void*);
static inline void local_yield(void);

static void time_init(void);


////////////////////////////////////////
//
// Alignment
//

#define ALIGN_DN(i, size)  ((i) & ~((size) - 1))
#define ALIGN_UP(i, size)  ALIGN_DN((i) + (size) - 1, size)


////////////////////////////////////////
//
// Error checking
//

static void ofiErrReport(const char*, int, const char*);

#define OFI_ERR(exprStr, retVal, errStr)                                \
    do {                                                                \
      ofiErrReport(exprStr, retVal, errStr); /* may not return */       \
      INTERNAL_ERROR_V("OFI error: %s: %s", exprStr, errStr);           \
    } while (0)

#define OFI_CHK(expr) OFI_CHK_1(expr, FI_SUCCESS)

#define OFI_CHK_1(expr, want1)                                          \
    do {                                                                \
      int retVal = (expr);                                              \
      if (retVal != want1) {                                            \
        OFI_ERR(#expr, retVal, fi_strerror(-retVal));                   \
      }                                                                 \
    } while (0)

#define OFI_CHK_2(expr, retVal, want2)                                  \
    do {                                                                \
      retVal = (expr);                                                  \
      if (retVal != FI_SUCCESS && retVal != want2) {                    \
        OFI_ERR(#expr, retVal, fi_strerror(-retVal));                   \
      }                                                                 \
    } while (0)

#define OFI_CHK_COUNT(expr, retVal)                                     \
  do {                                                                  \
    retVal = (expr);                                                    \
    if (retVal < 0) {                                                   \
      OFI_ERR(#expr, retVal, fi_strerror(-retVal));                     \
    }                                                                   \
  } while (0)


#define PTHREAD_CHK(expr) CHK_EQ_TYPED(expr, 0, int, "d")


////////////////////////////////////////
//
// Early declarations for AM handling and progress
//

//
// Ideally these would be declared with related stuff later in the
// file, but they're needed earlier than that and with each other,
// so they're here instead.
//

//
// Is this the (an) AM handler thread?
//
static __thread chpl_bool isAmHandler = false;


//
// Flag used to tell AM handler(s) to exit.
//
static atomic_bool amHandlersExit;


//
// Should the AM handler do liveness checks?
//
static chpl_bool amDoLivenessChecks = false;


//
// The ofi_rxm provider may return -FI_EAGAIN for read/write/send while
// doing on-demand connection when emulating FI_RDM endpoints.  The man
// page says: "Applications should be aware of this and retry until the
// the operation succeeds."  Handle this in a generalized way, because
// it seems like something we might encounter with other providers as
// well.
//
#define OFI_RIDE_OUT_EAGAIN(tcip, expr)                                 \
  do {                                                                  \
    ssize_t _ret;                                                       \
    if (isAmHandler) {                                                  \
      do {                                                              \
        OFI_CHK_2(expr, _ret, -FI_EAGAIN);                              \
        if (_ret == -FI_EAGAIN) {                                       \
          (*tcip->ensureProgressFn)(tcip);                              \
        }                                                               \
      } while (_ret == -FI_EAGAIN                                       \
               && !atomic_load_bool(&amHandlersExit));                  \
    } else {                                                            \
      do {                                                              \
        OFI_CHK_2(expr, _ret, -FI_EAGAIN);                              \
        if (_ret == -FI_EAGAIN) {                                       \
          (*tcip->ensureProgressFn)(tcip);                              \
        }                                                               \
      } while (_ret == -FI_EAGAIN);                                     \
    }                                                                   \
  } while (0)


////////////////////////////////////////
//
// Providers
//

//
// provider name in environment
//

static const char* ofi_provNameEnv = "FI_PROVIDER";
static pthread_once_t provNameOnce = PTHREAD_ONCE_INIT;
static const char* provName;

static
void setProvName(void) {
  provName = getenv(ofi_provNameEnv);
}

static
const char* getProviderName(void) {
  PTHREAD_CHK(pthread_once(&provNameOnce, setProvName));
  return provName;
}


//
// provider name parsing
//

static inline
chpl_bool isInProvName(const char* s, const char* prov_name) {
  if (prov_name == NULL) {
    return false;
  }

  char pn[strlen(prov_name) + 1];
  strcpy(pn, prov_name);

  char* tok;
  char* strSave;
  for (char* pnPtr = pn;
       (tok = strtok_r(pnPtr, ";", &strSave)) != NULL;
       pnPtr = NULL) {
    if (strcmp(s, tok) == 0) {
      return true;
    }
  }

  return false;
}


//
// provider type
//
typedef enum {
  provType_efa,
  provType_gni,
  provType_verbs,
  provType_rxd,
  provType_rxm,
  provTypeCount
} provider_t;


//
// provider sets
//

typedef chpl_bool providerSet_t[provTypeCount];

static inline
void providerSetSet(providerSet_t* s, provider_t p) {
  CHK_TRUE(p >= 0 && p < provTypeCount);
  (*s)[p] = 1;
}

static inline
int providerSetTest(providerSet_t* s, provider_t p) {
  CHK_TRUE(p >= 0 && p < provTypeCount);
  return (*s)[p] != 0;
}


//
// providers in use
//

static pthread_once_t providerInUseOnce = PTHREAD_ONCE_INIT;
static providerSet_t providerInUseSet;

static
void init_providerInUse(void) {
  if (chpl_numNodes <= 1) {
    return;
  }

  //
  // We can be using only one primary provider.
  //
  const char* pn = ofi_info->fabric_attr->prov_name;
  if (isInProvName("efa", pn)) {
    providerSetSet(&providerInUseSet, provType_efa);
  } else if (isInProvName("gni", pn)) {
    providerSetSet(&providerInUseSet, provType_gni);
  } else if (isInProvName("verbs", pn)) {
    providerSetSet(&providerInUseSet, provType_verbs);
  }
  //
  // We can be using any number of utility providers.
  //
  if (isInProvName("ofi_rxd", pn)) {
    providerSetSet(&providerInUseSet, provType_rxd);
  }
  if (isInProvName("ofi_rxm", pn)) {
    providerSetSet(&providerInUseSet, provType_rxm);
  }
}

static
chpl_bool providerInUse(provider_t p) {
  if (ofi_info != NULL) {
    // Early exit hedge: don't init "in use" info until we have one.
    PTHREAD_CHK(pthread_once(&providerInUseOnce, init_providerInUse));
  }

  return providerSetTest(&providerInUseSet, p);
}


//
// provider-specific behavior control
//

static chpl_bool provCtl_sizeAvsByNumEps;  // size AVs by numEPs (RxD)
static chpl_bool provCtl_readAmoNeedsOpnd; // READ AMO needs operand (RxD)


////////////////////////////////////////
//
// transaction tracking
//

//
// If we need to wait for an individual transaction's network completion
// we give the address of a 'txnDone' flag as the context pointer when we
// initiate the transaction, and then just wait for the flag to become
// true.  We encode this information in the context pointer we pass to
// libfabric, and then it hands it back to us in the CQ entry, and then
// checkTxCQ() uses that to figure out what to update.
//

typedef enum {
  txnTrkId,    // no tracking as such, context "ptr" is just an id value
  txnTrkDone,  // *ptr is atomic bool 'done' flag
  txnTrkTypeCount
} txnTrkType_t;

#define TXNTRK_TYPE_BITS 1
#define TXNTRK_ADDR_BITS (64 - TXNTRK_TYPE_BITS)
#define TXNTRK_TYPE_MASK ((1UL << TXNTRK_TYPE_BITS) - 1UL)
#define TXNTRK_ADDR_MASK (~(TXNTRK_TYPE_MASK << TXNTRK_ADDR_BITS))

typedef struct {
  txnTrkType_t typ;
  void* ptr;
} txnTrkCtx_t;

static inline
void* txnTrkEncode(txnTrkType_t typ, void* p) {
  assert((((uint64_t) txnTrkTypeCount - 1UL) & ~TXNTRK_TYPE_MASK) == 0UL);
  assert((((uint64_t) p) & ~TXNTRK_ADDR_MASK) == 0UL);
  return (void*) (  ((uint64_t) typ << TXNTRK_ADDR_BITS)
                  | ((uint64_t) p & TXNTRK_ADDR_MASK));
}

static inline
void* txnTrkEncodeId(intptr_t id) {
  return txnTrkEncode(txnTrkId, (void*) id);
}

static inline
void* txnTrkEncodeDone(void* pDone) {
  return txnTrkEncode(txnTrkDone, pDone);
}

static inline
txnTrkCtx_t txnTrkDecode(void* ctx) {
  const uint64_t u = (uint64_t) ctx;
  return (txnTrkCtx_t) { .typ = (u >> TXNTRK_ADDR_BITS) & TXNTRK_TYPE_MASK,
                         .ptr = (void*) (u & TXNTRK_ADDR_MASK) };
}


////////////////////////////////////////
//
// transaction ordering
//

//
// This is a little dummy memory buffer used when we need to do a
// remote GET or PUT to enforce ordering.  Its contents must _not_
// by considered meaningful, because it can be written to at any
// time by any remote node.
//
static uint32_t* orderDummy;
static uint32_t** orderDummyMap;


////////////////////////////////////////
//
// bitmaps
//

#define bitmapElemWidth 64
#define __bitmapBaseType_t(w) uint##w##_t
#define _bitmapBaseType_t(w) __bitmapBaseType_t(w)
#define bitmapBaseType_t _bitmapBaseType_t(bitmapElemWidth)

struct bitmap_t {
  size_t len;
  bitmapBaseType_t map[0];
};

static inline
size_t bitmapElemIdx(size_t i) {
  return i / bitmapElemWidth;
}

static inline
size_t bitmapOff(size_t i) {
  return i % bitmapElemWidth;
}

static inline
size_t bitmapNumElems(size_t len) {
  return (((ssize_t) len) - 1) / bitmapElemWidth + 1;
}

static inline
size_t bitmapSizeofMap(size_t len) {
  return bitmapNumElems(len) * sizeof(bitmapBaseType_t);
}

static inline
size_t bitmapSizeof(size_t len) {
  return offsetof(struct bitmap_t, map) + bitmapSizeofMap(len);
}

static inline
void bitmapZero(struct bitmap_t* b) {
  memset(&b->map, 0, bitmapSizeofMap(b->len));
}

static inline
bitmapBaseType_t bitmapElemBit(size_t i) {
  return ((bitmapBaseType_t) 1) << bitmapOff(i);
}

static inline
void bitmapClear(struct bitmap_t* b, size_t i) {
  b->map[bitmapElemIdx(i)] &= ~bitmapElemBit(i);
}

static inline
void bitmapSet(struct bitmap_t* b, size_t i) {
  b->map[bitmapElemIdx(i)] |= bitmapElemBit(i);
}

static inline
int bitmapTest(struct bitmap_t* b, size_t i) {
  return (b->map[bitmapElemIdx(i)] & bitmapElemBit(i)) != 0;
}

#define BITMAP_FOREACH_SET(b, i)                                        \
  do {                                                                  \
    size_t _eWid = bitmapElemWidth;                                     \
    size_t _eCnt = bitmapNumElems((b)->len);                            \
    size_t _bCnt = (b)->len;                                            \
    for (size_t _ei = 0; _ei < _eCnt; _ei++, _bCnt -= _eWid) {          \
      if ((b)->map[_ei] != 0) {                                         \
        size_t _bi_end = (_eWid < _bCnt) ? _eWid : _bCnt;               \
        for (size_t _bi = 0; _bi < _bi_end; _bi++) {                    \
          if (((b)->map[_ei] & bitmapElemBit(_bi)) != 0) {              \
            size_t i = _ei * bitmapElemWidth + _bi;

#define BITMAP_FOREACH_SET_END  } } } } } while (0);

static inline
struct bitmap_t* bitmapAlloc(size_t len) {
  struct bitmap_t* b;
  CHPL_CALLOC_SZ(b, 1, bitmapSizeof(len));
  b->len = len;
  return b;
}

static inline
void bitmapFree(struct bitmap_t* b) {
  if (DBG_TEST_MASK(DBG_ORDER)) {
    BITMAP_FOREACH_SET(b, node) {
      INTERNAL_ERROR_V("bitmapFree(): bitmap is not empty; first node %d",
                       (int) node);
    } BITMAP_FOREACH_SET_END
  }

  CHPL_FREE(b);
}


////////////////////////////////////////
//
// task private data
//

static inline
chpl_comm_taskPrvData_t* get_comm_taskPrvdata(void) {
  chpl_task_infoRuntime_t* infoRuntime;
  if ((infoRuntime = chpl_task_getInfoRuntime()) != NULL) {
    return &infoRuntime->comm_data;
  }

  if (isAmHandler) {
    static __thread chpl_comm_taskPrvData_t amHandlerCommData;
    return &amHandlerCommData;
  }

  return NULL;
}


////////////////////////////////////////
//
// task local buffering
//

// Largest size to use unordered transactions for
#define MAX_UNORDERED_TRANS_SZ 1024

//
// Maximum number of PUTs/AMOs in a chained transaction list.  This
// is a provisional value, not yet tuned.
//
#define MAX_TXNS_IN_FLIGHT 64

#define MAX_CHAINED_AMO_NF_LEN MAX_TXNS_IN_FLIGHT
#define MAX_CHAINED_PUT_LEN MAX_TXNS_IN_FLIGHT
#define MAX_CHAINED_GET_LEN MAX_TXNS_IN_FLIGHT

enum BuffType {
  amo_nf_buff = 1 << 0,
  get_buff    = 1 << 1,
  put_buff    = 1 << 2
};

// Per task information about non-fetching AMO buffers
typedef struct {
  chpl_bool          new;
  int                vi;
  uint64_t           opnd1_v[MAX_CHAINED_AMO_NF_LEN];
  c_nodeid_t         locale_v[MAX_CHAINED_AMO_NF_LEN];
  void*              object_v[MAX_CHAINED_AMO_NF_LEN];
  size_t             size_v[MAX_CHAINED_AMO_NF_LEN];
  enum fi_op         cmd_v[MAX_CHAINED_AMO_NF_LEN];
  enum fi_datatype   type_v[MAX_CHAINED_AMO_NF_LEN];
  uint64_t           remote_mr_v[MAX_CHAINED_AMO_NF_LEN];
  void*              local_mr;
} amo_nf_buff_task_info_t;

// Per task information about GET buffers
typedef struct {
  chpl_bool     new;
  int           vi;
  void*         tgt_addr_v[MAX_CHAINED_GET_LEN];
  c_nodeid_t    locale_v[MAX_CHAINED_GET_LEN];
  uint64_t      remote_mr_v[MAX_CHAINED_GET_LEN];
  void*         src_addr_v[MAX_CHAINED_GET_LEN];
  size_t        size_v[MAX_CHAINED_GET_LEN];
  void*         local_mr_v[MAX_CHAINED_GET_LEN];
} get_buff_task_info_t;

// Per task information about PUT buffers
typedef struct {
  chpl_bool     new;
  int           vi;
  void*         tgt_addr_v[MAX_CHAINED_PUT_LEN];
  c_nodeid_t    locale_v[MAX_CHAINED_PUT_LEN];
  void*         src_addr_v[MAX_CHAINED_PUT_LEN];
  char          src_v[MAX_CHAINED_PUT_LEN][MAX_UNORDERED_TRANS_SZ];
  size_t        size_v[MAX_CHAINED_PUT_LEN];
  uint64_t      remote_mr_v[MAX_CHAINED_PUT_LEN];
  void*         local_mr_v[MAX_CHAINED_PUT_LEN];
  struct bitmap_t nodeBitmap;
} put_buff_task_info_t;

// Acquire a task local buffer, initializing if needed
static inline
void* task_local_buff_acquire(enum BuffType t, size_t extra_size) {
  chpl_comm_taskPrvData_t* prvData = get_comm_taskPrvdata();
  if (prvData == NULL) return NULL;

#define DEFINE_INIT(TYPE, TLS_NAME)                                           \
  if (t == TLS_NAME) {                                                        \
    TYPE* info = prvData->TLS_NAME;                                           \
    if (info == NULL) {                                                       \
      prvData->TLS_NAME = chpl_mem_alloc(sizeof(TYPE) + extra_size,           \
                                         CHPL_RT_MD_COMM_PER_LOC_INFO, 0, 0); \
      info = prvData->TLS_NAME;                                               \
      info->new = true;                                                       \
      info->vi = 0;                                                           \
    }                                                                         \
    return info;                                                              \
  }

  DEFINE_INIT(amo_nf_buff_task_info_t, amo_nf_buff);
  DEFINE_INIT(get_buff_task_info_t, get_buff);
  DEFINE_INIT(put_buff_task_info_t, put_buff);

#undef DEFINE_INIT
  return NULL;
}

static void amo_nf_buff_task_info_flush(amo_nf_buff_task_info_t* info);
static void get_buff_task_info_flush(get_buff_task_info_t* info);
static void put_buff_task_info_flush(put_buff_task_info_t* info);

// Flush one or more task local buffers
static inline
void task_local_buff_flush(enum BuffType t) {
  chpl_comm_taskPrvData_t* prvData = get_comm_taskPrvdata();
  if (prvData == NULL) return;

#define DEFINE_FLUSH(TYPE, TLS_NAME, FLUSH_NAME)                              \
  if (t & TLS_NAME) {                                                         \
    TYPE* info = prvData->TLS_NAME;                                           \
    if (info != NULL && info->vi > 0) {                                       \
      FLUSH_NAME(info);                                                       \
    }                                                                         \
  }

  DEFINE_FLUSH(amo_nf_buff_task_info_t, amo_nf_buff,
               amo_nf_buff_task_info_flush);
  DEFINE_FLUSH(get_buff_task_info_t, get_buff, get_buff_task_info_flush);
  DEFINE_FLUSH(put_buff_task_info_t, put_buff, put_buff_task_info_flush);

#undef DEFINE_FLUSH
}

// Flush and destroy one or more task local buffers
static inline
void task_local_buff_end(enum BuffType t) {
  chpl_comm_taskPrvData_t* prvData = get_comm_taskPrvdata();
  if (prvData == NULL) return;

#define DEFINE_END(TYPE, TLS_NAME, FLUSH_NAME)                                \
  if (t & TLS_NAME) {                                                         \
    TYPE* info = prvData->TLS_NAME;                                           \
    if (info != NULL && info->vi > 0) {                                       \
      FLUSH_NAME(info);                                                       \
      chpl_mem_free(info, 0, 0);                                              \
      prvData->TLS_NAME = NULL;                                               \
    }                                                                         \
  }

  DEFINE_END(amo_nf_buff_task_info_t, amo_nf_buff,
             amo_nf_buff_task_info_flush);
  DEFINE_END(get_buff_task_info_t, get_buff, get_buff_task_info_flush);
  DEFINE_END(put_buff_task_info_t, put_buff, put_buff_task_info_flush);

#undef END
}


////////////////////////////////////////
//
// Interface: initialization
//

pthread_t pthread_that_inited;

static void init_ofi(void);
static void init_ofiFabricDomain(void);
static void init_ofiDoProviderChecks(void);
static void init_ofiEp(void);
static void init_ofiEpNumCtxs(void);
static void init_ofiEpTxCtx(int, chpl_bool,
                            struct fi_cq_attr*, struct fi_cntr_attr*);
static void init_ofiExchangeAvInfo(void);
static void init_ofiForMem(void);
static void init_ofiForRma(void);
static void init_ofiForAms(void);

static void init_bar(void);

static void init_broadcast_private(void);


//
// forward decls
//
static inline int mrGetLocalKey(void*, size_t);
static inline int mrGetDesc(void**, void*, size_t);


void chpl_comm_init(int *argc_p, char ***argv_p) {
  chpl_comm_ofi_abort_on_error =
    (chpl_env_rt_get("COMM_OFI_ABORT_ON_ERROR", NULL) != NULL);
  time_init();
  chpl_comm_ofi_oob_init();
  DBG_INIT();

  //
  // The user can specify the provider by setting either the Chapel
  // CHPL_RT_COMM_OFI_PROVIDER environment variable or the libfabric
  // FI_PROVIDER one, with the former overriding the latter if both
  // are set.
  //
  {
    const char* s = chpl_env_rt_get("COMM_OFI_PROVIDER", NULL);
    if (s != NULL) {
      chpl_env_set(ofi_provNameEnv, s, 1 /*overwrite*/);
    }
  }

  pthread_that_inited = pthread_self();
}


void chpl_comm_post_mem_init(void) {
  DBG_PRINTF(DBG_IFACE_SETUP, "%s()", __func__);

  chpl_comm_init_prv_bcast_tab();
  init_broadcast_private();
}


//
// No support for gdb for now
//
int chpl_comm_run_in_gdb(int argc, char* argv[], int gdbArgnum, int* status) {
  return 0;
}

//
// No support for lldb for now
//
int chpl_comm_run_in_lldb(int argc, char* argv[], int lldbArgnum, int* status) {
  return 0;
}


void chpl_comm_post_task_init(void) {
  DBG_PRINTF(DBG_IFACE_SETUP, "%s()", __func__);

  if (chpl_numNodes == 1)
    return;
  init_ofi();
  init_bar();
}


static
void init_ofi(void) {
  init_ofiFabricDomain();
  init_ofiDoProviderChecks();
  init_ofiEp();
  init_ofiExchangeAvInfo();
  init_ofiForMem();
  init_ofiForRma();
  init_ofiForAms();

  CHPL_CALLOC(orderDummy, 1);
  CHK_TRUE(mrGetLocalKey(orderDummy, sizeof(*orderDummy)) == 0);
  CHK_TRUE(mrGetDesc(NULL, orderDummy, sizeof(*orderDummy)) == 0);
  CHPL_CALLOC(orderDummyMap, chpl_numNodes);
  chpl_comm_ofi_oob_allgather(&orderDummy, orderDummyMap,
                              sizeof(orderDummyMap[0]));

  DBG_PRINTF(DBG_CFG,
             "AM config: recv buf size %zd MiB, %s, responses use %s",
             ofi_iov_reqs[ofi_msg_i].iov_len / (1L << 20),
             (ofi_amhPollSet == NULL) ? "explicit polling" : "poll+wait sets",
             (tciTab[tciTabLen - 1].txCQ != NULL) ? "CQ" : "counter");
  if (useScalableTxEp) {
    DBG_PRINTF(DBG_CFG,
               "per node config: 1 scalable tx ep + %d tx ctx%s (%d fixed), "
               "%d rx ctx%s",
               numTxCtxs, (numTxCtxs == 1) ? "" : "s",
               tciTabFixedAssignments ? chpl_task_getFixedNumThreads() : 0,
               numRxCtxs, (numRxCtxs == 1) ? "" : "s");
  } else {
    DBG_PRINTF(DBG_CFG,
               "per node config: %d regular tx ep+ctx%s (%d fixed), "
               "%d rx ctx%s",
               numTxCtxs, (numTxCtxs == 1) ? "" : "s",
               tciTabFixedAssignments ? chpl_task_getFixedNumThreads() : 0,
               numRxCtxs, (numRxCtxs == 1) ? "" : "s");
  }
}


#ifdef CHPL_COMM_DEBUG
struct cfgHint {
  const char* str;
  unsigned long int val;
};


static
chpl_bool getCfgHint(const char* evName, struct cfgHint hintVals[],
                     chpl_bool justOne, uint64_t* pVal) {
  const char* ev = chpl_env_rt_get(evName, "");
  if (strcmp(ev, "") == 0) {
    return false;
  }

  *pVal = 0;

  char evCopy[strlen(ev) + 1];
  strcpy(evCopy, ev);
  char* p = strtok(evCopy, "|");
  while (p != NULL) {
    int i;
    for (i = 0; hintVals[i].str != NULL; i++) {
      if (strcmp(p, hintVals[i].str) == 0) {
        *pVal |= hintVals[i].val;
        break;
      }
    }
    if (hintVals[i].str == NULL) {
      INTERNAL_ERROR_V("unknown config hint val in CHPL_RT_%s: \"%s\"",
                       evName, p);
    }
    p = strtok(NULL, "|");
    if (justOne && p != NULL) {
      INTERNAL_ERROR_V("too many config hint vals in CHPL_RT_%s=\"%s\"",
                       evName, ev);
    }
  }

  return true;
}


static
void debugOverrideHints(struct fi_info* hints) {
  #define CFG_HINT(s)    { #s, (uint64_t) (s) }
  #define CFG_HINT_NULL  { NULL, 0ULL }

  uint64_t val;

  {
    struct cfgHint hintVals[] = { CFG_HINT(FI_ATOMIC),
                                  CFG_HINT(FI_DIRECTED_RECV),
                                  CFG_HINT(FI_FENCE),
                                  CFG_HINT(FI_HMEM),
                                  CFG_HINT(FI_LOCAL_COMM),
                                  CFG_HINT(FI_MSG),
                                  CFG_HINT(FI_MULTICAST),
                                  CFG_HINT(FI_MULTI_RECV),
                                  CFG_HINT(FI_NAMED_RX_CTX),
                                  CFG_HINT(FI_READ),
                                  CFG_HINT(FI_RECV),
                                  CFG_HINT(FI_REMOTE_COMM),
                                  CFG_HINT(FI_REMOTE_READ),
                                  CFG_HINT(FI_REMOTE_WRITE),
                                  CFG_HINT(FI_RMA),
                                  CFG_HINT(FI_RMA_EVENT),
                                  CFG_HINT(FI_RMA_PMEM),
                                  CFG_HINT(FI_SEND),
                                  CFG_HINT(FI_SHARED_AV),
                                  CFG_HINT(FI_SOURCE),
                                  CFG_HINT(FI_SOURCE_ERR),
                                  CFG_HINT(FI_TAGGED),
                                  CFG_HINT(FI_TRIGGER),
                                  CFG_HINT(FI_VARIABLE_MSG),
                                  CFG_HINT(FI_WRITE), };
    if (getCfgHint("COMM_OFI_HINTS_CAPS",
                   hintVals, false /*justOne*/, &val)) {
      hints->caps = val;
    }
  }

  {
    struct cfgHint hintVals[] = { CFG_HINT(FI_COMMIT_COMPLETE),
                                  CFG_HINT(FI_COMPLETION),
                                  CFG_HINT(FI_DELIVERY_COMPLETE),
                                  CFG_HINT(FI_INJECT),
                                  CFG_HINT(FI_INJECT_COMPLETE),
                                  CFG_HINT(FI_TRANSMIT_COMPLETE),
                                  CFG_HINT_NULL, };
    if (getCfgHint("COMM_OFI_HINTS_TX_OP_FLAGS",
                   hintVals, false /*justOne*/, &val)) {
      hints->tx_attr->op_flags = val;
    }
  }

  {
    struct cfgHint hintVals[] = { CFG_HINT(FI_ORDER_ATOMIC_RAR),
                                  CFG_HINT(FI_ORDER_ATOMIC_RAW),
                                  CFG_HINT(FI_ORDER_ATOMIC_WAR),
                                  CFG_HINT(FI_ORDER_ATOMIC_WAW),
                                  CFG_HINT(FI_ORDER_NONE),
                                  CFG_HINT(FI_ORDER_RAR),
                                  CFG_HINT(FI_ORDER_RAS),
                                  CFG_HINT(FI_ORDER_RAW),
                                  CFG_HINT(FI_ORDER_RMA_RAR),
                                  CFG_HINT(FI_ORDER_RMA_RAW),
                                  CFG_HINT(FI_ORDER_RMA_WAR),
                                  CFG_HINT(FI_ORDER_RMA_WAW),
                                  CFG_HINT(FI_ORDER_SAR),
                                  CFG_HINT(FI_ORDER_SAS),
                                  CFG_HINT(FI_ORDER_SAW),
                                  CFG_HINT(FI_ORDER_WAR),
                                  CFG_HINT(FI_ORDER_WAS),
                                  CFG_HINT(FI_ORDER_WAW),
                                  CFG_HINT_NULL, };
    if (getCfgHint("COMM_OFI_HINTS_MSG_ORDER",
                   hintVals, false /*justOne*/, &val)) {
      hints->tx_attr->msg_order = hints->rx_attr->msg_order = val;
    }
  }

  {
    struct cfgHint hintVals[] = { CFG_HINT(FI_COMMIT_COMPLETE),
                                  CFG_HINT(FI_COMPLETION),
                                  CFG_HINT(FI_DELIVERY_COMPLETE),
                                  CFG_HINT(FI_MULTI_RECV),
                                  CFG_HINT_NULL, };
    if (getCfgHint("COMM_OFI_HINTS_RX_OP_FLAGS",
                   hintVals, false /*justOne*/, &val)) {
      hints->rx_attr->op_flags = val;
    }
  }

  {
    struct cfgHint hintVals[] = { CFG_HINT(FI_PROGRESS_UNSPEC),
                                  CFG_HINT(FI_PROGRESS_AUTO),
                                  CFG_HINT(FI_PROGRESS_MANUAL),
                                  CFG_HINT_NULL, };
    if (getCfgHint("COMM_OFI_HINTS_CONTROL_PROGRESS",
                   hintVals, true /*justOne*/, &val)) {
      hints->domain_attr->control_progress = (enum fi_progress) val;
    }
    if (getCfgHint("COMM_OFI_HINTS_DATA_PROGRESS",
                   hintVals, true /*justOne*/, &val)) {
      hints->domain_attr->data_progress = (enum fi_progress) val;
    }
  }

  {
    struct cfgHint hintVals[] = { CFG_HINT(FI_THREAD_UNSPEC),
                                  CFG_HINT(FI_THREAD_SAFE),
                                  CFG_HINT(FI_THREAD_FID),
                                  CFG_HINT(FI_THREAD_DOMAIN),
                                  CFG_HINT(FI_THREAD_COMPLETION),
                                  CFG_HINT(FI_THREAD_ENDPOINT),
                                  CFG_HINT_NULL, };
    if (getCfgHint("COMM_OFI_HINTS_THREADING",
                   hintVals, true /*justOne*/, &val)) {
      hints->domain_attr->threading = (enum fi_threading) val;
    }
  }

  {
    struct cfgHint hintVals[] = { CFG_HINT(FI_MR_UNSPEC),
                                  CFG_HINT(FI_MR_BASIC),
                                  CFG_HINT(FI_MR_SCALABLE),
                                  CFG_HINT(FI_MR_LOCAL),
                                  CFG_HINT(FI_MR_RAW),
                                  CFG_HINT(FI_MR_VIRT_ADDR),
                                  CFG_HINT(FI_MR_ALLOCATED),
                                  CFG_HINT(FI_MR_PROV_KEY),
                                  CFG_HINT(FI_MR_MMU_NOTIFY),
                                  CFG_HINT(FI_MR_RMA_EVENT),
                                  CFG_HINT(FI_MR_ENDPOINT),
                                  CFG_HINT(FI_MR_HMEM),
                                  CFG_HINT_NULL, };
    if (getCfgHint("COMM_OFI_HINTS_MR_MODE",
                   hintVals, false /*justOne*/, &val)) {
      hints->domain_attr->mr_mode = (int) val;
    }
  }

  #undef CFG_HINT
  #undef CFG_HINT_NULL
}
#endif


static inline
chpl_bool isInProvider(const char* s, struct fi_info* info) {
  return isInProvName(s, info->fabric_attr->prov_name);
}


static inline
chpl_bool isGoodCoreProvider(struct fi_info* info) {
  return (!isInProvName("sockets", info->fabric_attr->prov_name)
          && !isInProvName("tcp", info->fabric_attr->prov_name));
}


static inline
struct fi_info* findProvInList(struct fi_info* info,
                               chpl_bool skip_ungood_provs,
                               chpl_bool skip_RxD_provs,
                               chpl_bool skip_RxM_provs) {
  while (info != NULL
         && (   (skip_ungood_provs && !isGoodCoreProvider(info))
             || (skip_RxD_provs && isInProvider("ofi_rxd", info))
             || (skip_RxM_provs && isInProvider("ofi_rxm", info)))) {
    info = info->next;
  }
  return (info == NULL) ? NULL : fi_dupinfo(info);
}


static
struct fi_info* findProvider(struct fi_info** p_infoList,
                             struct fi_info* hints,
                             chpl_bool skip_RxD_provs,
                             chpl_bool skip_RxM_provs,
                             const char* feature) {
  chpl_bool skip_ungood_provs;

  if (hints != NULL) {
    int ret;
    OFI_CHK_2(fi_getinfo(COMM_OFI_FI_VERSION, NULL, NULL, 0, hints,
                         p_infoList),
              ret, -FI_ENODATA);
    skip_ungood_provs = (getProviderName() == NULL);
  } else {
    skip_ungood_provs = false;
  }

  struct fi_info* infoFound = NULL;
  if (*p_infoList != NULL
      && ((infoFound = findProvInList(*p_infoList, skip_ungood_provs,
                                      skip_RxD_provs, skip_RxM_provs))
          != NULL)) {
    DBG_PRINTF_NODE0(DBG_PROV,
                     "** found %sdesirable provider with %s",
                     (hints != NULL) ? "" : "less-", feature);
  } else {
    DBG_PRINTF_NODE0(DBG_PROV,
                     "** no %sdesirable provider with %s",
                     (hints != NULL) ? "" : "less-", feature);
  }

  return infoFound;
}


static
struct fi_info* findDlvrCmpltProv(struct fi_info** p_infoList,
                                  struct fi_info* hints) {
  //
  // We're looking for a provider that supports FI_DELIVERY_COMPLETE.
  // If we're given hints, then we don't have any candidates yet.  In
  // that case we're asked to get a provider list using those hints,
  // modified with delivery-complete, and from that select the first
  // "good" (or forced) provider, which is assumed to be the one that
  // will perform best.  Otherwise, we're just asked to find the best
  // less-good provider from the given list.
  //
  const char* prov_name = getProviderName();
  const chpl_bool forced_RxD = isInProvName("ofi_rxd", prov_name);
  const chpl_bool forced_RxM = isInProvName("ofi_rxm", prov_name);
  struct fi_info* infoFound;

  uint64_t op_flags_saved = 0;
  if (hints != NULL) {
    op_flags_saved = hints->tx_attr->op_flags;
    hints->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
  }

  infoFound = findProvider(p_infoList, hints,
                           !forced_RxD /*skip_RxD_provs*/,
                           !forced_RxM /*skip_RxM_provs*/,
                           "delivery-complete");

  if (hints != NULL) {
    hints->tx_attr->op_flags = op_flags_saved;
  }

  return infoFound;
}


static
struct fi_info* findMsgOrderProv(struct fi_info** p_infoList,
                                 struct fi_info* hints) {
  //
  // We're looking for a provider that supports the message orderings
  // that are sufficient for us to adhere to the MCM.  If we're given
  // hints, then we don't have any candidates yet.  In that case we're
  // asked to get a provider list using those hints, modified with the
  // needed message orderings, and from that select the first "good" (or
  // forced) provider, which is assumed to be the one that will perform
  // best.  Otherwise, we're just asked to find the best less-good
  // provider from the given list.
  //
  const char* prov_name = getProviderName();
  const chpl_bool forced_RxD = isInProvName("ofi_rxd", prov_name);
  struct fi_info* infoFound;

  uint64_t tx_msg_order_saved = 0;
  uint64_t rx_msg_order_saved = 0;
  if (hints != NULL) {
    tx_msg_order_saved = hints->tx_attr->msg_order;
    rx_msg_order_saved = hints->rx_attr->msg_order;
    hints->tx_attr->msg_order |= FI_ORDER_RAW  | FI_ORDER_WAW | FI_ORDER_SAW;
    hints->rx_attr->msg_order |= FI_ORDER_RAW  | FI_ORDER_WAW | FI_ORDER_SAW;
  }

  infoFound = findProvider(p_infoList, hints,
                           !forced_RxD /*skip_RxD_provs*/,
                           false /*skip_RxM_provs*/,
                           "message orderings");

  if (hints != NULL) {
    hints->tx_attr->msg_order = tx_msg_order_saved;
    hints->rx_attr->msg_order = rx_msg_order_saved;
  }

  return infoFound;
}


static
void init_ofiFabricDomain(void) {
  //
  // Build hints describing our fundamental requirements and get a list
  // of the providers that can satisfy those:
  // - capabilities:
  //   - messaging (send/receive), including multi-receive
  //   - RMA
  //   - transactions directed at both self and remote nodes
  //   - on Cray XC, atomics (gni provider doesn't volunteer this)
  // - tx endpoints:
  //   - default completion level
  //   - send-after-send ordering
  // - rx endpoints same as tx
  // - RDM endpoints
  // - domain threading model, since we manage thread contention ourselves
  // - resource management, to improve the odds we hear about exhaustion
  // - table-style address vectors
  // - in addition, include the memory registration modes we can support
  //
  const char* prov_name = getProviderName();
  struct fi_info* hints;
  CHK_TRUE((hints = fi_allocinfo()) != NULL);

  hints->caps = (FI_MSG | FI_MULTI_RECV
                 | FI_RMA | FI_LOCAL_COMM | FI_REMOTE_COMM);
  if ((strcmp(CHPL_TARGET_PLATFORM, "cray-xc") == 0
       && (prov_name == NULL || isInProvName("gni", prov_name)))
      || chpl_env_rt_get_bool("COMM_OFI_HINTS_CAPS_ATOMIC", false)) {
    hints->caps |= FI_ATOMIC;
  }
  hints->tx_attr->op_flags = FI_COMPLETION;
  hints->tx_attr->msg_order = FI_ORDER_SAS;
  hints->rx_attr->msg_order = hints->tx_attr->msg_order;
  hints->ep_attr->type = FI_EP_RDM;
  hints->domain_attr->threading = FI_THREAD_DOMAIN;
  hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
  hints->domain_attr->av_type = FI_AV_TABLE;

  hints->domain_attr->mr_mode = (  FI_MR_LOCAL
                                 | FI_MR_VIRT_ADDR
                                 | FI_MR_PROV_KEY // TODO: avoid pkey bcast?
                                 | FI_MR_ENDPOINT);
  if (chpl_numNodes > 1 && chpl_comm_getenvMaxHeapSize() > 0) {
    hints->domain_attr->mr_mode |= FI_MR_ALLOCATED;
  }

  chpl_bool ord_cmplt_forced = false;
#ifdef CHPL_COMM_DEBUG
  struct fi_info* hintsOrig = fi_dupinfo(hints);
  debugOverrideHints(hints);
  ord_cmplt_forced =
    hints->tx_attr->op_flags != hintsOrig->tx_attr->op_flags
    || hints->tx_attr->msg_order != hintsOrig->tx_attr->msg_order;
  fi_freeinfo(hintsOrig);
#endif

  DBG_PRINTF_NODE0(DBG_PROV_HINTS,
                   "====================\n"
                   "initial hints");
  DBG_PRINTF_NODE0(DBG_PROV_HINTS,
                   "%s", fi_tostr(hints, FI_TYPE_INFO));
  DBG_PRINTF_NODE0(DBG_PROV_HINTS,
                   "====================");

  //
  // To enable adhering to the Chapel MCM we need the following (within
  // each task, not across tasks):
  // - A PUT followed by a GET from the same address must return the
  //   PUT data.  For this we need read-after-write ordering or else
  //   delivery-complete.  Note that the RxM provider advertises
  //   delivery-complete but doesn't actually do it.
  // - When a PUT is following by an on-stmt, the on-stmt body must see
  //   the PUT data.  For this we need either send-after-write ordering
  //   or delivery-complete.
  // - Atomics have to be ordered if either is a write, whether they're
  //   done directly or via internal AMs.
  //
  // What we're hunting for is either a provider that can do all of the
  // above transaction orderings, or one that can do delivery-complete.
  // But we can't just get all the providers that match our fundamental
  // needs and then look through the list to find the first one that can
  // do either our transaction orderings or delivery-complete, because
  // if those weren't in our original hints they might not be expressed
  // by any of the returned providers.  Providers will not typically
  // "volunteer" capabilities that aren't asked for, especially if those
  // capabilities have performance costs.  So here, first see if we get
  // a "good" core provider when we hint at delivery-complete and then
  // (if needed) message ordering.  Then, if that doesn't succeed, we
  // settle for a not-so-good provider.  "Good" here means "neither tcp
  // nor sockets".  There are some wrinkles:
  // - Setting either the transaction orderings or the completion type
  //   in manually overridden hints causes those hints to be used as-is,
  //   turning off both the good-provider check and any attempt to find
  //   something sufficient for the MCM.
  // - Setting the FI_PROVIDER environment variable to manually specify
  //   a provider turns off the good-provider checks.
  // - We can't accept the RxM utility provider with any core provider
  //   for delivery-complete, because although RxM will match that it
  //   cannot actually do it, and programs will fail.  This is a known
  //   bug that can't be fixed without breaking other things:
  //     https://github.com/ofiwg/libfabric/issues/5601
  //   Explicitly including ofi_rxm in FI_PROVIDER overrides this.
  //

  int ret;

  //
  // Take manually overridden hints as forcing provider selection if
  // they adjust either the transaction orderings or completion type.
  // Otherwise, just flow those overrides into the selection process
  // below.
  //
  if (ord_cmplt_forced) {
    OFI_CHK_2(fi_getinfo(COMM_OFI_FI_VERSION, NULL, NULL, 0, hints, &ofi_info),
              ret, -FI_ENODATA);
    if (ret != FI_SUCCESS) {
      INTERNAL_ERROR_V_NODE0("No (forced) provider for prov_name \"%s\"",
                             (prov_name == NULL) ? "<any>" : prov_name);
    }
  }

  //
  // Try to find a good provider, then settle for a not-so-good one. By
  // default try delivery-complete first, then message ordering, but
  // allow that order to be swapped via the environment.
  //
  if (ofi_info == NULL) {
    const chpl_bool
      preferDlvrCmplt = chpl_env_rt_get_bool("COMM_OFI_DO_DELIVERY_COMPLETE",
                                             true);
    struct {
      struct fi_info* (*fn)(struct fi_info**, struct fi_info*);
      struct fi_info* infoList;
    } capTry[] = { { findDlvrCmpltProv, NULL },
                   { findMsgOrderProv, NULL }, };
    size_t capTryLen = sizeof(capTry) / sizeof(capTry[0]);

    if (!preferDlvrCmplt) {
      capTry[0].fn = findMsgOrderProv;
      capTry[1].fn = findDlvrCmpltProv;
    }

    // Search for a good provider.
    for (int i = 0; ofi_info == NULL && i < capTryLen; i++) {
      ofi_info = (*capTry[i].fn)(&capTry[i].infoList, hints);
    }

    // If necessary, search for a less-good provider.
    for (int i = 0; ofi_info == NULL && i < capTryLen; i++) {
      ofi_info = (*capTry[i].fn)(&capTry[i].infoList, NULL);
    }

    // ofi_info has the result; free intermediate list(s).
    for (int i = 0; i < capTryLen && capTry[i].infoList != NULL; i++) {
      fi_freeinfo(capTry[i].infoList);
    }
  }

  if (ofi_info == NULL) {
    //
    // We didn't find any provider at all.
    // NOTE: execution ends here.
    //
    INTERNAL_ERROR_V_NODE0("No libfabric provider for prov_name \"%s\"",
                           (prov_name == NULL) ? "<any>" : prov_name);
  }

  //
  // If we get here, we have a provider in ofi_info.
  //
  fi_freeinfo(hints);

  haveDeliveryComplete =
    (ofi_info->tx_attr->op_flags & FI_DELIVERY_COMPLETE) != 0;

  if (DBG_TEST_MASK(DBG_PROV_ALL)) {
    if (chpl_nodeID == 0) {
      DBG_PRINTF(DBG_PROV_ALL,
                 "====================\n"
                 "matched fabric(s):");
      struct fi_info* info;
      for (info = ofi_info; info != NULL; info = info->next) {
        DBG_PRINTF(DBG_PROV_ALL, "%s", fi_tostr(ofi_info, FI_TYPE_INFO));
      }
    }
  } else {
    DBG_PRINTF_NODE0(DBG_PROV,
                     "====================\n"
                     "matched fabric:");
    DBG_PRINTF_NODE0(DBG_PROV, "%s", fi_tostr(ofi_info, FI_TYPE_INFO));
  }

  DBG_PRINTF_NODE0(DBG_PROV | DBG_PROV_ALL,
                   "====================");

  if (verbosity >= 2) {
    if (chpl_nodeID == 0) {
      printf("COMM=ofi: using \"%s\" provider\n",
             ofi_info->fabric_attr->prov_name);
    }
  }

  //
  // Create the fabric domain and associated fabric access domain.
  //
  OFI_CHK(fi_fabric(ofi_info->fabric_attr, &ofi_fabric, NULL));
  OFI_CHK(fi_domain(ofi_fabric, ofi_info, &ofi_domain, NULL));
}


static
void init_ofiDoProviderChecks(void) {
  //
  // Set/compute various provider-specific things.
  //
  if (providerInUse(provType_gni)) {
    //
    // gni
    //
    // If there were questionable settings associated with the fixed
    // heap on a Cray XC system, say something about that now.
    //
    emit_delayedFixedHeapMsgs();
  }

  if (providerInUse(provType_rxd)) {
    //
    // ofi_rxd (utility provider with tcp, verbs, possibly others)
    //
    // - Based on tracebacks after internal error aborts, RxD seems to
    //   want to record an address per accessing endpoint for at least
    //   some AVs (perhaps just those for which it handles progress?).
    //   It uses the AV attribute 'count' member to size the data
    //   structure in which it stores those.  So, that member will need
    //   to account for all transmitting endpoints.
    //
    provCtl_sizeAvsByNumEps = true;
  }

  //
  // RxD and perhaps other providers must have a non-NULL buf arg for
  // fi_fetch_atomic(FI_ATOMIC_READ) or they segfault, even though the
  // fi_atomic man page says buf is ignored for that operation and may
  // be NULL.
  //
  provCtl_readAmoNeedsOpnd = true;
}


static
void init_ofiEp(void) {
  //
  // The AM handler is responsible not only for AM handling and progress
  // on any RMA it initiates but also progress on inbound RMA, if that
  // is needed.  It uses poll and wait sets to manage this, if it can.
  // Note: we'll either have both a poll and a wait set, or neither.
  //
  // We don't use poll and wait sets with the efa provider because that
  // doesn't support wait objects.  I tried just setting the cq_attr
  // wait object to FI_WAIT_UNSPEC for all providers, since we don't
  // reference the wait object explicitly anyway, but then saw hangs
  // with (at least) the tcp;ofi_rxm provider.
  //
  // We don't use poll and wait sets with the gni provider because (1)
  // it returns -ENOSYS for fi_poll_open() and (2) although a wait set
  // seems to work properly during execution, we haven't found a way to
  // avoid getting -FI_EBUSY when we try to close it.
  //
  if (!providerInUse(provType_efa)
      && !providerInUse(provType_gni)) {
    int ret;
    struct fi_poll_attr pollSetAttr = (struct fi_poll_attr)
                                      { .flags = 0, };
    OFI_CHK_2(fi_poll_open(ofi_domain, &pollSetAttr, &ofi_amhPollSet),
              ret, -FI_ENOSYS);
    if (ret == FI_SUCCESS) {
      struct fi_wait_attr waitSetAttr = (struct fi_wait_attr)
                                        { .wait_obj = FI_WAIT_UNSPEC, };
      OFI_CHK_2(fi_wait_open(ofi_fabric, &waitSetAttr, &ofi_amhWaitSet),
                ret, -FI_ENOSYS);
      if (ret != FI_SUCCESS) {
        ofi_amhPollSet = NULL;
        ofi_amhWaitSet = NULL;
      }
    } else {
      ofi_amhPollSet = NULL;
    }
  }

  //
  // Compute numbers of transmit and receive contexts, and then create
  // the transmit context table.
  //
  useScalableTxEp = (ofi_info->domain_attr->max_ep_tx_ctx > 1
                     && chpl_env_rt_get_bool("COMM_OFI_USE_SCALABLE_EP",
                                             true));
  init_ofiEpNumCtxs();

  tciTabLen = numTxCtxs;
  CHPL_CALLOC(tciTab, tciTabLen);

  //
  // Create transmit contexts.
  //

  //
  // For the CQ lengths, allow for whichever maxOutstanding (AMs or
  // RMAs) value is larger, plus quite a few for AM responses because
  // the network round-trip latency ought to be quite a bit more than
  // our AM handling time, so we want to be able to have many responses
  // in flight at once.
  //
  struct fi_av_attr avAttr = (struct fi_av_attr)
                             { .type = FI_AV_TABLE,
                               .count = chpl_numNodes * 2 /* AM, RMA+AMO */,
                               .name = NULL,
                               .rx_ctx_bits = 0, };
  if (provCtl_sizeAvsByNumEps) {
    // Workaround for RxD peculiarity.
    avAttr.count *= numTxCtxs;
  }

  OFI_CHK(fi_av_open(ofi_domain, &avAttr, &ofi_av, NULL));

  if (useScalableTxEp) {
    //
    // Use a scalable transmit endpoint and multiple tx contexts.  Make
    // just one address vector, in the first tciTab[] entry.  The others
    // will be synonyms for that one, to make the references easier.
    //
    OFI_CHK(fi_scalable_ep(ofi_domain, ofi_info, &ofi_txEpScal, NULL));
    OFI_CHK(fi_scalable_ep_bind(ofi_txEpScal, &ofi_av->fid, 0));
  } else {
    //
    // Use regular transmit endpoints; see below.
    //
  }

  //
  // Worker TX contexts need completion queues, so they can tell what
  // kinds of things are completing.
  //
  const int numWorkerTxCtxs = tciTabLen - numAmHandlers;
  struct fi_cq_attr cqAttr;
  struct fi_cntr_attr cntrAttr;

  {
    cqAttr = (struct fi_cq_attr)
             { .format = FI_CQ_FORMAT_MSG,
               .size = 100 + MAX_TXNS_IN_FLIGHT,
               .wait_obj = FI_WAIT_NONE, };
    txCQLen = cqAttr.size;
    for (int i = 0; i < numWorkerTxCtxs; i++) {
      init_ofiEpTxCtx(i, false /*isAMHandler*/, &cqAttr, NULL);
    }
  }

  //
  // TX contexts for the AM handler(s) can just use counters, if the
  // provider supports them.  Otherwise, they have to use CQs also.
  //
  const enum fi_wait_obj waitObj = (ofi_amhWaitSet == NULL)
                                   ? FI_WAIT_NONE
                                   : FI_WAIT_SET;
  if (true /*ofi_info->domain_attr->cntr_cnt == 0*/) { // disable tx counters
    cqAttr = (struct fi_cq_attr)
             { .format = FI_CQ_FORMAT_MSG,
               .size = 100,
               .wait_obj = waitObj,
               .wait_cond = FI_CQ_COND_NONE,
               .wait_set = ofi_amhWaitSet, };
    for (int i = numWorkerTxCtxs; i < tciTabLen; i++) {
      init_ofiEpTxCtx(i, true /*isAMHandler*/, &cqAttr, NULL);
    }
  } else {
    cntrAttr = (struct fi_cntr_attr)
               { .events = FI_CNTR_EVENTS_COMP,
                 .wait_obj = waitObj,
                 .wait_set = ofi_amhWaitSet, };
    for (int i = numWorkerTxCtxs; i < tciTabLen; i++) {
      init_ofiEpTxCtx(i, true /*isAMHandler*/, NULL, &cntrAttr);
    }
  }

  //
  // Create receive contexts.
  //
  // For the CQ length, allow for an appreciable proportion of the job
  // to send requests to us at once.
  //
  cqAttr = (struct fi_cq_attr)
           { .size = chpl_numNodes * numWorkerTxCtxs,
             .format = FI_CQ_FORMAT_DATA,
             .wait_obj = waitObj,
             .wait_cond = FI_CQ_COND_NONE,
             .wait_set = ofi_amhWaitSet, };
  cntrAttr = (struct fi_cntr_attr)
             { .events = FI_CNTR_EVENTS_COMP,
               .wait_obj = waitObj,
               .wait_set = ofi_amhWaitSet, };

  OFI_CHK(fi_endpoint(ofi_domain, ofi_info, &ofi_rxEp, NULL));
  OFI_CHK(fi_ep_bind(ofi_rxEp, &ofi_av->fid, 0));
  OFI_CHK(fi_cq_open(ofi_domain, &cqAttr, &ofi_rxCQ, &ofi_rxCQ));
  OFI_CHK(fi_ep_bind(ofi_rxEp, &ofi_rxCQ->fid, FI_TRANSMIT | FI_RECV));
  OFI_CHK(fi_enable(ofi_rxEp));

  OFI_CHK(fi_endpoint(ofi_domain, ofi_info, &ofi_rxEpRma, NULL));
  OFI_CHK(fi_ep_bind(ofi_rxEpRma, &ofi_av->fid, 0));
  if (true /*ofi_info->domain_attr->cntr_cnt == 0*/) { // disable tx counters
    OFI_CHK(fi_cq_open(ofi_domain, &cqAttr, &ofi_rxCQRma,
                       &checkRxRmaCmplsFn));
    ofi_rxCmplFidRma = &ofi_rxCQRma->fid;
    checkRxRmaCmplsFn = checkRxRmaCmplsCQ;
  } else {
    OFI_CHK(fi_cntr_open(ofi_domain, &cntrAttr, &ofi_rxCntrRma,
                         &checkRxRmaCmplsFn));
    ofi_rxCmplFidRma = &ofi_rxCntrRma->fid;
    checkRxRmaCmplsFn = checkRxRmaCmplsCntr;
  }
  OFI_CHK(fi_ep_bind(ofi_rxEpRma, ofi_rxCmplFidRma, FI_TRANSMIT | FI_RECV));
  OFI_CHK(fi_enable(ofi_rxEpRma));

  //
  // If we're using poll and wait sets, put all the progress-related
  // CQs and/or counters in the poll set.
  //
  if (ofi_amhPollSet != NULL) {
    OFI_CHK(fi_poll_add(ofi_amhPollSet, &ofi_rxCQ->fid, 0));
    OFI_CHK(fi_poll_add(ofi_amhPollSet, ofi_rxCmplFidRma, 0));
    OFI_CHK(fi_poll_add(ofi_amhPollSet, tciTab[tciTabLen - 1].txCmplFid, 0));
    pollSetSize = 3;
  }
}


static
void init_ofiEpNumCtxs(void) {
  CHK_TRUE(numAmHandlers == 1); // force reviewing this if #AM handlers changes

  //
  // Note for future maintainers: if interoperability between Chapel
  // and other languages someday results in non-tasking layer threads
  // calling Chapel code which then tries to communicate across nodes,
  // then some of this may have to be adjusted, especially e.g. the
  // tciTabFixedAssignments part.
  //

  //
  // Start with the maximum number of transmit contexts.  We'll reduce
  // the number incrementally as we discover we don't need that many.
  // Initially, just make sure there are enough for each AM handler to
  // have its own, plus at least one more.
  //
  const struct fi_domain_attr* dom_attr = ofi_info->domain_attr;
  int maxWorkerTxCtxs;
  if (useScalableTxEp)
    maxWorkerTxCtxs = dom_attr->max_ep_tx_ctx - numAmHandlers;
  else
    maxWorkerTxCtxs = dom_attr->ep_cnt - numAmHandlers;

  CHK_TRUE(maxWorkerTxCtxs > 0);

  //
  // If the user manually limited the communication concurrency, take
  // that into account.
  //
  const int commConcurrency = chpl_env_rt_get_int("COMM_CONCURRENCY", 0);
  if (commConcurrency > 0) {
    if (maxWorkerTxCtxs > commConcurrency) {
      maxWorkerTxCtxs = commConcurrency;
    }
  } else if (commConcurrency < 0) {
    chpl_warning("CHPL_RT_COMM_CONCURRENCY < 0, ignored", 0, 0);
  }

  const int fixedNumThreads = chpl_task_getFixedNumThreads();
  if (fixedNumThreads > 0) {
    //
    // The tasking layer uses a fixed number of threads.  If we can
    // have at least that many worker tx contexts, plus 1 for threads
    // that aren't fixed workers (like the process itself for example),
    // then each tasking layer fixed thread can have a private context
    // for the duration of the run.
    //
    CHK_TRUE(fixedNumThreads == chpl_task_getMaxPar()); // sanity
    if (maxWorkerTxCtxs > fixedNumThreads + 1)
      maxWorkerTxCtxs = fixedNumThreads + 1;
    tciTabFixedAssignments = (maxWorkerTxCtxs == fixedNumThreads + 1);
  } else {
    //
    // The tasking layer doesn't have a fixed number of threads, but
    // it still must have a maximum useful level of parallelism.  We
    // shouldn't need more worker tx contexts than whatever that is.
    //
    const int taskMaxPar = chpl_task_getMaxPar();
    if (maxWorkerTxCtxs > taskMaxPar)
      maxWorkerTxCtxs = taskMaxPar;

    tciTabFixedAssignments = false;
  }

  //
  // Now we know how many transmit contexts we'll have.
  //
  numTxCtxs = maxWorkerTxCtxs + numAmHandlers;
  if (useScalableTxEp) {
    ofi_info->ep_attr->tx_ctx_cnt = numTxCtxs;
  }

  //
  // Receive contexts are much easier -- we just need one
  // for each AM handler.
  //
  CHK_TRUE(dom_attr->max_ep_rx_ctx >= numAmHandlers);
  numRxCtxs = numAmHandlers;
}


static
void init_ofiEpTxCtx(int i, chpl_bool isAMHandler,
                     struct fi_cq_attr* cqAttr,
                     struct fi_cntr_attr* cntrAttr) {
  struct perTxCtxInfo_t* tcip = &tciTab[i];
  atomic_init_bool(&tcip->allocated, false);
  tcip->bound = false;

  if (useScalableTxEp) {
    OFI_CHK(fi_tx_context(ofi_txEpScal, i, NULL, &tcip->txCtx, NULL));
  } else {
    OFI_CHK(fi_endpoint(ofi_domain, ofi_info, &tcip->txCtx, NULL));
    OFI_CHK(fi_ep_bind(tcip->txCtx, &ofi_av->fid, 0));
  }

  if (cqAttr != NULL) {
    OFI_CHK(fi_cq_open(ofi_domain, cqAttr, &tcip->txCQ,
                       &tcip->checkTxCmplsFn));
    tcip->txCmplFid = &tcip->txCQ->fid;
    OFI_CHK(fi_ep_bind(tcip->txCtx, tcip->txCmplFid, FI_TRANSMIT | FI_RECV));
    tcip->checkTxCmplsFn = checkTxCmplsCQ;
  } else {
    OFI_CHK(fi_cntr_open(ofi_domain, cntrAttr, &tcip->txCntr,
                         &tcip->checkTxCmplsFn));
    tcip->txCmplFid = &tcip->txCntr->fid;
    OFI_CHK(fi_ep_bind(tcip->txCtx, tcip->txCmplFid,
                       FI_SEND | FI_READ | FI_WRITE));
    tcip->checkTxCmplsFn = checkTxCmplsCntr;
  }

  OFI_CHK(fi_enable(tcip->txCtx));

  tcip->ensureProgressFn = isAMHandler
                           ? amEnsureProgress
                           : tcip->checkTxCmplsFn;

}


static
void init_ofiExchangeAvInfo(void) {
  //
  // Exchange addresses with the rest of the nodes.
  //

  //
  // Get everybody else's address.
  // Note: this assumes all addresses, job-wide, are the same length.
  //
  if (DBG_TEST_MASK(DBG_CFG_AV)) {
    //
    // Sanity-check our same-address-length assumption.
    //
    size_t len = 0;
    size_t lenRma = 0;

    OFI_CHK_1(fi_getname(&ofi_rxEp->fid, NULL, &len), -FI_ETOOSMALL);
    OFI_CHK_1(fi_getname(&ofi_rxEpRma->fid, NULL, &lenRma), -FI_ETOOSMALL);
    CHK_TRUE(len == lenRma);

    size_t* lens;
    CHPL_CALLOC(lens, chpl_numNodes);
    chpl_comm_ofi_oob_allgather(&len, lens, sizeof(len));
    if (chpl_nodeID == 0) {
      for (int i = 0; i < chpl_numNodes; i++) {
        CHK_TRUE(lens[i] == len);
      }
    }
  }

  char* my_addr;
  char* addrs;
  size_t my_addr_len = 0;

  OFI_CHK_1(fi_getname(&ofi_rxEp->fid, NULL, &my_addr_len), -FI_ETOOSMALL);
  CHPL_CALLOC_SZ(my_addr, 2 * my_addr_len, 1);
  OFI_CHK(fi_getname(&ofi_rxEp->fid, my_addr, &my_addr_len));
  OFI_CHK(fi_getname(&ofi_rxEpRma->fid, my_addr + my_addr_len, &my_addr_len));
  CHPL_CALLOC_SZ(addrs, chpl_numNodes, 2 * my_addr_len);
  if (DBG_TEST_MASK(DBG_CFG_AV)) {
    char nameBuf[128];
    size_t nameLen;
    nameLen = sizeof(nameBuf);
    char nameBuf2[128];
    size_t nameLen2;
    nameLen2 = sizeof(nameBuf2);
    (void) fi_av_straddr(ofi_av, my_addr, nameBuf, &nameLen);
    (void) fi_av_straddr(ofi_av, my_addr + my_addr_len, nameBuf2, &nameLen2);
    DBG_PRINTF(DBG_CFG_AV, "my_addrs: %.*s%s, %.*s%s",
               (int) nameLen, nameBuf,
               (nameLen <= sizeof(nameBuf)) ? "" : "[...]",
               (int) nameLen2, nameBuf2,
               (nameLen2 <= sizeof(nameBuf2)) ? "" : "[...]");
  }
  chpl_comm_ofi_oob_allgather(my_addr, addrs, 2 * my_addr_len);

  //
  // Insert the addresses into the address vector and build up a vector
  // of remote receive endpoints.
  //
  // All the transmit context table entries have address vectors and we
  // always use the one associated with our tx context.  But if we have
  // a scalable endpoint then all of those AVs are really the same one.
  // Only when the provider cannot support scalable EPs and we have
  // multiple actual endpoints are the AVs individualized to those.
  //
  CHPL_CALLOC(ofi_rxAddrs, 2 * chpl_numNodes);
  CHK_TRUE(fi_av_insert(ofi_av, addrs, 2 * chpl_numNodes, ofi_rxAddrs, 0, NULL)
           == 2 * chpl_numNodes);

  CHPL_FREE(my_addr);
  CHPL_FREE(addrs);
}


static
void init_ofiForMem(void) {
  void* fixedHeapStart;
  size_t fixedHeapSize;
  chpl_comm_impl_regMemHeapInfo(&fixedHeapStart, &fixedHeapSize);

  //
  // We default to scalable registration if none of the settings that
  // force basic registration are present, but the user can override
  // that by specifying use of a fixed heap.  Note that this is to
  // some extent just a backstop, because if the user does specify a
  // fixed heap we will have earlier included FI_MR_ALLOCATED in our
  // hints, which might well have caused the selection of a provider
  // which requires basic registration.
  //
  const uint64_t basicMemRegBits = (FI_MR_BASIC
                                    | FI_MR_LOCAL
                                    | FI_MR_VIRT_ADDR
                                    | FI_MR_ALLOCATED
                                    | FI_MR_PROV_KEY);
  scalableMemReg = ((ofi_info->domain_attr->mr_mode & basicMemRegBits) == 0
                    && fixedHeapSize == 0);

  //
  // With scalable memory registration we just register the whole
  // address space here; with non-scalable we register each region
  // individually.  Currently with non-scalable we actually only
  // register a fixed heap.  We may do something more complicated
  // in the future, though.
  //
  if (scalableMemReg) {
    numMemRegions = 1;
    memTab[0].addr = (void*) 0;
    memTab[0].base = 0;
    memTab[0].size = SIZE_MAX;
  } else {
    if (fixedHeapSize == 0) {
      INTERNAL_ERROR_V("must specify fixed heap with %s provider",
                       ofi_info->fabric_attr->prov_name);
    }

    numMemRegions = 1;
    memTab[0].addr = fixedHeapStart;
    memTab[0].base = ((ofi_info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) == 0)
                     ? (size_t) fixedHeapStart
                     : (size_t) 0;
    memTab[0].size = fixedHeapSize;
  }

  const chpl_bool prov_key =
    ((ofi_info->domain_attr->mr_mode & FI_MR_PROV_KEY) != 0);

  uint64_t bufAcc = FI_RECV | FI_REMOTE_READ | FI_REMOTE_WRITE;
  if ((ofi_info->domain_attr->mr_mode & FI_MR_LOCAL) != 0) {
    bufAcc |= FI_SEND | FI_READ | FI_WRITE;
  }

  for (int i = 0; i < numMemRegions; i++) {
    DBG_PRINTF(DBG_MR, "[%d] fi_mr_reg(%p, %#zx, %#" PRIx64 ")",
               i, memTab[i].addr, memTab[i].size, bufAcc);
    OFI_CHK(fi_mr_reg(ofi_domain,
                      memTab[i].addr, memTab[i].size,
                      bufAcc, (prov_key ? 0 : i), 0, 0, &ofiMrTab[i], NULL));
    memTab[i].desc = fi_mr_desc(ofiMrTab[i]);
    memTab[i].key  = fi_mr_key(ofiMrTab[i]);
    CHK_TRUE(prov_key || memTab[i].key == i);
    DBG_PRINTF(DBG_MR, "[%d]     key %#" PRIx64, i, memTab[i].key);
    if ((ofi_info->domain_attr->mr_mode & FI_MR_ENDPOINT) != 0) {
      OFI_CHK(fi_mr_bind(ofiMrTab[i], &ofi_rxEpRma->fid, 0));
      OFI_CHK(fi_mr_enable(ofiMrTab[i]));
    }
  }

  //
  // Unless we're doing scalable registration of the entire address
  // space, share the memory regions around the job.
  //
  if (!scalableMemReg) {
    CHPL_CALLOC(memTabMap, chpl_numNodes);
    chpl_comm_ofi_oob_allgather(&memTab, memTabMap, sizeof(memTabMap[0]));
  }
}


static int isAtomicValid(enum fi_datatype);

static
void init_ofiForRma(void) {
  //
  // We need to make an initial call to isAtomicValid() to let it
  // initialize its internals.  The datatype here doesn't matter.
  //
  (void) isAtomicValid(FI_INT32);
}


static void init_amHandling(void);

static
void init_ofiForAms(void) {
  //
  // Compute the amount of space we should allow for AM landing zones.
  // We should have enough that we needn't re-post the multi-receive
  // buffer more often than, say, every tenth of a second.  We know from
  // the Chapel performance/comm/low-level/many-to-one test that the
  // comm=ugni AM handler can handle just over 150k "fast" AM requests
  // in 0.1 sec.  Assuming an average AM request size of 256 bytes, a 40
  // MiB buffer is enough to give us the desired 0.1 sec lifetime before
  // it needs renewing.  We actually then split this in half and create
  // 2 half-sized buffers (see below), so reflect that here also.
  //
  const size_t amLZSize = ((size_t) 40 << 20) / 2;

  //
  // Set the minimum multi-receive buffer space.  Make it big enough to
  // hold a max-sized request from every potential sender, but no more
  // than 10% of the buffer size.  Some providers don't have fi_setopt()
  // for some ep types, so allow this to fail in that case.  But note
  // that if it does fail and we get overruns we'll die or, worse yet,
  // silently compute wrong results.
  //
  {
    size_t sz = chpl_numNodes * tciTabLen * sizeof(struct amRequest_execOn_t);
    if (sz > amLZSize / 10) {
        sz = amLZSize / 10;
    }
    int ret;
    OFI_CHK_2(fi_setopt(&ofi_rxEp->fid, FI_OPT_ENDPOINT,
                        FI_OPT_MIN_MULTI_RECV, &sz, sizeof(sz)),
              ret, -FI_ENOSYS);
  }

  //
  // Pre-post multi-receive buffer for inbound AM requests.  In reality
  // set up two of these and swap back and forth between them, to hedge
  // against receiving "buffer filled and released" events out of order
  // with respect to the messages stored within them.
  //
  CHPL_CALLOC_SZ(amLZs[0], 1, amLZSize);
  CHPL_CALLOC_SZ(amLZs[1], 1, amLZSize);

  ofi_iov_reqs[0] = (struct iovec) { .iov_base = amLZs[0],
                                     .iov_len = amLZSize, };
  ofi_iov_reqs[1] = (struct iovec) { .iov_base = amLZs[1],
                                     .iov_len = amLZSize, };
  ofi_msg_reqs[0] = (struct fi_msg) { .msg_iov = &ofi_iov_reqs[0],
                                      .desc = NULL,
                                      .iov_count = 1,
                                      .addr = FI_ADDR_UNSPEC,
                                      .context = txnTrkEncodeId(__LINE__),
                                      .data = 0x0, };
  ofi_msg_reqs[1] = (struct fi_msg) { .msg_iov = &ofi_iov_reqs[1],
                                      .desc = NULL,
                                      .iov_count = 1,
                                      .addr = FI_ADDR_UNSPEC,
                                      .context = txnTrkEncodeId(__LINE__),
                                      .data = 0x0, };
  ofi_msg_i = 0;
  OFI_CHK(fi_recvmsg(ofi_rxEp, &ofi_msg_reqs[ofi_msg_i], FI_MULTI_RECV));
  DBG_PRINTF(DBG_AM_BUF,
             "pre-post fi_recvmsg(AMLZs %p, len %#zx)",
             ofi_msg_reqs[ofi_msg_i].msg_iov->iov_base,
             ofi_msg_reqs[ofi_msg_i].msg_iov->iov_len);

  init_amHandling();
}


#if 0
static
void init_ofiPerThread(void) {
}
#endif


void chpl_comm_rollcall(void) {
  DBG_PRINTF(DBG_IFACE_SETUP, "%s()", __func__);

  // Initialize diags
  chpl_comm_diags_init();

  chpl_msg(2, "executing on node %d of %d node(s): %s\n", chpl_nodeID,
           chpl_numNodes, chpl_nodeName());

  //
  // Only node 0 in multi-node programs does liveness checks, and only
  // after we're sure all the other nodes' AM handlers are running.
  //
  if (chpl_numNodes > 1 && chpl_nodeID == 0) {
    amDoLivenessChecks = true;
  }
}


//
// Chapel global and private variable support
//

wide_ptr_t* chpl_comm_broadcast_global_vars_helper(void) {
  DBG_PRINTF(DBG_IFACE_SETUP, "%s()", __func__);

  //
  // Gather the global variables' wide pointers on node 0 into a
  // buffer, and broadcast the address of that buffer to the other
  // nodes.
  //
  wide_ptr_t* buf;
  if (chpl_nodeID == 0) {
    CHPL_CALLOC(buf, chpl_numGlobalsOnHeap);
    for (int i = 0; i < chpl_numGlobalsOnHeap; i++) {
      buf[i] = *chpl_globals_registry[i];
    }
  }
  chpl_comm_ofi_oob_bcast(&buf, sizeof(buf));
  return buf;
}


static void*** chplPrivBcastTabMap;

static
void init_broadcast_private(void) {
  //
  //
  // Share the nodes' private broadcast tables around.  These are
  // needed by chpl_comm_broadcast_private(), below.
  //
  void** pbtMap;
  size_t pbtSize = chpl_rt_priv_bcast_tab_len
                   * sizeof(chpl_rt_priv_bcast_tab[0]);
  CHPL_CALLOC(pbtMap, chpl_numNodes * pbtSize);
  chpl_comm_ofi_oob_allgather(chpl_rt_priv_bcast_tab, pbtMap, pbtSize);
  CHPL_CALLOC(chplPrivBcastTabMap, chpl_numNodes);
  for (int i = 0; i < chpl_numNodes; i++) {
    chplPrivBcastTabMap[i] = &pbtMap[i * chpl_rt_priv_bcast_tab_len];
  }
}


void chpl_comm_broadcast_private(int id, size_t size) {
  DBG_PRINTF(DBG_IFACE_SETUP, "%s(%d, %zd)", __func__, id, size);

  for (int i = 0; i < chpl_numNodes; i++) {
    if (i != chpl_nodeID) {
      (void) ofi_put(chpl_rt_priv_bcast_tab[id], i,
                     chplPrivBcastTabMap[i][id], size);
    }
  }
}


////////////////////////////////////////
//
// Interface: shutdown
//

static void exit_all(int);
static void exit_any(int);

static void fini_amHandling(void);
static void fini_ofi(void);

static void amRequestShutdown(c_nodeid_t);

void chpl_comm_pre_task_exit(int all) {
  DBG_PRINTF(DBG_IFACE_SETUP, "%s(%d)", __func__, all);

  if (all) {
    if (chpl_nodeID == 0) {
      for (int node = 1; node < chpl_numNodes; node++) {
        amRequestShutdown(node);
      }
    } else {
      chpl_wait_for_shutdown();
    }

    chpl_comm_barrier("chpl_comm_pre_task_exit");
    fini_amHandling();
  }
}


void chpl_comm_exit(int all, int status) {
  DBG_PRINTF(DBG_IFACE_SETUP, "%s(%d, %d)", __func__, all, status);

  if (all) {
    exit_all(status);
  } else {
    exit_any(status);
  }
}


static void exit_all(int status) {
  fini_ofi();
  chpl_comm_ofi_oob_fini();
}


static void exit_any(int status) {
  //
  // (Over)abundance of caution mode: if exiting unilaterally with the
  // 'verbs' provider in use, call _exit() now instead of allowing the
  // usual runtime control flow to call exit() and invoke the atexit(3)
  // functions.  Otherwise we run the risk of segfaulting due to some
  // broken destructor code in librdmacm.  That was fixed years ago by
  //     https://github.com/linux-rdma/rdma-core/commit/9ef8ed2
  // but the fix doesn't seem to have made it into general circulation
  // yet.
  //
  // Flush all the stdio FILE streams first, in the hope of not losing
  // any output.
  //
  if (providerInUse(provType_verbs)) {
    fflush(NULL);
    _exit(status);
  }
}


static
void fini_ofi(void) {
  if (chpl_numNodes <= 1)
    return;

  for (int i = 0; i < numMemRegions; i++) {
    OFI_CHK(fi_close(&ofiMrTab[i]->fid));
  }

  if (memTabMap != NULL) {
    CHPL_FREE(memTabMap);
  }

  CHPL_FREE(amLZs[1]);
  CHPL_FREE(amLZs[0]);

  CHPL_FREE(ofi_rxAddrs);

  if (ofi_amhPollSet != NULL) {
    OFI_CHK(fi_poll_del(ofi_amhPollSet, tciTab[tciTabLen - 1].txCmplFid, 0));
    OFI_CHK(fi_poll_del(ofi_amhPollSet, ofi_rxCmplFidRma, 0));
    OFI_CHK(fi_poll_del(ofi_amhPollSet, &ofi_rxCQ->fid, 0));
  }

  OFI_CHK(fi_close(&ofi_rxEp->fid));
  OFI_CHK(fi_close(&ofi_rxCQ->fid));
  OFI_CHK(fi_close(&ofi_rxEpRma->fid));
  OFI_CHK(fi_close(ofi_rxCmplFidRma));

  for (int i = 0; i < tciTabLen; i++) {
    OFI_CHK(fi_close(&tciTab[i].txCtx->fid));
    OFI_CHK(fi_close(tciTab[i].txCmplFid));
  }

  if (useScalableTxEp) {
    OFI_CHK(fi_close(&ofi_txEpScal->fid));
  }

  OFI_CHK(fi_close(&ofi_av->fid));

  if (ofi_amhPollSet != NULL) {
    OFI_CHK(fi_close(&ofi_amhWaitSet->fid));
    OFI_CHK(fi_close(&ofi_amhPollSet->fid));
  }

  OFI_CHK(fi_close(&ofi_domain->fid));
  OFI_CHK(fi_close(&ofi_fabric->fid));

  fi_freeinfo(ofi_info);
}


////////////////////////////////////////
//
// Interface: Registered memory
//

static pthread_once_t fixedHeapOnce = PTHREAD_ONCE_INIT;
static size_t fixedHeapSize;
static void*  fixedHeapStart;

static pthread_once_t hugepageOnce = PTHREAD_ONCE_INIT;
static size_t hugepageSize;

static size_t nic_mem_map_limit;

static void init_fixedHeap(void);

static size_t get_hugepageSize(void);
static void init_hugepageSize(void);


void chpl_comm_impl_regMemHeapInfo(void** start_p, size_t* size_p) {
  DBG_PRINTF(DBG_IFACE_SETUP, "%s()", __func__);

  PTHREAD_CHK(pthread_once(&fixedHeapOnce, init_fixedHeap));
  *start_p = fixedHeapStart;
  *size_p  = fixedHeapSize;
}


static
void init_fixedHeap(void) {
  //
  // We only need a fixed heap if we're multinode, and either we're
  // on a Cray XC system or the user has explicitly specified a heap
  // size.
  //
  size_t size = chpl_comm_getenvMaxHeapSize();
  if ( ! (chpl_numNodes > 1
          && (strcmp(CHPL_TARGET_PLATFORM, "cray-xc") == 0 || size > 0))) {
    return;
  }

  //
  // On XC systems you really ought to use hugepages.  If called for,
  // a message will be emitted later.
  //
  size_t page_size;
  chpl_bool have_hugepages;
  void* start;

  if ((page_size = get_hugepageSize()) == 0) {
    page_size = chpl_getSysPageSize();
    have_hugepages = false;
  } else {
    have_hugepages = true;
  }

  if (size == 0) {
    size = (size_t) 16 << 30;
  }

  size = ALIGN_UP(size, page_size);

  //
  // The heap is supposed to be of fixed size and on hugepages.  Set
  // it up.
  //

  //
  // Considering the data size we'll register, compute the maximum
  // heap size that will allow all registrations to fit in the NIC
  // TLB.
  //
  const size_t nic_TLB_cache_pages = 512; // not publicly defined
  nic_mem_map_limit = nic_TLB_cache_pages * page_size;

  //
  // As a hedge against silliness, first reduce any request so that it's
  // no larger than the physical memory.  As a beneficial side effect
  // when the user request is ridiculously large, this also causes the
  // reduce-by-5% loop below to run faster and produce a final size
  // closer to the maximum available.
  //
  const size_t size_phys = ALIGN_DN(chpl_sys_physicalMemoryBytes(), page_size);
  if (size > size_phys)
    size = size_phys;

  //
  // Work our way down from the starting size in (roughly) 5% steps
  // until we can actually get that much from the system.
  //
  size_t decrement;

  if ((decrement = ALIGN_DN((size_t) (0.05 * size), page_size)) < page_size) {
    decrement = page_size;
  }

  size += decrement;
  do {
    size -= decrement;
    DBG_PRINTF(DBG_HUGEPAGES, "try allocating fixed heap, size %#zx", size);
    if (have_hugepages) {
      start = chpl_comm_ofi_hp_get_huge_pages(size);
    } else {
      CHK_SYS_MEMALIGN(start, page_size, size);
    }
  } while (start == NULL && size > decrement);

  if (start == NULL)
    chpl_error("cannot initialize heap: cannot get memory", 0, 0);

  chpl_comm_regMemHeapTouch(start, size);

  DBG_PRINTF(DBG_MR, "fixed heap on %spages, start=%p size=%#zx\n",
             have_hugepages ? "huge" : "regular ", start, size);

  fixedHeapSize  = size;
  fixedHeapStart = start;
}


static
void emit_delayedFixedHeapMsgs(void) {
  //
  // We only need a fixed heap if we're multinode on a Cray XC system
  // and using the gni provider.
  //
  if (chpl_numNodes <= 1 || !providerInUse(provType_gni)) {
    return;
  }

  //
  // On XC systems you really ought to use hugepages.
  //
  void* start;
  size_t size;
  chpl_comm_impl_regMemHeapInfo(&start, &size);
  if (hugepageSize == 0) {
    chpl_warning_explicit("not using hugepages may reduce performance",
                          __LINE__, __FILE__);
  }

  //
  // Warn if the size is larger than what will fit in the TLB cache.
  // While that may reduce performance it won't affect function, though,
  // so don't do anything dramatic like reducing the size to fit.
  //
  if (size > nic_mem_map_limit) {
    if (chpl_nodeID == 0) {
      size_t page_size = chpl_comm_impl_regMemHeapPageSize();
      char buf1[20], buf2[20], buf3[20], msg[200];
      chpl_snprintf_KMG_z(buf1, sizeof(buf1), nic_mem_map_limit);
      chpl_snprintf_KMG_z(buf2, sizeof(buf2), page_size);
      chpl_snprintf_KMG_f(buf3, sizeof(buf3), size);
      (void) snprintf(msg, sizeof(msg),
                      "Aries TLB cache can cover %s with %s pages; "
                      "with %s heap,\n"
                      "         cache refills may reduce performance",
                      buf1, buf2, buf3);
      chpl_warning(msg, 0, 0);
    }
  }
}


size_t chpl_comm_impl_regMemHeapPageSize(void) {
  DBG_PRINTF(DBG_IFACE_SETUP, "%s()", __func__);

  size_t sz;
  if ((sz = get_hugepageSize()) > 0)
    return sz;
  return chpl_getSysPageSize();
}


static
size_t get_hugepageSize(void) {
  PTHREAD_CHK(pthread_once(&hugepageOnce, init_hugepageSize));
  return hugepageSize;
}


static
void init_hugepageSize(void) {
  if (chpl_numNodes > 1
      && getenv("HUGETLB_DEFAULT_PAGE_SIZE") != NULL) {
    hugepageSize = chpl_comm_ofi_hp_gethugepagesize();
  }

  DBG_PRINTF(DBG_HUGEPAGES,
             "setting hugepage info: use hugepages %s, sz %#zx",
             (hugepageSize > 0) ? "YES" : "NO", hugepageSize);
}


static inline
struct memEntry* getMemEntry(memTab_t* tab, void* addr, size_t size) {
  char* myAddr = (char*) addr;

  for (int i = 0; i < numMemRegions; i++) {
    char* tabAddr = (char*) (*tab)[i].addr;
    char* tabAddrEnd = tabAddr + (*tab)[i].size;
    if (myAddr >= tabAddr && myAddr + size <= tabAddrEnd)
      return &(*tab)[i];
  }

  return NULL;
}


static inline
int mrGetDesc(void** pDesc, void* addr, size_t size) {
  void* desc;

  if (scalableMemReg) {
    desc = NULL;
  } else {
    struct memEntry* mr;
    if ((mr = getMemEntry(&memTab, addr, size)) == NULL) {
      DBG_PRINTF(DBG_MR_DESC, "mrGetDesc(%p, %zd): no entry", addr, size);
      return -1;
    }
    desc = mr->desc;
    DBG_PRINTF(DBG_MR_DESC, "mrGetDesc(%p, %zd): desc %p", addr, size, desc);
  }

  if (pDesc != NULL) {
    *pDesc = desc;
  }
  return 0;
}


static inline
int mrGetKey(uint64_t* pKey, uint64_t* pOff,
             int iNode, void* addr, size_t size) {
  uint64_t key;
  uint64_t off;

  if (scalableMemReg) {
    key = 0;
    off = (uint64_t) addr;
  } else {
    struct memEntry* mr;
    if ((mr = getMemEntry(&memTabMap[iNode], addr, size)) == NULL) {
      DBG_PRINTF(DBG_MR_KEY, "mrGetKey(%d:%p, %zd): no entry",
                 iNode, addr, size);
      return -1;
    }

    key = mr->key;
    off = (uint64_t) addr - mr->base;
    DBG_PRINTF(DBG_MR_KEY,
               "mrGetKey(%d:%p, %zd): key %" PRIx64 ", off %" PRIx64,
               iNode, addr, size, key, off);
  }

  if (pKey != NULL) {
    *pKey = key;
    *pOff = off;
  }
  return 0;
}


static inline
int mrGetLocalKey(void* addr, size_t size) {
  return mrGetKey(NULL, NULL, chpl_nodeID, addr, size);
}


////////////////////////////////////////
//
// Interface: memory consistency
//

static inline void amRequestNop(c_nodeid_t, chpl_bool);
static inline chpl_bool setUpDelayedAmDone(chpl_comm_taskPrvData_t**, void**);
static inline void retireDelayedAmDone(chpl_bool);

static inline
void mcmReleaseOneNode(c_nodeid_t node, struct perTxCtxInfo_t* tcip,
                       const char* dbgOrderStr) {
  DBG_PRINTF(DBG_ORDER,
             "dummy GET from %d for %s ordering",
             (int) node, dbgOrderStr);
  if (tcip->txCQ != NULL) {
    atomic_bool txnDone;
    atomic_init_bool(&txnDone, false);
    void* ctx = txnTrkEncodeDone(&txnDone);
    ofi_get_ll(orderDummy, node, orderDummyMap[node], 1, ctx, tcip);
    waitForTxnComplete(tcip, ctx);
    atomic_destroy_bool(&txnDone);
  } else {
    ofi_get_ll(orderDummy, node, orderDummyMap[node], 1, NULL, tcip);
    waitForTxnComplete(tcip, NULL);
  }
}


static
void mcmReleaseAllNodes(struct bitmap_t* b, struct perTxCtxInfo_t* tcip,
                        const char* dbgOrderStr) {
  //
  // Do a transaction (dummy GET or no-op AM) on every node in a bitmap.
  // Combined with our ordering assertions, this forces the results of
  // previous transactions to be visible in memory.  The effects of the
  // transactions we do here don't matter, only their completions.
  //
  // TODO: Allow multiple of these transactions outstanding at once,
  //       instead of waiting for each one before firing the next.
  //
  struct perTxCtxInfo_t* myTcip = tcip;
  if (myTcip == NULL) {
    CHK_TRUE((myTcip = tciAlloc()) != NULL);
  }

  BITMAP_FOREACH_SET(b, node) {
    bitmapClear(b, node);
    (*myTcip->checkTxCmplsFn)(myTcip);
    // If using CQ, need room for at least 1 txn.
    while (myTcip->txCQ != NULL && myTcip->numTxnsOut >= txCQLen) {
      sched_yield();
      (*myTcip->checkTxCmplsFn)(myTcip);
    }
    mcmReleaseOneNode(node, myTcip, dbgOrderStr);
  } BITMAP_FOREACH_SET_END

  if (tcip == NULL) {
    tciFree(myTcip);
  }
}


void chpl_comm_impl_unordered_task_fence(void) {
  DBG_PRINTF(DBG_IFACE_MCM, "%s()", __func__);

  task_local_buff_end(get_buff | put_buff | amo_nf_buff);
}


inline
void chpl_comm_impl_task_create(void) {
  DBG_PRINTF(DBG_IFACE_MCM, "%s()", __func__);

  retireDelayedAmDone(false /*taskIsEnding*/);
  waitForPutsVisAllNodes(NULL, NULL, false /*taskIsEnding*/);
}


void chpl_comm_impl_task_end(void) {
  DBG_PRINTF(DBG_IFACE_MCM, "%s()", __func__);

  task_local_buff_end(get_buff | put_buff | amo_nf_buff);
  retireDelayedAmDone(true /*taskIsEnding*/);
  waitForPutsVisAllNodes(NULL, NULL, true /*taskIsEnding*/);
}


////////////////////////////////////////
//
// Interface: Active Messages
//

typedef enum {
  am_opExecOn = CHPL_ARG_BUNDLE_KIND_COMM, // impl-nonspecific on-stmt
  am_opExecOnLrg,                          // on-stmt, large arg
  am_opGet,                                // do an RMA GET
  am_opPut,                                // do an RMA PUT
  am_opAMO,                                // do an AMO
  am_opFree,                               // free some memory
  am_opNop,                                // do nothing; for MCM & liveness
  am_opShutdown,                           // signal main process for shutdown
} amOp_t;

#ifdef CHPL_COMM_DEBUG
static inline
chpl_bool op_uses_on_bundle(amOp_t op) {
  return op == am_opExecOn || op == am_opExecOnLrg;
}
#endif

//
// Members are packed, potentially differently, in each AM request type
// to reduce space requirements.  The 'op' member must come first in all
// cases, so the AM handler can tell what kind of request it's looking
// at.
//

typedef uint8_t amDone_t;

struct amRequest_base_t {
  chpl_arg_bundle_kind_t op;  // operation
  c_nodeid_t node;            // initiator's node
  amDone_t* pAmDone;          // initiator's 'done' flag; may be NULL
#ifdef CHPL_COMM_DEBUG
  uint32_t crc;
  uint64_t seq;
#endif
};

struct amRequest_RMA_t {
  struct amRequest_base_t b;
  void* addr;                   // address on AM target node
  void* raddr;                  // address on AM initiator's node
  size_t size;                  // number of bytes
};

typedef union {
  int32_t i32;
  uint32_t u32;
  chpl_bool32 b32;
  int64_t i64;
  uint64_t u64;
  _real32 r32;
  _real64 r64;
} chpl_amo_datum_t;

struct amRequest_AMO_t {
  struct amRequest_base_t b;
  enum fi_op ofiOp;             // ofi AMO op
  enum fi_datatype ofiType;     // ofi object type
  int8_t size;                  // object size (bytes)
  void* obj;                    // object address on target node
  chpl_amo_datum_t operand1;    // first operand, if needed
  chpl_amo_datum_t operand2;    // second operand, if needed
  void* result;                 // result address on initiator's node
};

struct amRequest_free_t {
  struct amRequest_base_t b;
  void* p;                      // address to free, on AM target node
};

typedef union {
  struct amRequest_base_t b;
  struct amRequest_execOn_t xo;      // present only to set the max req size
  struct amRequest_execOnLrg_t xol;
  struct amRequest_RMA_t rma;
  struct amRequest_AMO_t amo;
  struct amRequest_free_t free;
} amRequest_t;

struct taskArg_RMA_t {
  chpl_task_bundle_t hdr;
  struct amRequest_RMA_t rma;
};


#ifdef CHPL_COMM_DEBUG
static const char* am_opName(amOp_t);
static const char* amo_opName(enum fi_op);
static const char* amo_typeName(enum fi_datatype);
static const char* am_seqIdStr(amRequest_t*);
static const char* am_reqStr(c_nodeid_t, amRequest_t*, size_t);
static const char* am_reqDoneStr(amRequest_t*);
#endif

static void amRequestExecOn(c_nodeid_t, c_sublocid_t, chpl_fn_int_t,
                            chpl_comm_on_bundle_t*, size_t,
                            chpl_bool, chpl_bool);
static void amRequestRMA(c_nodeid_t, amOp_t, void*, void*, size_t);
static void amRequestAMO(c_nodeid_t, void*, const void*, const void*, void*,
                         int, enum fi_datatype, size_t);
static void amRequestFree(c_nodeid_t, void*);
static void amRequestNop(c_nodeid_t, chpl_bool);
static void amRequestCommon(c_nodeid_t, amRequest_t*, size_t,
                            amDone_t**, chpl_bool, struct perTxCtxInfo_t*);
static inline void amWaitForDone(amDone_t*);


void chpl_comm_execute_on(c_nodeid_t node, c_sublocid_t subloc,
                          chpl_fn_int_t fid,
                          chpl_comm_on_bundle_t *arg, size_t argSize,
                          int ln, int32_t fn) {
  DBG_PRINTF(DBG_IFACE,
             "%s(%d, %d, %d, %p, %zd)", __func__,
             (int) node, (int) subloc, (int) fid, arg, argSize);

  CHK_TRUE(node != chpl_nodeID); // handled by the locale model

  if (chpl_comm_have_callbacks(chpl_comm_cb_event_kind_executeOn)) {
    chpl_comm_cb_info_t cb_data =
      {chpl_comm_cb_event_kind_executeOn, chpl_nodeID, node,
       .iu.executeOn={subloc, fid, arg, argSize, ln, fn}};
    chpl_comm_do_callbacks (&cb_data);
  }

  chpl_comm_diags_verbose_executeOn("", node, ln, fn);
  chpl_comm_diags_incr(execute_on);

  amRequestExecOn(node, subloc, fid, arg, argSize, false, true);
}


void chpl_comm_execute_on_nb(c_nodeid_t node, c_sublocid_t subloc,
                             chpl_fn_int_t fid,
                             chpl_comm_on_bundle_t *arg, size_t argSize,
                             int ln, int32_t fn) {
  DBG_PRINTF(DBG_IFACE,
             "%s(%d, %d, %d, %p, %zd)", __func__,
             (int) node, (int) subloc, (int) fid, arg, argSize);

  CHK_TRUE(node != chpl_nodeID); // handled by the locale model

  if (chpl_comm_have_callbacks(chpl_comm_cb_event_kind_executeOn_nb)) {
    chpl_comm_cb_info_t cb_data =
      {chpl_comm_cb_event_kind_executeOn_nb, chpl_nodeID, node,
       .iu.executeOn={subloc, fid, arg, argSize, ln, fn}};
    chpl_comm_do_callbacks (&cb_data);
  }

  chpl_comm_diags_verbose_executeOn("non-blocking", node, ln, fn);
  chpl_comm_diags_incr(execute_on_nb);

  amRequestExecOn(node, subloc, fid, arg, argSize, false, false);
}


void chpl_comm_execute_on_fast(c_nodeid_t node, c_sublocid_t subloc,
                               chpl_fn_int_t fid,
                               chpl_comm_on_bundle_t *arg, size_t argSize,
                               int ln, int32_t fn) {
  DBG_PRINTF(DBG_IFACE,
             "%s(%d, %d, %d, %p, %zd)", __func__,
             (int) node, (int) subloc, (int) fid, arg, argSize);

  CHK_TRUE(node != chpl_nodeID); // handled by the locale model

  if (chpl_comm_have_callbacks(chpl_comm_cb_event_kind_executeOn_fast)) {
    chpl_comm_cb_info_t cb_data =
      {chpl_comm_cb_event_kind_executeOn_fast, chpl_nodeID, node,
       .iu.executeOn={subloc, fid, arg, argSize, ln, fn}};
    chpl_comm_do_callbacks (&cb_data);
  }

  chpl_comm_diags_verbose_executeOn("fast", node, ln, fn);
  chpl_comm_diags_incr(execute_on_fast);

  amRequestExecOn(node, subloc, fid, arg, argSize, true, true);
}


static inline
void amRequestExecOn(c_nodeid_t node, c_sublocid_t subloc,
                     chpl_fn_int_t fid,
                     chpl_comm_on_bundle_t* arg, size_t argSize,
                     chpl_bool fast, chpl_bool blocking) {
  assert(!isAmHandler);
  CHK_TRUE(!(fast && !blocking)); // handler doesn't expect fast nonblocking

  retireDelayedAmDone(false /*taskIsEnding*/);

  arg->comm = (chpl_comm_bundleData_t) { .fast = fast,
                                         .fid = fid,
                                         .node = chpl_nodeID,
                                         .subloc = subloc,
                                         .argSize = argSize, };

  if (argSize <= sizeof(amRequest_t)) {
    //
    // The arg bundle will fit in max-sized AM request; just send it.
    //
    arg->kind = am_opExecOn;
    amRequestCommon(node, (amRequest_t*) arg, argSize,
                    blocking ? (amDone_t**) &arg->comm.pAmDone : NULL,
                    blocking /*yieldDuringTxnWait*/, NULL);
  } else {
    //
    // The arg bundle is too large for an AM request.  Send a copy of
    // the header to the target and have it retrieve the payload part
    // itself.
    //
    // For the nonblocking case we have to make a copy of the caller's
    // payload because as soon as we return, the caller may destroy
    // the original.  We also make a copy if the original is not in
    // registered memory and needs to be, in order to save the target
    // the overhead of doing an AM back to us to PUT the bundle to
    // itself.
    //
    arg->kind = am_opExecOnLrg;
    amRequest_t req = { .xol = { .hdr = *arg,
                                 .pPayload = &arg->payload, }, };

    chpl_bool heapCopyArg = !blocking || mrGetLocalKey(arg, argSize) != 0;
    if (heapCopyArg) {
      size_t payloadSize = argSize - offsetof(chpl_comm_on_bundle_t, payload);
      CHPL_CALLOC_SZ(req.xol.pPayload, 1, payloadSize);
      memcpy(req.xol.pPayload, &arg->payload, payloadSize);
    }

    amRequestCommon(node, &req, sizeof(req.xol),
                    blocking ? (amDone_t**) &req.xol.hdr.comm.pAmDone : NULL,
                    blocking /*yieldDuringTxnWait*/, NULL);

    //
    // If blocking and we heap-copied the arg, free that now.  The
    // nonblocking case has to be handled from the target side, since
    // only there do we know when we don't need the copy any more.
    //
    if (heapCopyArg && blocking) {
      CHPL_FREE(req.xol.pPayload);
    }
  }
}


static inline
void amRequestRMA(c_nodeid_t node, amOp_t op,
                  void* addr, void* raddr, size_t size) {
  assert(!isAmHandler);
  amRequest_t req = { .rma = { .b = { .op = op,
                                      .node = chpl_nodeID, },
                               .addr = raddr,
                               .raddr = addr,
                               .size = size, }, };
  retireDelayedAmDone(false /*taskIsEnding*/);
  amRequestCommon(node, &req, sizeof(req.rma),
                  &req.b.pAmDone, true /*yieldDuringTxnWait*/, NULL);
}


static inline
void amRequestAMO(c_nodeid_t node, void* object,
                  const void* operand1, const void* operand2, void* result,
                  int ofiOp, enum fi_datatype ofiType, size_t size) {
  assert(!isAmHandler);
  DBG_PRINTF((ofiOp == FI_ATOMIC_READ) ? DBG_AMO_READ : DBG_AMO,
             "AMO via AM: obj %d:%p, opnd1 <%s>, opnd2 <%s>, res %p, "
             "op %s, typ %s, sz %zd",
             (int) node, object,
             DBG_VAL(operand1, ofiType), DBG_VAL(operand2, ofiType), result,
             amo_opName(ofiOp), amo_typeName(ofiType), size);

  struct perTxCtxInfo_t* tcip;
  CHK_TRUE((tcip = tciAlloc()) != NULL);

  void* myResult = result;
  size_t resSize = size;

  //
  // If this is a non-fetching atomic and the task is ending (therefore
  // this is the _downEndCount()) we do it as a regular nonblocking AM.
  // If it's non-fetching and the task is not ending we may be able to
  // do it as a blocking AM but delay waiting for the 'done' indicator
  // until sometime later, when the next thing with MCM implications
  // comes along.  Otherwise, we have to do it as a normal blocking AM.
  //
  chpl_bool delayBlocking = false;
  chpl_comm_taskPrvData_t* prvData = NULL;
  amDone_t* pAmDone = NULL;
  if (myResult == NULL) {
    delayBlocking = setUpDelayedAmDone(&prvData, (void**) &pAmDone);
  } else {
    if (mrGetLocalKey(myResult, resSize) != 0) {
      myResult = allocBounceBuf(resSize);
      DBG_PRINTF((ofiOp == FI_ATOMIC_READ) ? DBG_AMO_READ : DBG_AMO,
                 "AMO result BB: %p", myResult);
      CHK_TRUE(mrGetLocalKey(myResult, resSize) == 0);
    }
  }

  amRequest_t req = { .amo = { .b = { .op = am_opAMO,
                                      .node = chpl_nodeID,
                                      .pAmDone = delayBlocking
                                                 ? pAmDone
                                                 : NULL, },
                               .ofiOp = ofiOp,
                               .ofiType = ofiType,
                               .size = size,
                               .obj = object,
                               .result = myResult, }, };

  if (operand1 != NULL) {
    memcpy(&req.amo.operand1, operand1, size);
  }
  if (operand2 != NULL) {
    memcpy(&req.amo.operand2, operand2, size);
  }
  amRequestCommon(node, &req, sizeof(req.amo),
                  delayBlocking ? NULL : &req.b.pAmDone,
                  true /*yieldDuringTxnWait*/, tcip);
  if (myResult != result) {
    memcpy(result, myResult, resSize);
    freeBounceBuf(myResult);
  }

  tciFree(tcip);
}


static inline
void amRequestFree(c_nodeid_t node, void* p) {
  amRequest_t req = { .free = { .b = { .op = am_opFree,
                                       .node = chpl_nodeID, },
                                .p = p, }, };
  amRequestCommon(node, &req, sizeof(req.free),
                  NULL, false /*yieldDuringTxnWait*/, NULL);
}


static inline
void amRequestNop(c_nodeid_t node, chpl_bool blocking) {
  amRequest_t req = { .b = { .op = am_opNop,
                             .node = chpl_nodeID, }, };
  amRequestCommon(node, &req, sizeof(req.b),
                  blocking ? &req.b.pAmDone : NULL,
                  false /*yieldDuringTxnWait*/, NULL);
}


static inline
void amRequestShutdown(c_nodeid_t node) {
  assert(!isAmHandler);
  amRequest_t req = { .b = { .op = am_opShutdown,
                             .node = chpl_nodeID, }, };
  amRequestCommon(node, &req, sizeof(req.b),
                  NULL, true /*yieldDuringTxnWait*/, NULL);
}


static inline
void amRequestCommon(c_nodeid_t node,
                     amRequest_t* req, size_t reqSize,
                     amDone_t** ppAmDone,
                     chpl_bool yieldDuringTxnWait,
                     struct perTxCtxInfo_t* tcip) {
  //
  // If blocking, make sure target can RMA PUT the indicator to us.
  //
  amDone_t amDone;
  amDone_t* pAmDone = NULL;
  if (ppAmDone != NULL) {
    pAmDone = &amDone;
    if (mrGetLocalKey(pAmDone, sizeof(*pAmDone)) != 0) {
      pAmDone = allocBounceBuf(sizeof(*pAmDone));
      CHK_TRUE(mrGetLocalKey(pAmDone, sizeof(*pAmDone)) == 0);
    }
    *pAmDone = 0;
    chpl_atomic_thread_fence(memory_order_release);

    *ppAmDone = pAmDone;
  }

#ifdef CHPL_COMM_DEBUG
  if (DBG_TEST_MASK(DBG_AM | DBG_AM_SEND | DBG_AM_RECV)
      || (req->b.op == am_opAMO && DBG_TEST_MASK(DBG_AMO))) {
    static atomic_uint_least64_t seq;

    static chpl_bool seqInited = false;
    if (!seqInited) {
      static pthread_mutex_t seqLock = PTHREAD_MUTEX_INITIALIZER;
      PTHREAD_CHK(pthread_mutex_lock(&seqLock));
      atomic_init_uint_least64_t(&seq, 1);
      seqInited = true;
      PTHREAD_CHK(pthread_mutex_unlock(&seqLock));
    }

    if (op_uses_on_bundle(req->b.op)) {
      req->xo.hdr.comm.seq = atomic_fetch_add_uint_least64_t(&seq, 1);
#ifdef DEBUG_CRC_MSGS
      req->xo.hdr.comm.crc = 0;
      req->xo.hdr.comm.crc = xcrc32((void*) req, reqSize, ~(uint32_t) 0);
#endif
    } else {
      req->b.seq = atomic_fetch_add_uint_least64_t(&seq, 1);
#ifdef DEBUG_CRC_MSGS
      req->b.crc = 0;
      req->b.crc = xcrc32((void*) req, reqSize, ~(uint32_t) 0);
#endif
    }
  }
#endif

  struct perTxCtxInfo_t* myTcip = tcip;
  if (myTcip == NULL) {
    CHK_TRUE((myTcip = tciAlloc()) != NULL);
  }

  amRequest_t* myReq = req;
  void* mrDesc = NULL;
  if (mrGetDesc(&mrDesc, myReq, reqSize) != 0) {
    myReq = allocBounceBuf(reqSize);
    DBG_PRINTF(DBG_AM | DBG_AM_SEND, "AM req BB: %p", myReq);
    CHK_TRUE(mrGetDesc(NULL, myReq, reqSize) == 0);
    memcpy(myReq, req, reqSize);
  }

  //
  // We're ready to send the request.  But for on-stmts and AMOs that
  // might modify their target variable, MCM conformance requires us
  // first to ensure that all previous PUTs are visible.  Similarly, for
  // GET and PUT ops, we have to ensure that PUTs to the same node are
  // visible.  No other ops depend on PUT visibility.
  //
  // A note about including RMA PUT ops here -- at first glance it would
  // seem that all PUTs targeting a given address would either be done
  // using the RMA interface or the message interface, rather than that
  // some PUTs would use one interface and others using the other.  But
  // whether we use RMA or messaging depends on whether we have an MR
  // key for the entire [address, address+size-1] memory range, so to
  // be strictly correct we need to allow for overlapping transfers to
  // go via different methods.
  //
  if (myReq->b.op == am_opExecOn
      || myReq->b.op == am_opExecOnLrg
      || (myReq->b.op == am_opAMO && myReq->amo.ofiOp != FI_ATOMIC_READ)) {
    waitForPutsVisAllNodes(myTcip, NULL, false /*taskIsEnding*/);
  } else if (myReq->b.op == am_opGet
             || myReq->b.op == am_opPut) {
    waitForPutsVisOneNode(node, myTcip, NULL);
  }

  //
  // Inject the message if it's small enough and we're not going to wait
  // for it anyway.  Otherwise, do a regular send.  Don't count injected
  // messages as "outstanding", because they won't generate CQ events.
  //
  if (pAmDone == NULL && reqSize <= ofi_info->tx_attr->inject_size) {
    if (DBG_TEST_MASK(DBG_AM | DBG_AM_SEND)
        || (req->b.op == am_opAMO && DBG_TEST_MASK(DBG_AMO))) {
      DBG_DO_PRINTF("tx AM req inject to %d: %s",
                    (int) node, am_reqStr(node, myReq, reqSize));
    }
    OFI_RIDE_OUT_EAGAIN(myTcip,
                        fi_inject(myTcip->txCtx, myReq, reqSize,
                                  rxMsgAddr(myTcip, node)));
    myTcip->numTxnsSent++;
  } else {
    atomic_bool txnDone;
    atomic_init_bool(&txnDone, false);
    void* ctx = txnTrkEncodeDone(&txnDone);

    if (DBG_TEST_MASK(DBG_AM | DBG_AM_SEND)
        || (req->b.op == am_opAMO && DBG_TEST_MASK(DBG_AMO))) {
      DBG_DO_PRINTF("tx AM req to %d: %s, ctx %p",
                    (int) node, am_reqStr(node, myReq, reqSize), ctx);
    }
    OFI_RIDE_OUT_EAGAIN(myTcip,
                        fi_send(myTcip->txCtx, myReq, reqSize,
                                mrDesc, rxMsgAddr(myTcip, node), ctx));
    myTcip->numTxnsOut++;
    myTcip->numTxnsSent++;
    waitForTxnComplete(myTcip, ctx);
    atomic_destroy_bool(&txnDone);
  }

  if (tcip == NULL) {
    tciFree(myTcip);
  }

  if (myReq != req) {
    freeBounceBuf(myReq);
  }

  if (pAmDone != NULL) {
    amWaitForDone(pAmDone);
    if (pAmDone != &amDone) {
      freeBounceBuf(pAmDone);
    }
  }
}


static inline
void amWaitForDone(amDone_t* pAmDone) {
  //
  // Wait for completion indicator.
  //
  DBG_PRINTF(DBG_AM | DBG_AM_SEND,
             "waiting for amDone indication in %p", pAmDone);
  while (!*(volatile amDone_t*) pAmDone) {
    local_yield();
  }
  DBG_PRINTF(DBG_AM | DBG_AM_SEND, "saw amDone indication in %p", pAmDone);
}


static inline
chpl_bool setUpDelayedAmDone(chpl_comm_taskPrvData_t** pPrvData,
                             void** ppAmDone) {
  //
  // Set up to record the completion of a delayed-blocking AM.
  //
  chpl_comm_taskPrvData_t* prvData;
  if ((*pPrvData = prvData = get_comm_taskPrvdata()) == NULL) {
    return false;
  }

  if (prvData->taskIsEnding) {
    //
    // This AMO is for our _downEndCount().  We don't care when that is
    // done because we won't do anything after it, and our parent only
    // cares about the effect on the endCount.  Therefore, send back
    // *ppAmDone==NULL to make our caller do a regular non-blocking AM.
    //
    *ppAmDone = NULL;
    return true;
  }

  //
  // Otherwise, this will be an actual delayed-blocking AM, and we'll
  // use the task-private 'done' indicator for it.
  //
  *ppAmDone = &prvData->amDone;
  prvData->amDone = 0;
  chpl_atomic_thread_fence(memory_order_release);
  prvData->amDonePending = true;
  return true;
}


static inline
void retireDelayedAmDone(chpl_bool taskIsEnding) {
  //
  // Wait for the completion of any delayed-blocking AM.
  //
  chpl_comm_taskPrvData_t* prvData = get_comm_taskPrvdata();
  if (prvData != NULL) {
    if (prvData->amDonePending) {
      amWaitForDone((amDone_t*) &prvData->amDone);
      prvData->amDonePending = false;
    }
    if (taskIsEnding) {
      prvData->taskIsEnding = true;
    }
  }
}


////////////////////////////////////////
//
// Handler-side active message support
//

static int numAmHandlersActive;
static pthread_cond_t amStartStopCond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t amStartStopMutex = PTHREAD_MUTEX_INITIALIZER;

static void amHandler(void*);
static void processRxAmReq(struct perTxCtxInfo_t*);
static void amHandleExecOn(chpl_comm_on_bundle_t*);
static inline void amWrapExecOnBody(void*);
static void amHandleExecOnLrg(chpl_comm_on_bundle_t*);
static void amWrapExecOnLrgBody(struct amRequest_execOnLrg_t*);
static void amWrapGet(struct taskArg_RMA_t*);
static void amWrapPut(struct taskArg_RMA_t*);
static void amHandleAMO(struct amRequest_AMO_t*);
static inline void amSendDone(c_nodeid_t, amDone_t*);
static inline void amCheckLiveness(void);

static inline void doCpuAMO(void*, const void*, const void*, void*,
                            enum fi_op, enum fi_datatype, size_t size);


static
void init_amHandling(void) {
  //
  // Sanity checks.
  //
  {
    chpl_comm_taskPrvData_t pd;
    CHK_TRUE(sizeof(pd.amDone) >= sizeof(amDone_t));
  }

  //
  // Start AM handler thread(s).  Don't proceed from here until at
  // least one is running.
  //
  atomic_init_bool(&amHandlersExit, false);

  PTHREAD_CHK(pthread_mutex_lock(&amStartStopMutex));
  for (int i = 0; i < numAmHandlers; i++) {
    CHK_TRUE(chpl_task_createCommTask(amHandler, NULL) == 0);
  }
  PTHREAD_CHK(pthread_cond_wait(&amStartStopCond, &amStartStopMutex));
  PTHREAD_CHK(pthread_mutex_unlock(&amStartStopMutex));
}


static
void fini_amHandling(void) {
  if (chpl_numNodes <= 1)
    return;

  //
  // Tear down the AM handler thread(s).  On node 0, don't proceed from
  // here until the last one has finished.
  //
  PTHREAD_CHK(pthread_mutex_lock(&amStartStopMutex));
  atomic_store_bool(&amHandlersExit, true);
  PTHREAD_CHK(pthread_cond_wait(&amStartStopCond, &amStartStopMutex));
  PTHREAD_CHK(pthread_mutex_unlock(&amStartStopMutex));

  atomic_destroy_bool(&amHandlersExit);
}


//
// The AM handler runs this.
//
static __thread struct perTxCtxInfo_t* amTcip;

static
void amHandler(void* argNil) {
  struct perTxCtxInfo_t* tcip;
  CHK_TRUE((tcip = tciAllocForAmHandler()) != NULL);
  amTcip = tcip;

  isAmHandler = true;

  DBG_PRINTF(DBG_AM, "AM handler running");

  //
  // Count this AM handler thread as running.  The creator thread
  // wants to be released as soon as at least one AM handler thread
  // is running, so if we're the first, do that.
  //
  PTHREAD_CHK(pthread_mutex_lock(&amStartStopMutex));
  if (++numAmHandlersActive == 1)
    PTHREAD_CHK(pthread_cond_signal(&amStartStopCond));
  PTHREAD_CHK(pthread_mutex_unlock(&amStartStopMutex));

  //
  // Process AM requests and watch transmit responses arrive.
  //
  while (!atomic_load_bool(&amHandlersExit)) {
    if (ofi_amhPollSet != NULL) {
      void* contexts[pollSetSize];
      int ret;
      OFI_CHK_COUNT(fi_poll(ofi_amhPollSet, contexts, pollSetSize), ret);

      if (ret == 0) {
        ret = fi_wait(ofi_amhWaitSet, 100 /*ms*/);
        if (ret != FI_SUCCESS
            && ret != -FI_EINTR
            && ret != -FI_ETIMEDOUT) {
          OFI_ERR("fi_wait(ofi_amhWaitSet)", ret, fi_strerror(ret));
        }
        OFI_CHK_COUNT(fi_poll(ofi_amhPollSet, contexts, pollSetSize), ret);
      }

      //
      // Process the CQs/counters that had events.  We really only have
      // to take any explicit actions for inbound AM messages and our
      // transmit endpoint.  For the RMA endpoint we just need to ensure
      // progress, and the poll call itself did that.
      //
      for (int i = 0; i < ret; i++) {
        if (contexts[i] == &ofi_rxCQ) {
          processRxAmReq(tcip);
        } else if (contexts[i] == &tcip->checkTxCmplsFn) {
          (*tcip->checkTxCmplsFn)(tcip);
        } else if (contexts[i] == &checkRxRmaCmplsFn) {
          // no action
        } else {
          INTERNAL_ERROR_V("unexpected context %p from fi_poll()",
                           contexts[i]);
        }
      }
    } else {
      //
      // The provider can't do poll sets.
      //
      processRxAmReq(tcip);
      (*tcip->checkTxCmplsFn)(tcip);
      (*checkRxRmaCmplsFn)();

      sched_yield();
    }

    if (amDoLivenessChecks) {
      amCheckLiveness();
    }
  }

  //
  // Un-count this AM handler thread.  Whoever told us to exit wants to
  // be released once all the AM handler threads are done, so if we're
  // the last, do that.
  //
  PTHREAD_CHK(pthread_mutex_lock(&amStartStopMutex));
  if (--numAmHandlersActive == 0)
    PTHREAD_CHK(pthread_cond_signal(&amStartStopCond));
  PTHREAD_CHK(pthread_mutex_unlock(&amStartStopMutex));

  DBG_PRINTF(DBG_AM, "AM handler done");
}


static
void processRxAmReq(struct perTxCtxInfo_t* tcip) {
  //
  // Process requests received on the AM request endpoint.
  //
  struct fi_cq_data_entry cqes[5];
  const size_t maxEvents = sizeof(cqes) / sizeof(cqes[0]);
  ssize_t ret;
  CHK_TRUE((ret = fi_cq_read(ofi_rxCQ, cqes, maxEvents)) > 0
           || ret == -FI_EAGAIN
           || ret == -FI_EAVAIL);
  if (ret == -FI_EAVAIL) {
    reportCQError(ofi_rxCQ);
  }

  const size_t numEvents = (ret == -FI_EAGAIN) ? 0 : ret;

  for (int i = 0; i < numEvents; i++) {
    if ((cqes[i].flags & FI_RECV) != 0) {
      //
      // This event is for an inbound AM request.  Handle it.
      //
      amRequest_t* req = (amRequest_t*) cqes[i].buf;
      DBG_PRINTF(DBG_AM_BUF,
                 "CQ rx AM req @ buffer offset %zd, sz %zd, seqId %s",
                 (char*) req - (char*) ofi_iov_reqs[ofi_msg_i].iov_base,
                 cqes[i].len, am_seqIdStr(req));

#if defined(CHPL_COMM_DEBUG) && defined(DEBUG_CRC_MSGS)
      if (DBG_TEST_MASK(DBG_AM)) {
        uint32_t sent_crc, rcvd_crc;
        size_t reqSize;
        if (op_uses_on_bundle(req->b.op)) {
          sent_crc = req->xo.hdr.comm.crc;
          req->xo.hdr.comm.crc = 0;
          reqSize = req->xo.hdr.comm.argSize;
        } else {
          sent_crc = req->b.crc;
          req->b.crc = 0;
          reqSize = (req->b.op == am_opGet || req->b.op == am_opPut)
                    ? sizeof(struct amRequest_RMA_t)
                    : (req->b.op == am_opAMO)
                    ? sizeof(struct amRequest_AMO_t)
                    : (req->b.op == am_opFree)
                    ? sizeof(struct amRequest_free_t)
                    : (req->b.op == am_opFree)
                    ? sizeof(struct amRequest_free_t)
                    : sizeof(struct amRequest_base_t);
        }
        uint32_t rcvd_crc = xcrc32((void*) req, reqSize, ~(uint32_t) 0);
        CHK_TRUE(rcvd_crc == sent_crc);
      }
#endif

      DBG_PRINTF(DBG_AM | DBG_AM_RECV,
                 "rx AM req: %s",
                 am_reqStr(chpl_nodeID, req, cqes[i].len));
      switch (req->b.op) {
      case am_opExecOn:
        if (req->xo.hdr.comm.fast) {
          amWrapExecOnBody(&req->xo.hdr);
        } else {
          amHandleExecOn(&req->xo.hdr);
        }
        break;

      case am_opExecOnLrg:
        amHandleExecOnLrg(&req->xol.hdr);
        break;

      case am_opGet:
        {
          struct taskArg_RMA_t arg = { .hdr.kind = CHPL_ARG_BUNDLE_KIND_TASK,
                                       .rma = req->rma, };
          chpl_task_startMovedTask(FID_NONE, (chpl_fn_p) amWrapGet,
                                   &arg, sizeof(arg), c_sublocid_any,
                                   chpl_nullTaskID);
        }
        break;

      case am_opPut:
        {
          struct taskArg_RMA_t arg = { .hdr.kind = CHPL_ARG_BUNDLE_KIND_TASK,
                                       .rma = req->rma, };
          chpl_task_startMovedTask(FID_NONE, (chpl_fn_p) amWrapPut,
                                   &arg, sizeof(arg), c_sublocid_any,
                                   chpl_nullTaskID);
        }
        break;

      case am_opAMO:
        amHandleAMO(&req->amo);
        break;

      case am_opFree:
        CHPL_FREE(req->free.p);
        break;

      case am_opNop:
        DBG_PRINTF(DBG_AM | DBG_AM_RECV, "%s", am_reqDoneStr(req));
        if (req->b.pAmDone != NULL) {
          amSendDone(req->b.node, req->b.pAmDone);
        }
        break;

      case am_opShutdown:
        chpl_signal_shutdown();
        break;

      default:
        INTERNAL_ERROR_V("unexpected AM op %d", (int) req->b.op);
        break;
      }
    }

    if ((cqes[i].flags & FI_MULTI_RECV) != 0) {
      //
      // Multi-receive buffer filled; post the other one.
      //
      ofi_msg_i = 1 - ofi_msg_i;
      OFI_CHK(fi_recvmsg(ofi_rxEp, &ofi_msg_reqs[ofi_msg_i], FI_MULTI_RECV));
      DBG_PRINTF(DBG_AM_BUF,
                 "re-post fi_recvmsg(AMLZs %p, len %#zx)",
                 ofi_msg_reqs[ofi_msg_i].msg_iov->iov_base,
                 ofi_msg_reqs[ofi_msg_i].msg_iov->iov_len);
    }

    CHK_TRUE((cqes[i].flags & ~(FI_MSG | FI_RECV | FI_MULTI_RECV)) == 0);
  }
}


static
void amHandleExecOn(chpl_comm_on_bundle_t* req) {
  chpl_comm_bundleData_t* comm = &req->comm;

  //
  // We only need a wrapper if we have to send a 'done' indicator back
  // or we need to produce the AM debug output.
  //
  chpl_fn_p fn = (comm->pAmDone == NULL && !DBG_TEST_MASK(DBG_AM | DBG_AM_RECV))
                 ? chpl_ftable[comm->fid]
                 : (chpl_fn_p) amWrapExecOnBody;
  chpl_task_startMovedTask(comm->fid, fn, req,
                           comm->argSize, comm->subloc, chpl_nullTaskID);
}


static inline
void amWrapExecOnBody(void* p) {
  chpl_comm_bundleData_t* comm = &((chpl_comm_on_bundle_t*) p)->comm;

  chpl_ftable_call(comm->fid, p);
  DBG_PRINTF(DBG_AM | DBG_AM_RECV, "%s", am_reqDoneStr(p));
  if (comm->pAmDone != NULL) {
    amSendDone(comm->node, comm->pAmDone);
  }
}


static inline
void amHandleExecOnLrg(chpl_comm_on_bundle_t* req) {
  struct amRequest_execOnLrg_t* xol = (struct amRequest_execOnLrg_t*) req;
  xol->hdr.kind = am_opExecOn;  // was am_opExecOnLrg, to direct us here
  chpl_task_startMovedTask(FID_NONE, (chpl_fn_p) amWrapExecOnLrgBody,
                           xol, sizeof(*xol),
                           xol->hdr.comm.subloc, chpl_nullTaskID);
}


static
void amWrapExecOnLrgBody(struct amRequest_execOnLrg_t* xol) {
  //
  // TODO: We could stack-allocate "bundle" here, if it was small enough
  //       (TBD) not to create the potential for stack overflow.  Some
  //       systems have fast enough networks that saving the dynamic
  //       alloc should be performance-visible.
  //

  //
  // The bundle header is in our argument, but we have to retrieve the
  // payload from the initiating side.
  //
  chpl_comm_bundleData_t* comm = &xol->hdr.comm;
  c_nodeid_t node = comm->node;

  chpl_comm_on_bundle_t* bundle;
  CHPL_CALLOC_SZ(bundle, 1, comm->argSize);
  *bundle = xol->hdr;

  size_t payloadSize = comm->argSize
                       - offsetof(chpl_comm_on_bundle_t, payload);
  CHK_TRUE(mrGetKey(NULL, NULL, node, xol->pPayload, payloadSize) == 0);
  (void) ofi_get(&bundle->payload, node, xol->pPayload, payloadSize);

  //
  // Iff this is a nonblocking executeOn, now that we have the payload
  // we can free the copy of it on the initiating side.  In the blocking
  // case the initiator will free it if that is necessary, since they
  // have to wait for the whole executeOn to complete anyway.  We save
  // some time here by not waiting for a network response.  Either we or
  // someone else will consume that completion later.  In the meantime
  // we can go ahead with the executeOn body.
  //
  if (comm->pAmDone == NULL) {
    amRequestFree(node, xol->pPayload);
  }

  //
  // Now we can finally call the body function.
  //
  chpl_ftable_call(bundle->comm.fid, bundle);
  DBG_PRINTF(DBG_AM | DBG_AM_RECV, "%s", am_reqDoneStr((amRequest_t*) xol));
  if (comm->pAmDone != NULL) {
    amSendDone(node, comm->pAmDone);
  }

  CHPL_FREE(bundle);
}


static
void amWrapGet(struct taskArg_RMA_t* tsk_rma) {
  struct amRequest_RMA_t* rma = &tsk_rma->rma;

  CHK_TRUE(mrGetKey(NULL, NULL, rma->b.node, rma->raddr, rma->size) == 0);
  (void) ofi_get(rma->addr, rma->b.node, rma->raddr, rma->size);

  DBG_PRINTF(DBG_AM | DBG_AM_RECV, "%s", am_reqDoneStr((amRequest_t*) rma));
  amSendDone(rma->b.node, rma->b.pAmDone);
}


static
void amWrapPut(struct taskArg_RMA_t* tsk_rma) {
  struct amRequest_RMA_t* rma = &tsk_rma->rma;

  CHK_TRUE(mrGetKey(NULL, NULL, rma->b.node, rma->raddr, rma->size) == 0);
  (void) ofi_put(rma->addr, rma->b.node, rma->raddr, rma->size);

  //
  // Note: the RMA bytes must be visible in target memory before the
  // 'done' indicator is.
  //

  DBG_PRINTF(DBG_AM | DBG_AM_RECV, "%s", am_reqDoneStr((amRequest_t*) rma));
  amSendDone(rma->b.node, rma->b.pAmDone);
}


static
void amHandleAMO(struct amRequest_AMO_t* amo) {
  assert(amo->b.node != chpl_nodeID);    // should be handled on initiator

  chpl_amo_datum_t result;
  size_t resSize = amo->size;
  doCpuAMO(amo->obj, &amo->operand1, &amo->operand2, &result,
           amo->ofiOp, amo->ofiType, amo->size);

  if (amo->result != NULL) {
    CHK_TRUE(mrGetKey(NULL, NULL, amo->b.node, amo->result, resSize) == 0);
    (void) ofi_put(&result, amo->b.node, amo->result, resSize);

    //
    // Note: the result must be visible in target memory before the
    // 'done' indicator is.
    //
  }

  DBG_PRINTF(DBG_AM | DBG_AM_RECV, "%s", am_reqDoneStr((amRequest_t*) amo));
  if (amo->b.pAmDone != NULL) {
    amSendDone(amo->b.node, amo->b.pAmDone);
  }
}


static inline
void amSendDone(c_nodeid_t node, amDone_t* pAmDone) {
  static __thread amDone_t* amDone = NULL;
  if (amDone == NULL) {
    amDone = allocBounceBuf(1);
    *amDone = 1;
  }

  //
  // Send the 'done' indicator.  Try to just inject it, thus generating
  // no completion event.  If we can't do that we'll send it the normal
  // way, but consume the completion later rather than waiting for it
  // now.
  //
  ofi_put_ll(amDone, node, pAmDone, sizeof(*pAmDone),
             txnTrkEncodeId(__LINE__), amTcip, true /*useInject*/);
}


static inline
void amCheckLiveness(void) {
  //
  // Only node 0 does liveness checks.  It cycles through the others,
  // checking to make sure we can AM to them.  To minimize overhead, we
  // try not to do a liveness check any more frequently than about every
  // 10 seconds and we also try not to make time calls much more often
  // than that, because they're expensive.  A "liveness check" is really
  // just a check that we can send a no-op AM without an unrecoverable
  // error resulting.  That's sufficient to get us an -EMFILE return if
  // we run up against the open file limit, for example.
  //
  const double timeInterval = 10.0;
  static __thread double lastTime = 0.0;

  static __thread int countInterval = 10000;
  static __thread int count;

  if (lastTime == 0.0) {
    //
    // The first time we've been called, initialize.
    //
    lastTime = chpl_comm_ofi_time_get();
    count = countInterval;
  } else if (--count == 0) {
    //
    // After the first time, do the "liveness" checks and adjust the
    // counter interval as needed.
    //
    double time = chpl_comm_ofi_time_get();

    double timeRatio = (time - lastTime) / timeInterval;
    const double minTimeRatio = 3.0 / 4.0;
    const double maxTimeRatio = 4.0 / 3.0;
    if (timeRatio < minTimeRatio) {
      timeRatio = minTimeRatio;
    } else if (timeRatio > maxTimeRatio) {
      timeRatio = maxTimeRatio;
    }
    countInterval /= timeRatio;

    static __thread c_nodeid_t node = 1;
    if (--node == 0) {
      node = chpl_numNodes - 1;
    }
    amRequestNop(node, false /*blocking*/);
    count = countInterval;
    lastTime = time;
  }
}


////////////////////////////////////////
//
// Interface: RMA
//

chpl_comm_nb_handle_t chpl_comm_put_nb(void* addr, c_nodeid_t node,
                                       void* raddr, size_t size,
                                       int32_t commID, int ln, int32_t fn) {
  chpl_comm_put(addr, node, raddr, size, commID, ln, fn);
  return NULL;
}


chpl_comm_nb_handle_t chpl_comm_get_nb(void* addr, c_nodeid_t node,
                                       void* raddr, size_t size,
                                       int32_t commID, int ln, int32_t fn) {
  chpl_comm_get(addr, node, raddr, size, commID, ln, fn);
  return NULL;
}


int chpl_comm_test_nb_complete(chpl_comm_nb_handle_t h) {
  chpl_comm_diags_incr(test_nb);

  // fi_cq_readfrom?
  return ((void*) h) == NULL;
}


void chpl_comm_wait_nb_some(chpl_comm_nb_handle_t* h, size_t nhandles) {
  chpl_comm_diags_incr(wait_nb);

  size_t i;
  // fi_cq_readfrom?
  for( i = 0; i < nhandles; i++ ) {
    CHK_TRUE(h[i] == NULL);
  }
}


int chpl_comm_try_nb_some(chpl_comm_nb_handle_t* h, size_t nhandles) {
  chpl_comm_diags_incr(try_nb);

  size_t i;
  // fi_cq_readfrom?
  for( i = 0; i < nhandles; i++ ) {
    CHK_TRUE(h[i] == NULL);
  }
  return 0;
}


void chpl_comm_put(void* addr, c_nodeid_t node, void* raddr,
                   size_t size, int32_t commID, int ln, int32_t fn) {
  DBG_PRINTF(DBG_IFACE,
             "%s(%p, %d, %p, %zd, %d)", __func__,
             addr, (int) node, raddr, size, (int) commID);

  retireDelayedAmDone(false /*taskIsEnding*/);

  //
  // Sanity checks, self-communication.
  //
  CHK_TRUE(addr != NULL);
  CHK_TRUE(raddr != NULL);

  if (size == 0) {
    return;
  }

  if (node == chpl_nodeID) {
    memmove(raddr, addr, size);
    return;
  }

  // Communications callback support
  if (chpl_comm_have_callbacks(chpl_comm_cb_event_kind_put)) {
      chpl_comm_cb_info_t cb_data =
        {chpl_comm_cb_event_kind_put, chpl_nodeID, node,
         .iu.comm={addr, raddr, size, commID, ln, fn}};
      chpl_comm_do_callbacks (&cb_data);
  }

  chpl_comm_diags_verbose_rdma("put", node, size, ln, fn, commID);
  chpl_comm_diags_incr(put);

  (void) ofi_put(addr, node, raddr, size);
}


void chpl_comm_get(void* addr, int32_t node, void* raddr,
                   size_t size, int32_t commID, int ln, int32_t fn) {
  DBG_PRINTF(DBG_IFACE,
             "%s(%p, %d, %p, %zd, %d)", __func__,
             addr, (int) node, raddr, size, (int) commID);

  retireDelayedAmDone(false /*taskIsEnding*/);

  //
  // Sanity checks, self-communication.
  //
  CHK_TRUE(addr != NULL);
  CHK_TRUE(raddr != NULL);

  if (size == 0) {
    return;
  }

  if (node == chpl_nodeID) {
    memmove(addr, raddr, size);
    return;
  }

  // Communications callback support
  if (chpl_comm_have_callbacks(chpl_comm_cb_event_kind_get)) {
      chpl_comm_cb_info_t cb_data =
        {chpl_comm_cb_event_kind_get, chpl_nodeID, node,
         .iu.comm={addr, raddr, size, commID, ln, fn}};
      chpl_comm_do_callbacks (&cb_data);
  }

  chpl_comm_diags_verbose_rdma("get", node, size, ln, fn, commID);
  chpl_comm_diags_incr(get);

  (void) ofi_get(addr, node, raddr, size);
}


void chpl_comm_put_strd(void* dstaddr_arg, size_t* dststrides,
                        c_nodeid_t dstnode,
                        void* srcaddr_arg, size_t* srcstrides,
                        size_t* count, int32_t stridelevels, size_t elemSize,
                        int32_t commID, int ln, int32_t fn) {
  DBG_PRINTF(DBG_IFACE,
             "%s(%p, %p, %d, %p, %p, %p, %d, %zd, %d)", __func__,
             dstaddr_arg, dststrides, (int) dstnode, srcaddr_arg, srcstrides,
             count, (int) stridelevels, elemSize, (int) commID);

  put_strd_common(dstaddr_arg, dststrides,
                  dstnode,
                  srcaddr_arg, srcstrides,
                  count, stridelevels, elemSize,
                  1, NULL,
                  commID, ln, fn);
}


void chpl_comm_get_strd(void* dstaddr_arg, size_t* dststrides,
                        c_nodeid_t srcnode,
                        void* srcaddr_arg, size_t* srcstrides, size_t* count,
                        int32_t stridelevels, size_t elemSize,
                        int32_t commID, int ln, int32_t fn) {
  DBG_PRINTF(DBG_IFACE,
             "%s(%p, %p, %d, %p, %p, %p, %d, %zd, %d)", __func__,
             dstaddr_arg, dststrides, (int) srcnode, srcaddr_arg, srcstrides,
             count, (int) stridelevels, elemSize, (int) commID);

  get_strd_common(dstaddr_arg, dststrides,
                  srcnode,
                  srcaddr_arg, srcstrides,
                  count, stridelevels, elemSize,
                  1, NULL,
                  commID, ln, fn);
}


void chpl_comm_getput_unordered(c_nodeid_t dstnode, void* dstaddr,
                                c_nodeid_t srcnode, void* srcaddr,
                                size_t size, int32_t commID,
                                int ln, int32_t fn) {
  DBG_PRINTF(DBG_IFACE,
             "%s(%d, %p, %d, %p, %zd, %d)", __func__,
             (int) dstnode, dstaddr, (int) srcnode, srcaddr, size,
             (int) commID);

  assert(dstaddr != NULL);
  assert(srcaddr != NULL);

  if (size == 0)
    return;

  if (dstnode == chpl_nodeID && srcnode == chpl_nodeID) {
    retireDelayedAmDone(false /*taskIsEnding*/);
    memmove(dstaddr, srcaddr, size);
    return;
  }

  if (dstnode == chpl_nodeID) {
    chpl_comm_get_unordered(dstaddr, srcnode, srcaddr, size, commID, ln, fn);
  } else if (srcnode == chpl_nodeID) {
    chpl_comm_put_unordered(srcaddr, dstnode, dstaddr, size, commID, ln, fn);
  } else {
    if (size <= MAX_UNORDERED_TRANS_SZ) {
      char buf[MAX_UNORDERED_TRANS_SZ];
      chpl_comm_get(buf, srcnode, srcaddr, size, commID, ln, fn);
      chpl_comm_put(buf, dstnode, dstaddr, size, commID, ln, fn);
    } else {
      // Note, we do not expect this case to trigger, but if it does we may
      // want to do on-stmt to src node and then transfer
      char* buf = chpl_mem_alloc(size, CHPL_RT_MD_COMM_PER_LOC_INFO, 0, 0);
      chpl_comm_get(buf, srcnode, srcaddr, size, commID, ln, fn);
      chpl_comm_put(buf, dstnode, dstaddr, size, commID, ln, fn);
      chpl_mem_free(buf, 0, 0);
    }
  }
}


void chpl_comm_get_unordered(void* addr, c_nodeid_t node, void* raddr,
                             size_t size, int32_t commID, int ln, int32_t fn) {
  DBG_PRINTF(DBG_IFACE,
             "%s(%p, %d, %p, %zd, %d)", __func__,
             addr, (int) node, raddr, size, (int) commID);

  retireDelayedAmDone(false /*taskIsEnding*/);

  //
  // Sanity checks, self-communication.
  //
  CHK_TRUE(addr != NULL);
  CHK_TRUE(raddr != NULL);

  if (size == 0) {
    return;
  }

  if (node == chpl_nodeID) {
    memmove(addr, raddr, size);
    return;
  }

  // Communications callback support
  if (chpl_comm_have_callbacks(chpl_comm_cb_event_kind_get)) {
      chpl_comm_cb_info_t cb_data =
        {chpl_comm_cb_event_kind_get, chpl_nodeID, node,
         .iu.comm={addr, raddr, size, commID, ln, fn}};
      chpl_comm_do_callbacks (&cb_data);
  }

  chpl_comm_diags_verbose_rdma("unordered get", node, size, ln, fn, commID);
  chpl_comm_diags_incr(get);

  do_remote_get_buff(addr, node, raddr, size);
}


void chpl_comm_put_unordered(void* addr, c_nodeid_t node, void* raddr,
                             size_t size, int32_t commID, int ln, int32_t fn) {
  DBG_PRINTF(DBG_IFACE,
             "%s(%p, %d, %p, %zd, %d)", __func__,
             addr, (int) node, raddr, size, (int) commID);

  retireDelayedAmDone(false /*taskIsEnding*/);

  //
  // Sanity checks, self-communication.
  //
  CHK_TRUE(addr != NULL);
  CHK_TRUE(raddr != NULL);

  if (size == 0) {
    return;
  }

  if (node == chpl_nodeID) {
    memmove(raddr, addr, size);
    return;
  }

  // Communications callback support
  if (chpl_comm_have_callbacks(chpl_comm_cb_event_kind_put)) {
      chpl_comm_cb_info_t cb_data =
        {chpl_comm_cb_event_kind_put, chpl_nodeID, node,
         .iu.comm={addr, raddr, size, commID, ln, fn}};
      chpl_comm_do_callbacks (&cb_data);
  }

  chpl_comm_diags_verbose_rdma("unordered put", node, size, ln, fn, commID);
  chpl_comm_diags_incr(put);

  do_remote_put_buff(addr, node, raddr, size);
}


void chpl_comm_getput_unordered_task_fence(void) {
  DBG_PRINTF(DBG_IFACE_MCM, "%s()", __func__);

  task_local_buff_flush(get_buff | put_buff);
}


////////////////////////////////////////
//
// Internal communication support
//

static inline struct perTxCtxInfo_t* tciAllocCommon(chpl_bool);
static struct perTxCtxInfo_t* findFreeTciTabEntry(chpl_bool);

static __thread struct perTxCtxInfo_t* _ttcip;


static inline
struct perTxCtxInfo_t* tciAlloc(void) {
  return tciAllocCommon(false /*bindToAmHandler*/);
}


static inline
struct perTxCtxInfo_t* tciAllocForAmHandler(void) {
  return tciAllocCommon(true /*bindToAmHandler*/);
}


static inline
struct perTxCtxInfo_t* tciAllocCommon(chpl_bool bindToAmHandler) {
  if (_ttcip != NULL) {
    //
    // If the last tx context we used is bound to our thread or can be
    // re-allocated, use that.
    //
    if (_ttcip->bound) {
      DBG_PRINTF(DBG_TCIPS, "realloc bound tciTab[%td]", _ttcip - tciTab);
      return _ttcip;
    }

    if (!atomic_exchange_bool(&_ttcip->allocated, true)) {
      DBG_PRINTF(DBG_TCIPS, "realloc tciTab[%td]", _ttcip - tciTab);
      return _ttcip;
    }
  }

  //
  // Find a tx context that isn't busy and use that one.  If this is
  // for either the AM handler or a tasking layer fixed worker thread,
  // bind it permanently.
  //
  _ttcip = findFreeTciTabEntry(bindToAmHandler);
  if (bindToAmHandler
      || (tciTabFixedAssignments && chpl_task_isFixedThread())) {
    _ttcip->bound = true;
  }
  DBG_PRINTF(DBG_TCIPS, "alloc%s tciTab[%td]",
             _ttcip->bound ? " bound" : "", _ttcip - tciTab);
  return _ttcip;
}


static
struct perTxCtxInfo_t* findFreeTciTabEntry(chpl_bool bindToAmHandler) {
  //
  // Find a tx context that isn't busy.  Note that tx contexts for
  // AM handlers and other threads come out of different blocks of
  // the table.
  //
  const int numWorkerTxCtxs = tciTabLen - numAmHandlers;
  struct perTxCtxInfo_t* tcip;

  if (bindToAmHandler) {
    //
    // AM handlers use tciTab[numWorkerTxCtxs .. tciTabLen - 1].  For
    // now we only support a single AM handler, so this is simple.  If
    // we ever have more, the CHK_FALSE will force us to revisit this.
    //
    tcip = &tciTab[numWorkerTxCtxs];
    CHK_FALSE(atomic_exchange_bool(&tcip->allocated, true));
    return tcip;
  }

  //
  // Workers use tciTab[0 .. numWorkerTxCtxs - 1].  Search forever for
  // an entry we can use.  Give up (and kill the program) only if we
  // discover they're all bound, because if that's true we can predict
  // we'll never find a free one.
  //
  static __thread int last_iw = 0;
  tcip = NULL;

  do {
    int iw = last_iw;
    chpl_bool allBound = true;

    do {
      if (++iw >= numWorkerTxCtxs)
        iw = 0;
      allBound = allBound && tciTab[iw].bound;
      if (!atomic_exchange_bool(&tciTab[iw].allocated, true)) {
        tcip = &tciTab[iw];
      }
    } while (tcip == NULL && iw != last_iw);

    if (tcip == NULL) {
      CHK_FALSE(allBound);
      local_yield();
    }
  } while (tcip == NULL);

  return tcip;
}


static inline
void tciFree(struct perTxCtxInfo_t* tcip) {
  //
  // Bound contexts stay bound.  We only release non-bound ones.
  //
  if (!tcip->bound) {
    DBG_PRINTF(DBG_TCIPS, "free tciTab[%td]", tcip - tciTab);
    atomic_store_bool(&tcip->allocated, false);
  }
}


static inline
chpl_comm_nb_handle_t ofi_put(const void* addr, c_nodeid_t node,
                              void* raddr, size_t size) {
  //
  // Don't ask the provider to transfer more than it wants to.
  //
  if (size > ofi_info->ep_attr->max_msg_size) {
    DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE,
               "splitting large PUT %d:%p <= %p, size %zd",
               (int) node, raddr, addr, size);

    size_t chunkSize = ofi_info->ep_attr->max_msg_size;
    for (size_t i = 0; i < size; i += chunkSize) {
      if (chunkSize > size - i) {
        chunkSize = size - i;
      }
      (void) ofi_put(&((const char*) addr)[i], node, &((char*) raddr)[i],
                     chunkSize);
    }

    return NULL;
  }

  DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE,
             "PUT %d:%p <= %p, size %zd",
             (int) node, raddr, addr, size);

  void* myAddr = (void*) addr;

  uint64_t mrKey;
  uint64_t mrRaddr;
  if (mrGetKey(&mrKey, &mrRaddr, node, raddr, size) == 0) {
    //
    // The remote address is RMA-accessible; PUT directly to it.
    //
    void* mrDesc = NULL;
    if (mrGetDesc(&mrDesc, myAddr, size) != 0) {
      myAddr = allocBounceBuf(size);
      DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE, "PUT src BB: %p", myAddr);
      CHK_TRUE(mrGetDesc(&mrDesc, myAddr, size) == 0);
      memcpy(myAddr, addr, size);
    }

    struct perTxCtxInfo_t* tcip;
    CHK_TRUE((tcip = tciAlloc()) != NULL);

    //
    // If we're using delivery-complete for MCM conformance we just
    // write the data and wait for the CQ event.  If we're using message
    // ordering we have to force the data into visibility by following
    // the PUT with a dummy GET from the same node, taking advantage of
    // our asserted read-after-write ordering.  If we don't have bound
    // tx contexts we have to do that immediately, here, because message
    // ordering only works within endpoint pairs.  But if we do have
    // bound tx contexts we can delay that dummy GET or even avoid it
    // altogether, if a real GET happens to come along after this.  A
    // wrinkle is that we don't currently delay the GET if the PUT data
    // is too big to inject, because we want to return immediately and
    // that isn't safe until the source buffer has been injected.  But
    // this could be dealt with in the future by using fi_writemsg() and
    // asking for injection completion.
    //
    assert(tcip->txCQ != NULL);  // PUTs require a CQ, at least for now

    if (haveDeliveryComplete
        || !tcip->bound
        || size > ofi_info->tx_attr->inject_size) {
      atomic_bool txnDone;
      atomic_init_bool(&txnDone, false);
      void* ctx = txnTrkEncodeDone(&txnDone);

      DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE,
                 "tx write: %d:%p <= %p, size %zd, key 0x%" PRIx64 ", ctx %p",
                 (int) node, raddr, myAddr, size, mrKey, ctx);
      OFI_RIDE_OUT_EAGAIN(tcip,
                          fi_write(tcip->txCtx, myAddr, size,
                                   mrDesc, rxRmaAddr(tcip, node),
                                   mrRaddr, mrKey,
                                   haveDeliveryComplete ? ctx : NULL));
      tcip->numTxnsOut++;
      tcip->numTxnsSent++;

      if (!haveDeliveryComplete) {
        DBG_PRINTF(DBG_ORDER,
                   "dummy GET from %d for PUT ordering", (int) node);
        ofi_get_ll(orderDummy, node, orderDummyMap[node], 1, ctx, tcip);
      }

      waitForTxnComplete(tcip, ctx);
      atomic_destroy_bool(&txnDone);
    } else {
      DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE,
                 "tx write inject: %d:%p <= %p, size %zd, key 0x%" PRIx64,
                 (int) node, raddr, myAddr, size, mrKey);
      OFI_RIDE_OUT_EAGAIN(tcip,
                          fi_inject_write(tcip->txCtx, myAddr, size,
                                          rxRmaAddr(tcip, node),
                                          mrRaddr, mrKey));
      tcip->numTxnsSent++;
      chpl_comm_taskPrvData_t* prvData = get_comm_taskPrvdata();
      assert(prvData != NULL);
      if (prvData->putBitmap == NULL) {
        prvData->putBitmap = bitmapAlloc(chpl_numNodes);
      }
      bitmapSet(prvData->putBitmap, node);
    }

    tciFree(tcip);
  } else {
    //
    // The remote address is not RMA-accessible.  Make sure that the
    // local one is, then do the opposite RMA from the remote side.
    //
    if (mrGetLocalKey(myAddr, size) != 0) {
      myAddr = allocBounceBuf(size);
      DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE, "PUT via AM GET tgt BB: %p", myAddr);
      CHK_TRUE(mrGetLocalKey(myAddr, size) == 0);
      memcpy(myAddr, addr, size);
    }

    DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE,
               "PUT %d:%p <= %p, size %zd, via AM GET",
               (int) node, raddr, myAddr, size);
    amRequestRMA(node, am_opGet, myAddr, raddr, size);
  }

  if (myAddr != addr) {
    freeBounceBuf(myAddr);
  }

  return NULL;
}


static inline
void ofi_put_ll(const void* addr, c_nodeid_t node,
                void* raddr, size_t size, void* ctx,
                struct perTxCtxInfo_t* tcip, chpl_bool useInject) {
  uint64_t mrKey = 0;
  uint64_t mrRaddr = 0;
  CHK_TRUE(mrGetKey(&mrKey, &mrRaddr, node, raddr, size) == 0);

  void* myAddr = (void*) addr;
  void* mrDesc = NULL;
  CHK_TRUE(mrGetDesc(&mrDesc, myAddr, size) == 0);

  struct perTxCtxInfo_t* myTcip = tcip;
  if (myTcip == NULL) {
    CHK_TRUE((myTcip = tciAlloc()) != NULL);
  }

  //
  // Inject if we can, otherwise do a regular write.  Don't count inject
  // as an outstanding operation, because it won't generate a CQ event.
  //
  if (useInject && size <= ofi_info->tx_attr->inject_size) {
    DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE,
               "tx write ll inject: %d:%p <= %p, size %zd, key 0x%" PRIx64,
               (int) node, raddr, myAddr, size, mrKey);
    OFI_RIDE_OUT_EAGAIN(myTcip,
                        fi_inject_write(myTcip->txCtx, myAddr, size,
                                        rxRmaAddr(myTcip, node),
                                        mrRaddr, mrKey));
    myTcip->numTxnsSent++;
  } else {
    DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE,
               "tx write ll: %d:%p <= %p, size %zd, key 0x%" PRIx64 ", ctx %p",
               (int) node, raddr, myAddr, size, mrKey, ctx);
    OFI_RIDE_OUT_EAGAIN(myTcip,
                        fi_write(myTcip->txCtx, myAddr, size,
                                 mrDesc, rxRmaAddr(myTcip, node),
                                 mrRaddr, mrKey, ctx));
    myTcip->numTxnsOut++;
    myTcip->numTxnsSent++;
  }

  if (myTcip != tcip) {
    tciFree(myTcip);
  }
}


static
void ofi_put_V(int v_len, void** addr_v, void** local_mr_v,
               c_nodeid_t* locale_v, void** raddr_v, uint64_t* remote_mr_v,
               size_t* size_v, struct bitmap_t* b) {
  DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE | DBG_RMA_UNORD,
             "put_V(%d): %d:%p <= %p, size %zd, key 0x%" PRIx64,
             v_len, (int) locale_v[0], raddr_v[0], addr_v[0], size_v[0],
             remote_mr_v[0]);

  assert(!isAmHandler);

  struct perTxCtxInfo_t* tcip;
  CHK_TRUE((tcip = tciAlloc()) != NULL);

  //
  // Make sure we have enough free CQ entries to initiate the entire
  // batch of transactions.
  //
  if (tcip->txCQ != NULL && v_len > txCQLen - tcip->numTxnsOut) {
    (*tcip->checkTxCmplsFn)(tcip);
    while (v_len > txCQLen - tcip->numTxnsOut) {
      sched_yield();
      (*tcip->checkTxCmplsFn)(tcip);
    }
  }

  //
  // Initiate the batch.  Record which nodes we PUT to, so that we can
  // force them to be visible in target memory at the end.
  //
  bitmapZero(b);
  for (int vi = 0; vi < v_len; vi++) {
    struct iovec msg_iov = (struct iovec)
                           { .iov_base = addr_v[vi],
                             .iov_len = size_v[vi] };
    struct fi_rma_iov rma_iov = (struct fi_rma_iov)
                                { .addr = (uint64_t) raddr_v[vi],
                                  .len = size_v[vi],
                                  .key = remote_mr_v[vi] };
    struct fi_msg_rma msg = (struct fi_msg_rma)
                            { .msg_iov = &msg_iov,
                              .desc = &local_mr_v[vi],
                              .iov_count = 1,
                              .addr = rxRmaAddr(tcip, locale_v[vi]),
                              .rma_iov = &rma_iov,
                              .rma_iov_count = 1,
                              .context = txnTrkEncodeId(__LINE__),
                              .data = 0 };
    DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE,
               "tx writemsg: %d:%p <= %p, size %zd, key 0x%" PRIx64,
               (int) locale_v[vi], (void*) msg.rma_iov->addr,
               msg.msg_iov->iov_base, msg.msg_iov->iov_len, msg.rma_iov->key);
    //
    // Add another transaction to the group and go on without waiting.
    // Throw FI_MORE except for the last one in the batch.
    //
    OFI_RIDE_OUT_EAGAIN(tcip,
                        fi_writemsg(tcip->txCtx, &msg,
                                    (vi < v_len - 1) ? FI_MORE : 0));
    tcip->numTxnsOut++;
    tcip->numTxnsSent++;
    bitmapSet(b, locale_v[vi]);
  }

  //
  // Enforce Chapel MCM: force all of the above PUTs to appear in
  // target memory.
  //
  mcmReleaseAllNodes(b, tcip, "unordered PUT");

  tciFree(tcip);
}


/*
 *** START OF BUFFERED PUT OPERATIONS ***
 *
 * Support for buffered PUT operations. We internally buffer PUT operations and
 * then initiate them all at once for increased transaction rate.
 */

// Flush buffered PUTs for the specified task info and reset the counter.
static inline
void put_buff_task_info_flush(put_buff_task_info_t* info) {
  if (info->vi > 0) {
    DBG_PRINTF(DBG_RMA_UNORD,
               "put_buff_task_info_flush(): info has %d entries",
               info->vi);
    ofi_put_V(info->vi, info->src_addr_v, info->local_mr_v,
              info->locale_v, info->tgt_addr_v, info->remote_mr_v,
              info->size_v, &info->nodeBitmap);
    info->vi = 0;
  }
}


static inline
void do_remote_put_buff(void* addr, c_nodeid_t node, void* raddr,
                        size_t size) {
  uint64_t mrKey;
  uint64_t mrRaddr;
  put_buff_task_info_t* info;
  size_t extra_size = bitmapSizeofMap(chpl_numNodes);
  if (size > MAX_UNORDERED_TRANS_SZ
      || mrGetKey(&mrKey, &mrRaddr, node, raddr, size) != 0
      || (info = task_local_buff_acquire(put_buff, extra_size)) == NULL) {
    (void) ofi_put(addr, node, raddr, size);
    return;
  }

  if (info->new) {
    info->nodeBitmap.len = chpl_numNodes;
    info->new = false;
  }

  void* mrDesc = NULL;
  CHK_TRUE(mrGetDesc(&mrDesc, info->src_v, size) == 0);

  int vi = info->vi;
  memcpy(&info->src_v[vi], addr, size);
  info->src_addr_v[vi] = &info->src_v[vi];
  info->locale_v[vi] = node;
  info->tgt_addr_v[vi] = (void*) mrRaddr;
  info->size_v[vi] = size;
  info->remote_mr_v[vi] = mrKey;
  info->local_mr_v[vi] = mrDesc;
  info->vi++;

  DBG_PRINTF(DBG_RMA_UNORD,
             "do_remote_put_buff(): info[%d] = "
             "{%p, %d, %p, %zd, %" PRIx64 ", %p}",
             vi, info->src_addr_v[vi], (int) node, raddr, size, mrKey, mrDesc);

  // flush if buffers are full
  if (info->vi == MAX_CHAINED_PUT_LEN) {
    put_buff_task_info_flush(info);
  }
}
/*** END OF BUFFERED PUT OPERATIONS ***/


static inline
chpl_comm_nb_handle_t ofi_get(void* addr, c_nodeid_t node,
                              void* raddr, size_t size) {
  //
  // Don't ask the provider to transfer more than it wants to.
  //
  if (size > ofi_info->ep_attr->max_msg_size) {
    DBG_PRINTF(DBG_RMA | DBG_RMA_READ,
               "splitting large GET %p <= %d:%p, size %zd",
               addr, (int) node, raddr, size);

    size_t chunkSize = ofi_info->ep_attr->max_msg_size;
    for (size_t i = 0; i < size; i += chunkSize) {
      if (chunkSize > size - i) {
        chunkSize = size - i;
      }
      (void) ofi_get(&((char*) addr)[i], node, &((char*) raddr)[i],
                     chunkSize);
    }

    return NULL;
  }

  DBG_PRINTF(DBG_RMA | DBG_RMA_READ,
             "GET %p <= %d:%p, size %zd",
             addr, (int) node, raddr, size);

  void* myAddr = addr;

  uint64_t mrKey;
  uint64_t mrRaddr;
  if (mrGetKey(&mrKey, &mrRaddr, node, raddr, size) == 0) {
    //
    // The remote address is RMA-accessible; GET directly from it.
    //
    void* mrDesc = NULL;
    if (mrGetDesc(&mrDesc, myAddr, size) != 0) {
      myAddr = allocBounceBuf(size);
      DBG_PRINTF(DBG_RMA | DBG_RMA_READ, "GET tgt BB: %p", myAddr);
      CHK_TRUE(mrGetDesc(&mrDesc, myAddr, size) == 0);
    }

    struct perTxCtxInfo_t* tcip;
    CHK_TRUE((tcip = tciAlloc()) != NULL);

    atomic_bool txnDone;
    atomic_init_bool(&txnDone, false);
    void* ctx = (tcip->txCQ == NULL)
                ? txnTrkEncodeId(__LINE__)
                : txnTrkEncodeDone(&txnDone);

    DBG_PRINTF(DBG_RMA | DBG_RMA_READ,
               "tx read: %p <= %d:%p(0x%" PRIx64 "), size %zd, key 0x%" PRIx64
               ", ctx %p",
               myAddr, (int) node, raddr, mrRaddr, size, mrKey, ctx);
    OFI_RIDE_OUT_EAGAIN(tcip,
                        fi_read(tcip->txCtx, myAddr, size,
                                mrDesc, rxRmaAddr(tcip, node),
                                mrRaddr, mrKey, ctx));
    tcip->numTxnsOut++;
    tcip->numTxnsSent++;

    //
    // This GET will force any outstanding PUT to the same node
    // to be visible.
    //
    if (!haveDeliveryComplete && tcip->bound) {
      chpl_comm_taskPrvData_t* prvData = get_comm_taskPrvdata();
      assert(prvData != NULL);
      if (prvData->putBitmap != NULL) {
        bitmapClear(prvData->putBitmap, node);
      }
    }

    waitForTxnComplete(tcip, ctx);
    atomic_destroy_bool(&txnDone);
    tciFree(tcip);
  } else {
    //
    // The remote address is not RMA-accessible.  Make sure that the
    // local one is, then do the opposite RMA from the remote side.
    //
    if (mrGetLocalKey(myAddr, size) != 0) {
      myAddr = allocBounceBuf(size);
      DBG_PRINTF(DBG_RMA | DBG_RMA_READ, "GET via AM PUT src BB: %p", myAddr);
      CHK_TRUE(mrGetLocalKey(myAddr, size) == 0);
    }

    DBG_PRINTF(DBG_RMA | DBG_RMA_READ,
               "GET %p <= %d:%p, size %zd, via AM PUT",
               myAddr, (int) node, raddr, size);
    amRequestRMA(node, am_opPut, myAddr, raddr, size);
  }

  if (myAddr != addr) {
    memcpy(addr, myAddr, size);
    freeBounceBuf(myAddr);
  }

  return NULL;
}


static inline
void ofi_get_ll(void* addr, c_nodeid_t node,
                void* raddr, size_t size, void* ctx,
                struct perTxCtxInfo_t* tcip) {
  DBG_PRINTF(DBG_RMA | DBG_RMA_READ,
             "GET LL %p <= %d:%p, size %zd",
             addr, (int) node, raddr, size);

  uint64_t mrKey = 0;
  uint64_t mrRaddr = 0;
  CHK_TRUE(mrGetKey(&mrKey, &mrRaddr, node, raddr, size) == 0);

  void* myAddr = (void*) addr;
  void* mrDesc = NULL;
  CHK_TRUE(mrGetDesc(&mrDesc, myAddr, size) == 0);

  struct perTxCtxInfo_t* myTcip = tcip;
  if (myTcip == NULL) {
    CHK_TRUE((myTcip = tciAlloc()) != NULL);
  }

  DBG_PRINTF(DBG_RMA | DBG_RMA_READ,
             "tx read: %p <= %d:%p(0x%" PRIx64 "), size %zd, key 0x%" PRIx64
             ", ctx %p",
             myAddr, (int) node, raddr, mrRaddr, size, mrKey, ctx);
  OFI_RIDE_OUT_EAGAIN(myTcip,
                      fi_read(myTcip->txCtx, myAddr, size,
                              mrDesc, rxRmaAddr(myTcip, node),
                              mrRaddr, mrKey, ctx));
  myTcip->numTxnsOut++;
  myTcip->numTxnsSent++;

  if (myTcip != tcip) {
    tciFree(myTcip);
  }
}


static
void ofi_get_V(int v_len, void** addr_v, void** local_mr_v,
               c_nodeid_t* locale_v, void** raddr_v, uint64_t* remote_mr_v,
               size_t* size_v) {
  DBG_PRINTF(DBG_RMA | DBG_RMA_READ | DBG_RMA_UNORD,
             "get_V(%d): %p <= %d:%p, size %zd, key 0x%" PRIx64,
             v_len, addr_v[0], (int) locale_v[0], raddr_v[0], size_v[0],
             remote_mr_v[0]);

  assert(!isAmHandler);

  struct perTxCtxInfo_t* tcip;
  CHK_TRUE((tcip = tciAlloc()) != NULL);

  //
  // Make sure we have enough free CQ entries to initiate the entire
  // batch of transactions.
  //
  if (tcip->txCQ != NULL && v_len > txCQLen - tcip->numTxnsOut) {
    (*tcip->checkTxCmplsFn)(tcip);
    while (v_len > txCQLen - tcip->numTxnsOut) {
      sched_yield();
      (*tcip->checkTxCmplsFn)(tcip);
    }
  }

  for (int vi = 0; vi < v_len; vi++) {
    struct iovec msg_iov = (struct iovec)
                           { .iov_base = addr_v[vi],
                             .iov_len = size_v[vi] };
    struct fi_rma_iov rma_iov = (struct fi_rma_iov)
                                { .addr = (uint64_t) raddr_v[vi],
                                  .len = size_v[vi],
                                  .key = remote_mr_v[vi] };
    struct fi_msg_rma msg = (struct fi_msg_rma)
                            { .msg_iov = &msg_iov,
                              .desc = &local_mr_v[vi],
                              .iov_count = 1,
                              .addr = rxRmaAddr(tcip, locale_v[vi]),
                              .rma_iov = &rma_iov,
                              .rma_iov_count = 1,
                              .context = txnTrkEncodeId(__LINE__),
                              .data = 0 };
    DBG_PRINTF(DBG_RMA | DBG_RMA_READ,
               "tx readmsg: %p <= %d:%p, size %zd, key 0x%" PRIx64,
               msg.msg_iov->iov_base, (int) locale_v[vi],
               (void*) msg.rma_iov->addr, msg.msg_iov->iov_len,
               msg.rma_iov->key);
    tcip->numTxnsOut++;
    tcip->numTxnsSent++;
    if (tcip->numTxnsOut < txCQLen && vi < v_len - 1) {
      // Add another transaction to the group and go on without waiting.
      OFI_RIDE_OUT_EAGAIN(tcip,
                          fi_readmsg(tcip->txCtx, &msg, FI_MORE));
    } else {
      // Initiate last transaction in group and wait for whole group.
      OFI_RIDE_OUT_EAGAIN(tcip,
                          fi_readmsg(tcip->txCtx, &msg, 0));
      while (tcip->numTxnsOut > 0) {
        (*tcip->ensureProgressFn)(tcip);
      }
    }
  }

  tciFree(tcip);
}


/*
 *** START OF BUFFERED GET OPERATIONS ***
 *
 * Support for buffered GET operations. We internally buffer GET operations and
 * then initiate them all at once for increased transaction rate.
 */

// Flush buffered GETs for the specified task info and reset the counter.
static inline
void get_buff_task_info_flush(get_buff_task_info_t* info) {
  if (info->vi > 0) {
    DBG_PRINTF(DBG_RMA_UNORD,
               "get_buff_task_info_flush(): info has %d entries",
               info->vi);
    ofi_get_V(info->vi, info->tgt_addr_v, info->local_mr_v,
              info->locale_v, info->src_addr_v, info->remote_mr_v,
              info->size_v);
    info->vi = 0;
  }
}


static inline
void do_remote_get_buff(void* addr, c_nodeid_t node, void* raddr,
                        size_t size) {
  uint64_t mrKey;
  uint64_t mrRaddr;
  get_buff_task_info_t* info;
  if (size > MAX_UNORDERED_TRANS_SZ
      || mrGetKey(&mrKey, &mrRaddr, node, raddr, size) != 0
      || (info = task_local_buff_acquire(get_buff, 0)) == NULL) {
    (void) ofi_get(addr, node, raddr, size);
    return;
  }

  void* mrDesc = NULL;
  CHK_TRUE(mrGetDesc(&mrDesc, addr, size) == 0);

  int vi = info->vi;
  info->tgt_addr_v[vi] = addr;
  info->locale_v[vi] = node;
  info->remote_mr_v[vi] = mrKey;
  info->src_addr_v[vi] = (void*) mrRaddr;
  info->size_v[vi] = size;
  info->local_mr_v[vi] = mrDesc;
  info->vi++;

  DBG_PRINTF(DBG_RMA_UNORD,
             "do_remote_get_buff(): info[%d] = "
             "{%p, %d, %" PRIx64 ", %p, %zd, %p}",
             vi, addr, (int) node, mrKey, raddr, size, mrDesc);

  // flush if buffers are full
  if (info->vi == MAX_CHAINED_GET_LEN) {
    get_buff_task_info_flush(info);
  }
}
/*** END OF BUFFERED GET OPERATIONS ***/


static
chpl_comm_nb_handle_t ofi_amo(c_nodeid_t node, uint64_t object, uint64_t mrKey,
                              const void* operand1, const void* operand2,
                              void* result,
                              enum fi_op ofiOp, enum fi_datatype ofiType,
                              size_t size) {
  void* myRes = result;
  void* mrDescRes = NULL;
  if (myRes != NULL && mrGetDesc(&mrDescRes, myRes, size) != 0) {
    myRes = allocBounceBuf(size);
    DBG_PRINTF((ofiOp == FI_ATOMIC_READ) ? DBG_AMO_READ : DBG_AMO,
               "AMO result BB: %p", myRes);
    CHK_TRUE(mrGetDesc(&mrDescRes, myRes, size) == 0);
  }

  void* myOpnd1 = (void*) operand1;
  void* mrDescOpnd1 = NULL;
  if (myOpnd1 != NULL && mrGetDesc(&mrDescOpnd1, myOpnd1, size) != 0) {
    myOpnd1 = allocBounceBuf(size);
    DBG_PRINTF(DBG_AMO, "AMO operand1 BB: %p", myOpnd1);
    CHK_TRUE(mrGetDesc(&mrDescOpnd1, myOpnd1, size) == 0);
    memcpy(myOpnd1, operand1, size);
  }

  void* myOpnd2 = (void*) operand2;
  void* mrDescOpnd2 = NULL;
  if (myOpnd2 != NULL && mrGetDesc(&mrDescOpnd2, myOpnd2, size) != 0) {
    myOpnd2 = allocBounceBuf(size);
    DBG_PRINTF(DBG_AMO, "AMO operand2 BB: %p", myOpnd2);
    CHK_TRUE(mrGetDesc(&mrDescOpnd2, myOpnd2, size) == 0);
    memcpy(myOpnd2, operand2, size);
  }

  struct perTxCtxInfo_t* tcip;
  CHK_TRUE((tcip = tciAlloc()) != NULL);

  if (ofiOp != FI_ATOMIC_READ) {
    waitForPutsVisAllNodes(tcip, NULL, false /*taskIsEnding*/);
  }

  atomic_bool txnDone;
  atomic_init_bool(&txnDone, false);
  void* ctx = (tcip->txCQ == NULL)
              ? txnTrkEncodeId(__LINE__)
              : txnTrkEncodeDone(&txnDone);

  DBG_PRINTF((ofiOp == FI_ATOMIC_READ) ? DBG_AMO_READ : DBG_AMO,
             "tx AMO: obj %d:%" PRIx64 ", opnd1 <%s>, opnd2 <%s>, "
             "op %s, typ %s, sz %zd, ctx %p",
             (int) node, object,
             DBG_VAL(myOpnd1, ofiType), DBG_VAL(myOpnd2, ofiType),
             amo_opName(ofiOp), amo_typeName(ofiType), size, ctx);

  if (ofiOp == FI_CSWAP) {
    OFI_CHK(fi_compare_atomic(tcip->txCtx,
                              myOpnd2, 1, mrDescOpnd2, myOpnd1, mrDescOpnd1,
                              myRes, mrDescRes,
                              rxRmaAddr(tcip, node), object, mrKey,
                              ofiType, ofiOp, ctx));
  } else if (result != NULL) {
    void* bufArg = myOpnd1;
    // Workaround for bug wherein operand1 is unused but nevertheless
    // must not be NULL.
    if (provCtl_readAmoNeedsOpnd) {
      if (ofiOp == FI_ATOMIC_READ && bufArg == NULL) {
        static int64_t dummy;
        bufArg = &dummy;
      }
    }
    OFI_CHK(fi_fetch_atomic(tcip->txCtx,
                            bufArg, 1, mrDescOpnd1, myRes, mrDescRes,
                            rxRmaAddr(tcip, node), object, mrKey,
                            ofiType, ofiOp, ctx));
  } else {
    OFI_CHK(fi_atomic(tcip->txCtx,
                      myOpnd1, 1, mrDescOpnd1,
                      rxRmaAddr(tcip, node), object, mrKey,
                      ofiType, ofiOp, ctx));
  }
  tcip->numTxnsOut++;
  tcip->numTxnsSent++;

  //
  // Wait for network completion.
  //
  waitForTxnComplete(tcip, ctx);
  atomic_destroy_bool(&txnDone);
  tciFree(tcip);

  if (result != NULL) {
    if (myRes != result) {
      memcpy(result, myRes, size);
      freeBounceBuf(myRes);
    }
  }

  if (result != NULL) {
    DBG_PRINTF((ofiOp == FI_ATOMIC_READ) ? DBG_AMO_READ : DBG_AMO,
               "  AMO result: %p is %s",
               result,
               DBG_VAL(result,  ofiType));
  }

  if (myOpnd1 != operand1) {
    freeBounceBuf(myOpnd1);
  }

  if (myOpnd2 != operand2) {
    freeBounceBuf(myOpnd2);
  }

  return NULL;
}


static
void ofi_amo_nf_V(int v_len, uint64_t* opnd1_v, void* local_mr,
                  c_nodeid_t* locale_v, void** object_v, uint64_t* remote_mr_v,
                  size_t* size_v, enum fi_op* cmd_v,
                  enum fi_datatype* type_v) {
  DBG_PRINTF(DBG_AMO | DBG_AMO_UNORD,
             "amo_nf_V(%d): obj %d:%p, opnd1 <%s>, op %s, typ %s, sz %zd, "
             "key 0x%" PRIx64,
             v_len, (int) locale_v[0], object_v[0],
             DBG_VAL(&opnd1_v[0], type_v[0]),
             amo_opName(cmd_v[0]), amo_typeName(type_v[0]), size_v[0],
             remote_mr_v[0]);

  assert(!isAmHandler);

  struct perTxCtxInfo_t* tcip;
  CHK_TRUE((tcip = tciAlloc()) != NULL);

  //
  // Make sure we have enough free CQ entries to initiate the entire
  // batch of transactions.
  //
  if (tcip->txCQ != NULL && v_len > txCQLen - tcip->numTxnsOut) {
    (*tcip->checkTxCmplsFn)(tcip);
    while (v_len > txCQLen - tcip->numTxnsOut) {
      sched_yield();
      (*tcip->checkTxCmplsFn)(tcip);
    }
  }

  //
  // Initiate the batch.
  //
  for (int vi = 0; vi < v_len; vi++) {
    struct fi_ioc msg_iov = (struct fi_ioc)
                            { .addr = &opnd1_v[vi],
                              .count = 1 };
    struct fi_rma_ioc rma_iov = (struct fi_rma_ioc)
                                { .addr = (uint64_t) object_v[vi],
                                  .count = 1,
                                  .key = remote_mr_v[vi] };
    struct fi_msg_atomic msg = (struct fi_msg_atomic)
                               { .msg_iov = &msg_iov,
                                 .desc = &local_mr,
                                 .iov_count = 1,
                                 .addr = rxRmaAddr(tcip, locale_v[vi]),
                                 .rma_iov = &rma_iov,
                                 .rma_iov_count = 1,
                                 .datatype = type_v[vi],
                                 .op = cmd_v[vi],
                                 .context = txnTrkEncodeId(__LINE__),
                                 .data = 0 };
    DBG_PRINTF(DBG_RMA | DBG_RMA_WRITE,
               "tx atomicmsg: obj %d:%p, opnd1 <%s>, op %s, typ %s, sz %zd, "
               "key 0x%" PRIx64,
               (int) locale_v[vi], (void*) msg.rma_iov->addr,
               DBG_VAL(msg.msg_iov->addr, msg.datatype), amo_opName(msg.op),
               amo_typeName(msg.datatype), size_v[vi], msg.rma_iov->key);
    //
    // Add another transaction to the group and go on without waiting.
    // Throw FI_MORE except for the last one in the batch.
    //
    OFI_RIDE_OUT_EAGAIN(tcip,
                        fi_atomicmsg(tcip->txCtx, &msg,
                                     (vi < v_len - 1) ? FI_MORE : 0));
    tcip->numTxnsOut++;
    tcip->numTxnsSent++;
  }

  tciFree(tcip);
}


void amEnsureProgress(struct perTxCtxInfo_t* tcip) {
  (*tcip->checkTxCmplsFn)(tcip);

  //
  // We only have responsibility for inbound AMs and RMA if we're doing
  // manual progress.
  //
  if (ofi_info->domain_attr->data_progress != FI_PROGRESS_MANUAL) {
    return;
  }

  if (ofi_amhPollSet != NULL) {
    void* contexts[pollSetSize];
    int ret;
    OFI_CHK_COUNT(fi_poll(ofi_amhPollSet, contexts, pollSetSize), ret);

    //
    // Process the CQs/counters that had events.  We really only have
    // to take any explicit actions for our transmit endpoint.  If we
    // have inbound AM messages we want to handle those in the main
    // poll loop.  And for the RMA endpoint we just need to ensure
    // progress, which the poll call itself will have done.
    //
    for (int i = 0; i < ret; i++) {
      if (contexts[i] == &ofi_rxCQ) {
        // no action
      } else if (contexts[i] == &tcip->checkTxCmplsFn) {
        (*tcip->checkTxCmplsFn)(tcip);
      } else if (contexts[i] == &checkRxRmaCmplsFn) {
        // no action
      } else {
        INTERNAL_ERROR_V("unexpected context %p from fi_poll()",
                         contexts[i]);
      }
    }
  } else {
    //
    // The provider can't do poll sets.
    //
    (*tcip->checkTxCmplsFn)(tcip);
    (*checkRxRmaCmplsFn)();
  }
}


static
void checkRxRmaCmplsCQ(void) {
  struct fi_cq_data_entry cqe;
  (void) readCQ(ofi_rxCQRma, &cqe, 1);
}


static
void checkRxRmaCmplsCntr(void) {
  (void) fi_cntr_read(ofi_rxCntrRma);
}


static
void checkTxCmplsCQ(struct perTxCtxInfo_t* tcip) {
  struct fi_cq_msg_entry cqes[txCQLen];
  const size_t cqesSize = sizeof(cqes) / sizeof(cqes[0]);
  const size_t numEvents = readCQ(tcip->txCQ, cqes, cqesSize);

  tcip->numTxnsOut -= numEvents;
  for (int i = 0; i < numEvents; i++) {
    struct fi_cq_msg_entry* cqe = &cqes[i];
    const txnTrkCtx_t trk = txnTrkDecode(cqe->op_context);
    DBG_PRINTF(DBG_ACK, "CQ ack tx, flags %#" PRIx64 ", ctx %d:%p",
               cqe->flags, trk.typ, trk.ptr);
    if (trk.typ == txnTrkDone) {
      atomic_store_explicit_bool((atomic_bool*) trk.ptr, true,
                                 memory_order_release);
    } else if (trk.typ != txnTrkId) {
      INTERNAL_ERROR_V("unexpected trk.typ %d", trk.typ);
    }
  }
}


static
void checkTxCmplsCntr(struct perTxCtxInfo_t* tcip) {
  uint64_t count = fi_cntr_read(tcip->txCntr);
  if (count > tcip->numTxnsSent) {
    INTERNAL_ERROR_V("fi_cntr_read() %" PRIu64 ", but numTxnsSent %" PRIu64,
                     count, tcip->numTxnsSent);
  }
  tcip->numTxnsOut = tcip->numTxnsSent - count;
}


static inline
size_t readCQ(struct fid_cq* cq, void* buf, size_t count) {
  ssize_t ret;
  CHK_TRUE((ret = fi_cq_read(cq, buf, count)) > 0
           || ret == -FI_EAGAIN
           || ret == -FI_EAVAIL);
  if (ret == -FI_EAVAIL) {
    reportCQError(cq);
  }
  return (ret == -FI_EAGAIN) ? 0 : ret;
}


static
void reportCQError(struct fid_cq* cq) {
  char err_data[ofi_info->domain_attr->max_err_data];
  struct fi_cq_err_entry err = (struct fi_cq_err_entry)
                               { .err_data = err_data,
                                 .err_data_size = sizeof(err_data) };
  fi_cq_readerr(cq, &err, 0);
  const txnTrkCtx_t trk = txnTrkDecode(err.op_context);
  if (err.err == FI_ETRUNC) {
    //
    // This only happens when reading from the CQ associated with the
    // inbound AM request multi-receive buffer.
    //
    // We ran out of inbound buffer space and a message was truncated.
    // If the fi_setopt(FI_OPT_MIN_MULTI_RECV) worked and nobody sent
    // anything larger than that, this shouldn't happen.  In any case,
    // we can't recover, but let's provide some information to help
    // aid failure analysis.
    //
    INTERNAL_ERROR_V("fi_cq_readerr(): AM recv buf FI_ETRUNC: "
                     "flags %#" PRIx64 ", len %zd, olen %zd, ctx %d:%p",
                     err.flags, err.len, err.olen, trk.typ, trk.ptr);
  } else {
    char buf[100];
    const char* errStr = fi_cq_strerror(cq, err.prov_errno, err.err_data,
                                        buf, sizeof(buf));
    INTERNAL_ERROR_V("fi_cq_read(): err %d, prov_errno %d, errStr %s, "
                     "ctx %d:%p",
                     err.err, err.prov_errno, errStr, trk.typ, trk.ptr);
  }
}


static inline
void waitForTxnComplete(struct perTxCtxInfo_t* tcip, void* ctx) {
  (*tcip->ensureProgressFn)(tcip);
  const txnTrkCtx_t trk = txnTrkDecode(ctx);
  if (trk.typ == txnTrkDone) {
    while (!atomic_load_explicit_bool((atomic_bool*) trk.ptr,
                                      memory_order_acquire)) {
      sched_yield();
      (*tcip->ensureProgressFn)(tcip);
    }
  } else {
    while (tcip->numTxnsOut > 0) {
      sched_yield();
      (*tcip->ensureProgressFn)(tcip);
    }
  }
}


static inline
void waitForPutsVisOneNode(c_nodeid_t node, struct perTxCtxInfo_t* tcip,
                           chpl_comm_taskPrvData_t* prvData) {
  //
  // Enforce MCM: at the end of a task, make sure all our outstanding
  // PUTs have actually completed on their target nodes.  Note that
  // we can only have PUTs outstanding if we're forced to use message
  // ordering because the provider lacks delivery-complete and we've
  // got a bound tx context.
  //
  if (!haveDeliveryComplete && tcip->bound) {
    chpl_comm_taskPrvData_t* myPrvData = prvData;
    if (myPrvData == NULL) {
      CHK_TRUE((myPrvData = get_comm_taskPrvdata()) != NULL);
    }

    if (myPrvData->putBitmap != NULL
        && bitmapTest(myPrvData->putBitmap, node)) {
      bitmapClear(myPrvData->putBitmap, node);
      mcmReleaseOneNode(node, tcip, "PUT");
    }
  }
}


static inline
void waitForPutsVisAllNodes(struct perTxCtxInfo_t* tcip,
                            chpl_comm_taskPrvData_t* prvData,
                            chpl_bool taskIsEnding) {
  //
  // Enforce MCM: at the end of a task, make sure all our outstanding
  // PUTs have actually completed on their target nodes.  Note that
  // we can only have PUTs outstanding if we're forced to use message
  // ordering because the provider lacks delivery-complete and we've
  // got a bound tx context.
  //
  if (chpl_numNodes > 1 && !haveDeliveryComplete) {
    struct perTxCtxInfo_t* myTcip = tcip;
    if (myTcip == NULL) {
      CHK_TRUE((myTcip = tciAlloc()) != NULL);
    }

    if (myTcip->bound) {
      chpl_comm_taskPrvData_t* myPrvData = prvData;
      if (myPrvData == NULL) {
        CHK_TRUE((myPrvData = get_comm_taskPrvdata()) != NULL);
      }

      if (myPrvData->putBitmap != NULL) {
        mcmReleaseAllNodes(myPrvData->putBitmap, NULL, "PUT");
        if (taskIsEnding) {
          bitmapFree(myPrvData->putBitmap);
          myPrvData->putBitmap = NULL;
        }
      }
    }

    if (myTcip != tcip) {
      tciFree(myTcip);
    }
  }
}


static
void* allocBounceBuf(size_t size) {
  void* p;
  CHPL_CALLOC_SZ(p, 1, size);
  return p;
}


static
void freeBounceBuf(void* p) {
  CHPL_FREE(p);
}


static inline
void local_yield(void) {
#ifdef CHPL_COMM_DEBUG
  pthread_t pthreadWas = { 0 };
  if (chpl_task_isFixedThread()) {
    pthreadWas = pthread_self();
  }
#endif

  //
  // Our task cannot make progress.  Yield, to allow some other task to
  // free up whatever resource we need.
  //
  // DANGER: Don't call this function on a worker thread while holding
  //         a tciTab[] entry, that is, between tcip=tciAlloc() and
  //         tciFree().  If you do and your task switches threads due
  //         to the chpl_task_yield(), we can end up with two threads
  //         using the same tciTab[] entry simultaneously.
  //
  chpl_task_yield();

#ifdef CHPL_COMM_DEBUG
  //
  // There are things in the comm layer that will break if tasks can
  // switch threads when they think their thread is fixed.
  //
  if (chpl_task_isFixedThread()) {
    CHK_TRUE(pthread_self() == pthreadWas);
  }
#endif
}


////////////////////////////////////////
//
// Interface: network atomics
//

static inline void doAMO(c_nodeid_t, void*, const void*, const void*, void*,
                         int, enum fi_datatype, size_t);


//
// WRITE
//
#define DEFN_CHPL_COMM_ATOMIC_WRITE(fnType, ofiType, Type)              \
  void chpl_comm_atomic_write_##fnType                                  \
         (void* desired, c_nodeid_t node, void* object,                 \
          memory_order order, int ln, int32_t fn) {                     \
    DBG_PRINTF(DBG_IFACE_AMO_WRITE,                                     \
               "%s(%p, %d, %p, %d, %s)", __func__,                      \
               desired, (int) node, object,                             \
               ln, chpl_lookupFilename(fn));                            \
    chpl_comm_diags_verbose_amo("amo write", node, ln, fn);             \
    chpl_comm_diags_incr(amo);                                          \
    doAMO(node, object, desired, NULL, NULL,                            \
          FI_ATOMIC_WRITE, ofiType, sizeof(Type));                      \
  }

DEFN_CHPL_COMM_ATOMIC_WRITE(int32, FI_INT32, int32_t)
DEFN_CHPL_COMM_ATOMIC_WRITE(int64, FI_INT64, int64_t)
DEFN_CHPL_COMM_ATOMIC_WRITE(uint32, FI_UINT32, uint32_t)
DEFN_CHPL_COMM_ATOMIC_WRITE(uint64, FI_UINT64, uint64_t)
DEFN_CHPL_COMM_ATOMIC_WRITE(real32, FI_FLOAT, _real32)
DEFN_CHPL_COMM_ATOMIC_WRITE(real64, FI_DOUBLE, _real64)


//
// READ
//
#define DEFN_CHPL_COMM_ATOMIC_READ(fnType, ofiType, Type)               \
  void chpl_comm_atomic_read_##fnType                                   \
         (void* result, c_nodeid_t node, void* object,                  \
          memory_order order, int ln, int32_t fn) {                     \
    DBG_PRINTF(DBG_IFACE_AMO_READ,                                      \
               "%s(%p, %d, %p, %d, %s)", __func__,                      \
               result, (int) node, object,                              \
               ln, chpl_lookupFilename(fn));                            \
    chpl_comm_diags_verbose_amo("amo read", node, ln, fn);              \
    chpl_comm_diags_incr(amo);                                          \
    doAMO(node, object, NULL, NULL, result,                             \
          FI_ATOMIC_READ, ofiType, sizeof(Type));                       \
  }

DEFN_CHPL_COMM_ATOMIC_READ(int32, FI_INT32, int32_t)
DEFN_CHPL_COMM_ATOMIC_READ(int64, FI_INT64, int64_t)
DEFN_CHPL_COMM_ATOMIC_READ(uint32, FI_UINT32, uint32_t)
DEFN_CHPL_COMM_ATOMIC_READ(uint64, FI_UINT64, uint64_t)
DEFN_CHPL_COMM_ATOMIC_READ(real32, FI_FLOAT, _real32)
DEFN_CHPL_COMM_ATOMIC_READ(real64, FI_DOUBLE, _real64)


#define DEFN_CHPL_COMM_ATOMIC_XCHG(fnType, ofiType, Type)               \
  void chpl_comm_atomic_xchg_##fnType                                   \
         (void* desired, c_nodeid_t node, void* object, void* result,   \
          memory_order order, int ln, int32_t fn) {                     \
    DBG_PRINTF(DBG_IFACE_AMO,                                           \
               "%s(%p, %d, %p, %p, %d, %s)", __func__,                  \
               desired, (int) node, object, result,                     \
               ln, chpl_lookupFilename(fn));                            \
    chpl_comm_diags_verbose_amo("amo xchg", node, ln, fn);              \
    chpl_comm_diags_incr(amo);                                          \
    doAMO(node, object, desired, NULL, result,                          \
          FI_ATOMIC_WRITE, ofiType, sizeof(Type));                      \
  }

DEFN_CHPL_COMM_ATOMIC_XCHG(int32, FI_INT32, int32_t)
DEFN_CHPL_COMM_ATOMIC_XCHG(int64, FI_INT64, int64_t)
DEFN_CHPL_COMM_ATOMIC_XCHG(uint32, FI_UINT32, uint32_t)
DEFN_CHPL_COMM_ATOMIC_XCHG(uint64, FI_UINT64, uint64_t)
DEFN_CHPL_COMM_ATOMIC_XCHG(real32, FI_FLOAT, _real32)
DEFN_CHPL_COMM_ATOMIC_XCHG(real64, FI_DOUBLE, _real64)


#define DEFN_CHPL_COMM_ATOMIC_CMPXCHG(fnType, ofiType, Type)            \
  void chpl_comm_atomic_cmpxchg_##fnType                                \
         (void* expected, void* desired, c_nodeid_t node, void* object, \
          chpl_bool32* result, memory_order succ, memory_order fail,    \
          int ln, int32_t fn) {                                         \
    DBG_PRINTF(DBG_IFACE_AMO,                                           \
               "%s(%p, %p, %d, %p, %p, %d, %s)", __func__,              \
               expected, desired, (int) node, object, result,           \
               ln, chpl_lookupFilename(fn));                            \
    chpl_comm_diags_verbose_amo("amo cmpxchg", node, ln, fn);           \
    chpl_comm_diags_incr(amo);                                          \
    Type old_value;                                                     \
    Type old_expected;                                                  \
    memcpy(&old_expected, expected, sizeof(Type));                      \
    doAMO(node, object, &old_expected, desired, &old_value,             \
          FI_CSWAP, ofiType, sizeof(Type));                             \
    *result = (chpl_bool32)(old_value == old_expected);                 \
    if (!*result) memcpy(expected, &old_value, sizeof(Type));           \
  }

DEFN_CHPL_COMM_ATOMIC_CMPXCHG(int32, FI_INT32, int32_t)
DEFN_CHPL_COMM_ATOMIC_CMPXCHG(int64, FI_INT64, int64_t)
DEFN_CHPL_COMM_ATOMIC_CMPXCHG(uint32, FI_UINT32, uint32_t)
DEFN_CHPL_COMM_ATOMIC_CMPXCHG(uint64, FI_UINT64, uint64_t)
DEFN_CHPL_COMM_ATOMIC_CMPXCHG(real32, FI_FLOAT, _real32)
DEFN_CHPL_COMM_ATOMIC_CMPXCHG(real64, FI_DOUBLE, _real64)


#define DEFN_IFACE_AMO_SIMPLE_OP(fnOp, ofiOp, fnType, ofiType, Type)    \
  void chpl_comm_atomic_##fnOp##_##fnType                               \
         (void* operand, c_nodeid_t node, void* object,                 \
          memory_order order, int ln, int32_t fn) {                     \
    DBG_PRINTF(DBG_IFACE_AMO,                                           \
               "%s(<%s>, %d, %p, %d, %s)", __func__,                    \
               DBG_VAL(operand, ofiType), (int) node,                   \
               object, ln, chpl_lookupFilename(fn));                    \
    chpl_comm_diags_verbose_amo("amo " #fnOp, node, ln, fn);            \
    chpl_comm_diags_incr(amo);                                          \
    doAMO(node, object, operand, NULL, NULL,                            \
          ofiOp, ofiType, sizeof(Type));                                \
  }                                                                     \
                                                                        \
  void chpl_comm_atomic_##fnOp##_unordered_##fnType                     \
         (void* operand, c_nodeid_t node, void* object,                 \
          int ln, int32_t fn) {                                         \
    DBG_PRINTF(DBG_IFACE_AMO,                                           \
               "%s(<%s>, %d, %p, %d, %s)", __func__,                    \
               DBG_VAL(operand, ofiType), (int) node,                   \
               object, ln, chpl_lookupFilename(fn));                    \
    chpl_comm_diags_verbose_amo("amo unord_" #fnOp, node, ln, fn);      \
    chpl_comm_diags_incr(amo);                                          \
    do_remote_amo_nf_buff(operand, node, object, sizeof(Type),          \
                          ofiOp, ofiType);                              \
  }                                                                     \
                                                                        \
  void chpl_comm_atomic_fetch_##fnOp##_##fnType                         \
         (void* operand, c_nodeid_t node, void* object, void* result,   \
          memory_order order, int ln, int32_t fn) {                     \
    DBG_PRINTF(DBG_IFACE_AMO,                                           \
               "%s(<%s>, %d, %p, %p, %d, %s)", __func__,                \
               DBG_VAL(operand, ofiType), (int) node,                   \
               object, result, ln, chpl_lookupFilename(fn));            \
    chpl_comm_diags_verbose_amo("amo fetch_" #fnOp, node, ln, fn);      \
    chpl_comm_diags_incr(amo);                                          \
    doAMO(node, object, operand, NULL, result,                          \
          ofiOp, ofiType, sizeof(Type));                                \
  }

DEFN_IFACE_AMO_SIMPLE_OP(and, FI_BAND, int32, FI_INT32, int32_t)
DEFN_IFACE_AMO_SIMPLE_OP(and, FI_BAND, int64, FI_INT64, int64_t)
DEFN_IFACE_AMO_SIMPLE_OP(and, FI_BAND, uint32, FI_UINT32, uint32_t)
DEFN_IFACE_AMO_SIMPLE_OP(and, FI_BAND, uint64, FI_UINT64, uint64_t)

DEFN_IFACE_AMO_SIMPLE_OP(or, FI_BOR, int32, FI_INT32, int32_t)
DEFN_IFACE_AMO_SIMPLE_OP(or, FI_BOR, int64, FI_INT64, int64_t)
DEFN_IFACE_AMO_SIMPLE_OP(or, FI_BOR, uint32, FI_UINT32, uint32_t)
DEFN_IFACE_AMO_SIMPLE_OP(or, FI_BOR, uint64, FI_UINT64, uint64_t)

DEFN_IFACE_AMO_SIMPLE_OP(xor, FI_BXOR, int32, FI_INT32, int32_t)
DEFN_IFACE_AMO_SIMPLE_OP(xor, FI_BXOR, int64, FI_INT64, int64_t)
DEFN_IFACE_AMO_SIMPLE_OP(xor, FI_BXOR, uint32, FI_UINT32, uint32_t)
DEFN_IFACE_AMO_SIMPLE_OP(xor, FI_BXOR, uint64, FI_UINT64, uint64_t)

DEFN_IFACE_AMO_SIMPLE_OP(add, FI_SUM, int32, FI_INT32, int32_t)
DEFN_IFACE_AMO_SIMPLE_OP(add, FI_SUM, int64, FI_INT64, int64_t)
DEFN_IFACE_AMO_SIMPLE_OP(add, FI_SUM, uint32, FI_UINT32, uint32_t)
DEFN_IFACE_AMO_SIMPLE_OP(add, FI_SUM, uint64, FI_UINT64, uint64_t)
DEFN_IFACE_AMO_SIMPLE_OP(add, FI_SUM, real32, FI_FLOAT, _real32)
DEFN_IFACE_AMO_SIMPLE_OP(add, FI_SUM, real64, FI_DOUBLE, _real64)


#define DEFN_IFACE_AMO_SUB(fnType, ofiType, Type, negate)               \
  void chpl_comm_atomic_sub_##fnType                                    \
         (void* operand, c_nodeid_t node, void* object,                 \
          memory_order order, int ln, int32_t fn) {                     \
    DBG_PRINTF(DBG_IFACE_AMO,                                           \
               "%s(<%s>, %d, %p, %d, %s)", __func__,                    \
               DBG_VAL(operand, ofiType), (int) node, object,           \
               ln, chpl_lookupFilename(fn));                            \
    Type myOpnd = negate(*(Type*) operand);                             \
    chpl_comm_diags_verbose_amo("amo sub", node, ln, fn);               \
    chpl_comm_diags_incr(amo);                                          \
    doAMO(node, object, &myOpnd, NULL, NULL,                            \
          FI_SUM, ofiType, sizeof(Type));                               \
  }                                                                     \
                                                                        \
  void chpl_comm_atomic_sub_unordered_##fnType                          \
         (void* operand, c_nodeid_t node, void* object,                 \
          int ln, int32_t fn) {                                         \
    DBG_PRINTF(DBG_IFACE_AMO,                                           \
               "%s(<%s>, %d, %p, %d, %s)", __func__,                    \
               DBG_VAL(operand, ofiType), (int) node, object,           \
               ln, chpl_lookupFilename(fn));                            \
    Type myOpnd = negate(*(Type*) operand);                             \
    chpl_comm_diags_verbose_amo("amo unord_sub", node, ln, fn);         \
    chpl_comm_diags_incr(amo);                                          \
    do_remote_amo_nf_buff(&myOpnd, node, object, sizeof(Type),          \
                          FI_SUM, ofiType);                             \
  }                                                                     \
                                                                        \
  void chpl_comm_atomic_fetch_sub_##fnType                              \
         (void* operand, c_nodeid_t node, void* object, void* result,   \
          memory_order order, int ln, int32_t fn) {                     \
    DBG_PRINTF(DBG_IFACE_AMO,                                           \
               "%s(<%s>, %d, %p, %p, %d, %s)", __func__,                \
               DBG_VAL(operand, ofiType), (int) node, object,           \
               result, ln, chpl_lookupFilename(fn));                    \
    Type myOpnd = negate(*(Type*) operand);                             \
    chpl_comm_diags_verbose_amo("amo fetch_sub", node, ln, fn);         \
    chpl_comm_diags_incr(amo);                                          \
    doAMO(node, object, &myOpnd, NULL, result,                          \
          FI_SUM, ofiType, sizeof(Type));                               \
  }

#define NEGATE_I32(x) ((x) == INT32_MIN ? (x) : -(x))
#define NEGATE_I64(x) ((x) == INT64_MIN ? (x) : -(x))
#define NEGATE_U_OR_R(x) (-(x))

DEFN_IFACE_AMO_SUB(int32, FI_INT32, int32_t, NEGATE_I32)
DEFN_IFACE_AMO_SUB(int64, FI_INT64, int64_t, NEGATE_I64)
DEFN_IFACE_AMO_SUB(uint32, FI_UINT32, uint32_t, NEGATE_U_OR_R)
DEFN_IFACE_AMO_SUB(uint64, FI_UINT64, uint64_t, NEGATE_U_OR_R)
DEFN_IFACE_AMO_SUB(real32, FI_FLOAT, _real32, NEGATE_U_OR_R)
DEFN_IFACE_AMO_SUB(real64, FI_DOUBLE, _real64, NEGATE_U_OR_R)

void chpl_comm_atomic_unordered_task_fence(void) {
  DBG_PRINTF(DBG_IFACE_MCM, "%s()", __func__);

  task_local_buff_flush(amo_nf_buff);
}


//
// internal AMO utilities
//

static
int computeAtomicValid(enum fi_datatype ofiType) {
  //
  // At least one provider (ofi_rxm) segfaults if the endpoint given to
  // fi*atomicvalid() entirely lacks atomic caps.  The man page isn't
  // clear on whether this should work, so just avoid that situation.
  //
  if ((ofi_info->tx_attr->caps & FI_ATOMIC) == 0) {
    return 0;
  }

  struct fid_ep* ep = tciTab[0].txCtx; // assume same answer for all endpoints
  size_t count;                        // ignored

#define my_valid(typ, op) \
  (fi_atomicvalid(ep, typ, op, &count) == 0 && count > 0)
#define my_fetch_valid(typ, op) \
  (fi_fetch_atomicvalid(ep, typ, op, &count) == 0 && count > 0)
#define my_compare_valid(typ, op) \
  (fi_compare_atomicvalid(ep, typ, op, &count) == 0 && count > 0)

  // For integral types, all operations matter.
  if (ofiType == FI_INT32
      || ofiType == FI_UINT32
      || ofiType == FI_INT64
      || ofiType == FI_UINT64) {
    return (   my_valid(ofiType, FI_SUM)
            && my_valid(ofiType, FI_BOR)
            && my_valid(ofiType, FI_BAND)
            && my_valid(ofiType, FI_BXOR)
            && my_valid(ofiType, FI_ATOMIC_WRITE)
            && my_fetch_valid(ofiType, FI_SUM)
            && my_fetch_valid(ofiType, FI_BOR)
            && my_fetch_valid(ofiType, FI_BAND)
            && my_fetch_valid(ofiType, FI_BXOR)
            && my_fetch_valid(ofiType, FI_ATOMIC_READ)
            && my_fetch_valid(ofiType, FI_ATOMIC_WRITE)
            && my_compare_valid(ofiType, FI_CSWAP));
  }

  //
  // For real types, only sum, read, write, and cswap matter.
  //
  return (   my_valid(ofiType, FI_SUM)
          && my_valid(ofiType, FI_ATOMIC_WRITE)
          && my_fetch_valid(ofiType, FI_SUM)
          && my_fetch_valid(ofiType, FI_ATOMIC_READ)
          && my_fetch_valid(ofiType, FI_ATOMIC_WRITE)
          && my_compare_valid(ofiType, FI_CSWAP));

#undef my_valid
#undef my_fetch_valid
#undef my_compare_valid
}


static
int isAtomicValid(enum fi_datatype ofiType) {
  static chpl_bool inited = false;
  static int validByType[FI_DATATYPE_LAST];

  if (!inited) {
    validByType[FI_INT32]  = computeAtomicValid(FI_INT32);
    validByType[FI_UINT32] = computeAtomicValid(FI_UINT32);
    validByType[FI_INT64]  = computeAtomicValid(FI_INT64);
    validByType[FI_UINT64] = computeAtomicValid(FI_UINT64);
    validByType[FI_FLOAT]  = computeAtomicValid(FI_FLOAT);
    validByType[FI_DOUBLE] = computeAtomicValid(FI_DOUBLE);
    inited = true;
  }

  return validByType[ofiType];
}


static inline
void doAMO(c_nodeid_t node, void* object,
           const void* operand1, const void* operand2, void* result,
           int ofiOp, enum fi_datatype ofiType, size_t size) {
  if (chpl_numNodes <= 1) {
    doCpuAMO(object, operand1, operand2, result, ofiOp, ofiType, size);
    return;
  }

  retireDelayedAmDone(false /*taskIsEnding*/);

  uint64_t mrKey;
  uint64_t mrRaddr;
  if (!isAtomicValid(ofiType)
      || mrGetKey(&mrKey, &mrRaddr, node, object, size) != 0) {
    //
    // We can't do the AMO on the network, so do it on the CPU.  If the
    // object is on this node do it directly; otherwise, use an AM.
    //
    if (node == chpl_nodeID) {
      if (ofiOp != FI_ATOMIC_READ) {
        waitForPutsVisAllNodes(NULL, NULL, false /*taskIsEnding*/);
      }
      doCpuAMO(object, operand1, operand2, result, ofiOp, ofiType, size);
    } else {
      amRequestAMO(node, object, operand1, operand2, result,
                   ofiOp, ofiType, size);
    }
  } else {
    //
    // The type is supported for network atomics and the object address
    // is remotely accessible.  Do the AMO natively.
    //
    ofi_amo(node, mrRaddr, mrKey, operand1, operand2, result,
            ofiOp, ofiType, size);
  }
}


static inline
void doCpuAMO(void* obj,
              const void* operand1, const void* operand2, void* result,
              enum fi_op ofiOp, enum fi_datatype ofiType, size_t size) {
  CHK_TRUE(size == 4 || size == 8);

  chpl_amo_datum_t* myOpnd1 = (chpl_amo_datum_t*) operand1;
  chpl_amo_datum_t* myOpnd2 = (chpl_amo_datum_t*) operand2;

#define CPU_INT_ARITH_AMO(_o, _t, _m)                                   \
  do {                                                                  \
    if (result == NULL) {                                               \
      (void) atomic_fetch_##_o##_##_t((atomic_##_t*) obj,               \
                                      myOpnd1->_m);                     \
    } else {                                                            \
      *(_t*) result = atomic_fetch_##_o##_##_t((atomic_##_t*) obj,      \
                                               myOpnd1->_m);            \
    }                                                                   \
  } while (0)

  //
  // Here we implement AMOs which the NIC cannot or should not do.
  //
  switch (ofiOp) {
  case FI_ATOMIC_WRITE:
    if (result == NULL) {
      //
      // write
      //
      if (size == 4) {
        atomic_store_uint_least32_t(obj, myOpnd1->u32);
      } else {
        atomic_store_uint_least64_t(obj, myOpnd1->u64);
      }
    } else {
      //
      // exchange
      //
      if (size == 4) {
        *(uint32_t*) result = atomic_exchange_uint_least32_t(obj,
                                                             myOpnd1->u32);
      } else {
        *(uint64_t*) result = atomic_exchange_uint_least64_t(obj,
                                                             myOpnd1->u64);
      }
    }
    break;

  case FI_ATOMIC_READ:
    if (size == 4) {
      *(uint32_t*) result = atomic_load_uint_least32_t(obj);
    } else {
      *(uint64_t*) result = atomic_load_uint_least64_t(obj);
    }
    break;

  case FI_CSWAP:
    if (size == 4) {
      uint32_t myOpnd1Val = myOpnd1->u32;
      (void) atomic_compare_exchange_strong_uint_least32_t(obj,
                                                           &myOpnd1Val,
                                                           myOpnd2->u32);
      *(uint32_t*) result = myOpnd1Val;
    } else {
      uint64_t myOpnd1Val = myOpnd1->u64;
      (void) atomic_compare_exchange_strong_uint_least64_t(obj,
                                                           &myOpnd1Val,
                                                           myOpnd2->u64);
      *(uint64_t*) result = myOpnd1Val;
    }
    break;

  case FI_BAND:
    if (ofiType == FI_INT32) {
      CPU_INT_ARITH_AMO(and, int_least32_t, i32);
    } else if (ofiType == FI_UINT32) {
      CPU_INT_ARITH_AMO(and, uint_least32_t, u32);
    } else if (ofiType == FI_INT64) {
      CPU_INT_ARITH_AMO(and, int_least64_t, i64);
    } else if (ofiType == FI_UINT64) {
      CPU_INT_ARITH_AMO(and, uint_least64_t, u64);
    } else {
      INTERNAL_ERROR_V("doCpuAMO(): unsupported ofiOp %d, ofiType %d",
                       ofiOp, ofiType);
    }
    break;

  case FI_BOR:
    if (ofiType == FI_INT32) {
      CPU_INT_ARITH_AMO(or, int_least32_t, i32);
    } else if (ofiType == FI_UINT32) {
      CPU_INT_ARITH_AMO(or, uint_least32_t, u32);
    } else if (ofiType == FI_INT64) {
      CPU_INT_ARITH_AMO(or, int_least64_t, i64);
    } else if (ofiType == FI_UINT64) {
      CPU_INT_ARITH_AMO(or, uint_least64_t, u64);
    } else {
      INTERNAL_ERROR_V("doCpuAMO(): unsupported ofiOp %d, ofiType %d",
                       ofiOp, ofiType);
    }
    break;

  case FI_BXOR:
    if (ofiType == FI_INT32) {
      CPU_INT_ARITH_AMO(xor, int_least32_t, i32);
    } else if (ofiType == FI_UINT32) {
      CPU_INT_ARITH_AMO(xor, uint_least32_t, u32);
    } else if (ofiType == FI_INT64) {
      CPU_INT_ARITH_AMO(xor, int_least64_t, i64);
    } else if (ofiType == FI_UINT64) {
      CPU_INT_ARITH_AMO(xor, uint_least64_t, u64);
    } else {
      INTERNAL_ERROR_V("doCpuAMO(): unsupported ofiOp %d, ofiType %d",
                       ofiOp, ofiType);
    }
    break;

  case FI_SUM:
    if (ofiType == FI_INT32) {
      CPU_INT_ARITH_AMO(add, int_least32_t, i32);
    } else if (ofiType == FI_UINT32) {
      CPU_INT_ARITH_AMO(add, uint_least32_t, u32);
    } else if (ofiType == FI_INT64) {
      CPU_INT_ARITH_AMO(add, int_least64_t, i64);
    } else if (ofiType == FI_UINT64) {
      CPU_INT_ARITH_AMO(add, uint_least64_t, u64);
    } else if (ofiType == FI_FLOAT) {
      CPU_INT_ARITH_AMO(add, _real32, r32);
    } else if (ofiType == FI_DOUBLE) {
      CPU_INT_ARITH_AMO(add, _real64, r64);
    } else {
      INTERNAL_ERROR_V("doCpuAMO(): unsupported ofiOp %d, ofiType %d",
                       ofiOp, ofiType);
    }
    break;


  default:
    INTERNAL_ERROR_V("doCpuAMO(): unsupported ofiOp %d, ofiType %d",
                     ofiOp, ofiType);
  }

  if (DBG_TEST_MASK(DBG_AMO | DBG_AMO_READ)) {
    if (result == NULL) {
      DBG_PRINTF(DBG_AMO,
                 "doCpuAMO(%p, %s, %s, %s): now %s",
                 obj, amo_opName(ofiOp), amo_typeName(ofiType),
                 DBG_VAL(myOpnd1, ofiType),
                 DBG_VAL((chpl_amo_datum_t*) obj, ofiType));
    } else if (ofiOp == FI_ATOMIC_READ) {
      DBG_PRINTF(DBG_AMO_READ,
                 "doCpuAMO(%p, %s, %s): res %p is %s",
                 obj, amo_opName(ofiOp), amo_typeName(ofiType), result,
                 DBG_VAL(result, ofiType));
    } else {
      DBG_PRINTF(DBG_AMO,
                 "doCpuAMO(%p, %s, %s, %s, %s): now %s, res %p is %s",
                 obj, amo_opName(ofiOp), amo_typeName(ofiType),
                 DBG_VAL(myOpnd1, ofiType),
                 DBG_VAL(myOpnd2, ofiType),
                 DBG_VAL((chpl_amo_datum_t*) obj, ofiType), result,
                 DBG_VAL(result, (ofiOp == FI_CSWAP) ? FI_INT32 : ofiType));
    }
  }

#undef CPU_INT_ARITH_AMO
}


/*
 *** START OF NON-FETCHING BUFFERED ATOMIC OPERATIONS ***
 *
 * Support for non-fetching buffered atomic operations. We internally buffer
 * atomic operations and then initiate them all at once for increased
 * transaction rate.
 */

// Flush buffered AMOs for the specified task info and reset the counter.
static inline
void amo_nf_buff_task_info_flush(amo_nf_buff_task_info_t* info) {
  if (info->vi > 0) {
    DBG_PRINTF(DBG_AMO_UNORD,
               "amo_nf_buff_task_info_flush(): info has %d entries",
               info->vi);
    ofi_amo_nf_V(info->vi, info->opnd1_v, info->local_mr,
                 info->locale_v, info->object_v, info->remote_mr_v,
                 info->size_v, info->cmd_v, info->type_v);
    info->vi = 0;
  }
}


static inline
void do_remote_amo_nf_buff(void* opnd1, c_nodeid_t node,
                           void* object, size_t size,
                           enum fi_op ofiOp, enum fi_datatype ofiType) {
  //
  // "Unordered" is possible only for actual network atomic ops.
  //
  if (chpl_numNodes <= 1) {
    doCpuAMO(object, opnd1, NULL, NULL, ofiOp, ofiType, size);
    return;
  }

  retireDelayedAmDone(false /*taskIsEnding*/);

  uint64_t mrKey;
  uint64_t mrRaddr;
  if (!isAtomicValid(ofiType)
      || mrGetKey(&mrKey, &mrRaddr, node, object, size) != 0) {
    if (node == chpl_nodeID) {
      doCpuAMO(object, opnd1, NULL, NULL, ofiOp, ofiType, size);
    } else {
      amRequestAMO(node, object, opnd1, NULL, NULL,
                   ofiOp, ofiType, size);
    }
    return;
  }

  amo_nf_buff_task_info_t* info = task_local_buff_acquire(amo_nf_buff, 0);
  if (info == NULL) {
    ofi_amo(node, mrRaddr, mrKey, opnd1, NULL, NULL, ofiOp, ofiType, size);
    return;
  }

  if (info->new) {
    //
    // The AMO operands themselves are stored in a vector in the info,
    // so we only need one local memory descriptor for that vector.
    //
    CHK_TRUE(mrGetDesc(&info->local_mr, info->opnd1_v, size) == 0);
    info->new = false;
  }

  int vi = info->vi;
  info->opnd1_v[vi]     = size == 4 ? *(uint32_t*) opnd1:
                                      *(uint64_t*) opnd1;
  info->locale_v[vi]    = node;
  info->object_v[vi]    = object;
  info->size_v[vi]      = size;
  info->cmd_v[vi]       = ofiOp;
  info->type_v[vi]      = ofiType;
  info->remote_mr_v[vi] = mrKey;
  info->vi++;

  DBG_PRINTF(DBG_AMO_UNORD,
             "do_remote_amo_nf_buff(): info[%d] = "
             "{%p, %d, %p, %zd, %d, %d, %" PRIx64 ", %p}",
             vi, &info->opnd1_v[vi], (int) node, object, size,
             (int) ofiOp, (int) ofiType, mrKey, info->local_mr);

  // flush if buffers are full
  if (info->vi == MAX_CHAINED_AMO_NF_LEN) {
    amo_nf_buff_task_info_flush(info);
  }
}
/*** END OF NON-FETCHING BUFFERED ATOMIC OPERATIONS ***/


////////////////////////////////////////
//
// Interface: utility
//

int chpl_comm_addr_gettable(c_nodeid_t node, void* start, size_t size) {
  // No way to know if the page is mapped on the remote (without a round trip)
  return 0;
}


int32_t chpl_comm_getMaxThreads(void) {
  // no limit
  return 0;
}


////////////////////////////////////////
//
// Interface: barriers
//

//
// We do a simple tree-based split-phase barrier, with locale 0 as the
// root of the tree.  Each of the locales has a barrier_info_t struct,
// and knows the address of that struct in its child locales (locales
// num_children*my_idx+1 - num_children*my_idx+num_children) and its
// parent (locale (my_idx-1)/num_children).  Notify and release flags on
// all locales start out 0.  The notify step consists of each locale
// waiting for its children, if it has any, to set the child_notify
// flags in its own barrier info struct to 1, and then if it is not
// locale 0, setting the child_notify flag corresponding to itself in
// its parent's barrier info struct to 1.  Thus notification propagates
// up from the leaves of the tree to the root.  In the wait phase each
// locale except locale 0 waits for the parent_release flag in its own
// barrier info struct to become 1.  Once a locale sees that, it clears
// all of the flags in its own struct and then sets the parent_release
// flags in both of its existing children to 1.  Thus releases propagate
// down from locale 0 to the leaves.  Once waiting is complete at the
// leaves, all of the flags throughout the job are back to 0 and the
// process can repeat.
//
// Note that we can (and do) do other things while waiting for notify
// and release flags to be set.  In fact we have to task-yield while
// doing so, in case the PUTs need to be done via AM for some reason
// (unregistered memory, e.g.).
//
// TODO: vectorize the child PUTs.
//
#define BAR_TREE_NUM_CHILDREN 64

typedef struct {
  volatile int child_notify[BAR_TREE_NUM_CHILDREN];
  volatile int parent_release;
}  bar_info_t;

static c_nodeid_t bar_childFirst;
static c_nodeid_t bar_numChildren;
static c_nodeid_t bar_parent;

static bar_info_t bar_info;
static bar_info_t** bar_infoMap;


static
void init_bar(void) {
  bar_childFirst = BAR_TREE_NUM_CHILDREN * chpl_nodeID + 1;
  if (bar_childFirst >= chpl_numNodes)
    bar_numChildren = 0;
  else {
    bar_numChildren = BAR_TREE_NUM_CHILDREN;
    if (bar_childFirst + bar_numChildren >= chpl_numNodes)
      bar_numChildren = chpl_numNodes - bar_childFirst;
  }
  bar_parent = (chpl_nodeID - 1) / BAR_TREE_NUM_CHILDREN;

  CHPL_CALLOC(bar_infoMap, chpl_numNodes);
  const bar_info_t* p = &bar_info;
  chpl_comm_ofi_oob_allgather(&p, bar_infoMap, sizeof(p));
}


void chpl_comm_barrier(const char *msg) {
  DBG_PRINTF(DBG_IFACE_SETUP, "%s('%s')", __func__, msg);

#ifdef CHPL_COMM_DEBUG
  chpl_msg(2, "%d: enter barrier for '%s'\n", chpl_nodeID, msg);
#endif

  if (chpl_numNodes == 1) {
    return;
  }

  DBG_PRINTF(DBG_BARRIER, "barrier '%s'", (msg == NULL) ? "" : msg);

  if (pthread_equal(pthread_self(), pthread_that_inited)
      || numAmHandlersActive == 0) {
    //
    // Either this is the main (chpl_comm_init()ing) thread or
    // comm layer setup is not complete yet.  Use OOB barrier.
    //
    chpl_comm_ofi_oob_barrier();
    DBG_PRINTF(DBG_BARRIER, "barrier '%s' done via out-of-band",
               (msg == NULL) ? "" : msg);
    return;
  }

  //
  // Ensure our outstanding nonfetching AMOs and PUTs are visible.
  // (Visibility of operations done by other tasks on this node is
  // the caller's responsibility.)
  //
  retireDelayedAmDone(false /*taskIsEnding*/);
  waitForPutsVisAllNodes(NULL, NULL, false /*taskIsEnding*/);

  //
  // Wait for our child locales to notify us that they have reached the
  // barrier.
  //
  DBG_PRINTF(DBG_BARRIER, "BAR wait for %d children", (int) bar_numChildren);
  for (uint32_t i = 0; i < bar_numChildren; i++) {
    while (bar_info.child_notify[i] == 0) {
      local_yield();
    }
  }

  const int one = 1;

  if (chpl_nodeID != 0) {
    //
    // Notify our parent locale that we have reached the barrier.
    //
    c_nodeid_t parChild = (chpl_nodeID - 1) % BAR_TREE_NUM_CHILDREN;

    DBG_PRINTF(DBG_BARRIER, "BAR notify parent %d", (int) bar_parent);
    ofi_put(&one, bar_parent,
            (void*) &bar_infoMap[bar_parent]->child_notify[parChild],
            sizeof(one));

    //
    // Wait for our parent locale to release us from the barrier.
    //
    DBG_PRINTF(DBG_BARRIER, "BAR wait for parental release");
    while (bar_info.parent_release == 0) {
      local_yield();
    }
  }

  //
  // Clear all our barrier flags.
  //
  for (int i = 0; i < bar_numChildren; i++)
    bar_info.child_notify[i] = 0;
  bar_info.parent_release = 0;

  //
  // Release our children.
  //
  if (bar_numChildren > 0) {
    for (int i = 0; i < bar_numChildren; i++) {
      c_nodeid_t child = bar_childFirst + i;
      DBG_PRINTF(DBG_BARRIER, "BAR release child %d", (int) child);
      ofi_put(&one, child,
              (void*) &bar_infoMap[child]->parent_release,
              sizeof(one));
    }
  }

  DBG_PRINTF(DBG_BARRIER, "barrier '%s' done via PUTs",
             (msg == NULL) ? "" : msg);
}


////////////////////////////////////////
//
// Time
//

static double timeBase;


static
void time_init(void) {
  timeBase = chpl_comm_ofi_time_get();
}


double chpl_comm_ofi_time_get(void) {
  struct timespec _ts;
  (void) clock_gettime(CLOCK_MONOTONIC, &_ts);
  return ((double) _ts.tv_sec + (double) _ts.tv_nsec * 1e-9) - timeBase;
}


////////////////////////////////////////
//
// Error reporting
//

//
// Here we just handle a few special cases where we think we can be
// more informative than usual.  If we return, the usual internal
// error message will be printed.
//
static
void ofiErrReport(const char* exprStr, int retVal, const char* errStr) {
  if (retVal == -FI_EMFILE) {
    //
    // We've run into the limit on the number of files we can have open
    // at once.
    //
    // Some providers open a lot of files.  The tcp provider, as one
    // example, can open as many as roughly 9 files per node, plus 2
    // socket files for each connected endpoint.  Because of this, one
    // can exceed a quite reasonable open-file limit in a job running on
    // a fairly modest number of many-core locales.  Thus for example,
    // extremeBlock will get -FI_EMFILE with an open file limit of 1024
    // when run on 32 24-core locales.  Here, try to inform the user
    // about this without getting overly technical.
    //
    INTERNAL_ERROR_V(
      "OFI error: %s: %s:\n"
      "  The program has reached the limit on the number of files it can\n"
      "  have open at once.  This may be because the product of the number\n"
      "  of locales (%d) and the communication concurrency (roughly %d) is\n"
      "  a significant fraction of the open-file limit (%ld).  If so,\n"
      "  either setting CHPL_RT_COMM_CONCURRENCY to decrease communication\n"
      "  concurrency or running on fewer locales may allow the program to\n"
      "  execute successfully.  Or, you may be able to use `ulimit` to\n"
      "  increase the open file limit and achieve the same result.",
      exprStr, errStr, (int) chpl_numNodes, numTxCtxs, sysconf(_SC_OPEN_MAX));
  }
}


#ifdef CHPL_COMM_DEBUG

////////////////////////////////////////
//
// Debugging support
//

void chpl_comm_ofi_dbg_init(void) {
  const char* ev;
  if ((ev = chpl_env_rt_get("COMM_OFI_DEBUG", NULL)) == NULL) {
    return;
  }

  //
  // Compute the debug level from the keywords in the env var.
  //
  {
    #define OFIDBG_MACRO(_enum, _desc) { #_enum, _desc },
    static struct {
      const char* kw;
      const char* desc;
    } dbgCodes[] = { OFI_ALL_DEBUGS(OFIDBG_MACRO) };
    static const int dbgCodesLen = sizeof(dbgCodes) / sizeof(dbgCodes[0]);

    char evc[strlen(ev) + 1];  // non-constant, for strtok()
    strcpy(evc, ev);

    //
    // Loop over comma-separated tokens in the env var.
    //
    chpl_comm_ofi_dbg_level = 0;
    char* tok;
    char* strSave;
    for (char* evcPtr = evc;
         (tok = strtok_r(evcPtr, ",", &strSave)) != NULL;
         evcPtr = NULL) {
      //
      // Users can use lowercase and dashes; table contains uppercase
      // and underbars, because it defines C symbols.  Canonicalize
      // user's token.
      //
      char ctok[strlen(tok)];
      for (int i = 0; i < sizeof(ctok); i++) {
        if (tok[i] == '-') {
          ctok[i] = '_';
        } else {
          ctok[i] = toupper(tok[i]);
        }
      }

      //
      // Find user's keyword in table.
      //
      int i, iPrefix;
      for (i = 0, iPrefix = -1; i < dbgCodesLen; i++) {
        if (strncmp(ctok, dbgCodes[i].kw, sizeof(ctok)) == 0) {
          if (dbgCodes[i].kw[sizeof(ctok)] == '\0') {
            break;
          } else {
            // Accept a match to the prefix of a keyword iff unambiguous.
            iPrefix = (iPrefix < 0) ? i : dbgCodesLen;
          }
        }
      }

      //
      // Add found debug bit to our set of same, or say "what?".
      //
      if (i >= 0 && i < dbgCodesLen) {
        chpl_comm_ofi_dbg_level |= (uint64_t) 1 << i;
      } else if (iPrefix >= 0 && iPrefix < dbgCodesLen) {
        chpl_comm_ofi_dbg_level |= (uint64_t) 1 << iPrefix;
      } else {
        //
        // All nodes exit on error, but only node 0 says why.
        //
        if (chpl_nodeID == 0) {
          if (ctok[0] != '?' && strncmp(ctok, "HELP", sizeof(ctok)) != 0) {
            printf("Warning: unknown or ambiguous comm=ofi debug keyword "
                   "\"%s\"\n",
                   tok);
          }

          //
          // Print pretty table of debug keywords and descriptions.
          //
          printf("Debug keywords (case ignored, -_ equiv) and descriptions\n");
          printf("--------------------------------------------------------\n");

          int kwLenMax = 0;
          for (int ic = 0; ic < dbgCodesLen; ic++) {
            if (strlen(dbgCodes[ic].kw) > kwLenMax) {
              kwLenMax = strlen(dbgCodes[ic].kw);
            }
          }

          for (int ic = 0; ic < dbgCodesLen; ic++) {
            char kw[strlen(dbgCodes[ic].kw)];
            for (int ik = 0; ik < sizeof(kw); ik++) {
              if (dbgCodes[ic].kw[ik] == '_') {
                kw[ik] = '-';
              } else {
                kw[ik] = tolower(dbgCodes[ic].kw[ik]);
              }
            }
            printf("%.*s:%*s %s\n",
                   (int) sizeof(kw), kw, (int) (kwLenMax - sizeof(kw)), "",
                   dbgCodes[ic].desc);
          }
        }

        chpl_comm_ofi_oob_fini();
        chpl_exit_any(0);
      }
    }
  }

  if ((ev = chpl_env_rt_get("COMM_OFI_DEBUG_FNAME", NULL)) == NULL) {
    chpl_comm_ofi_dbg_file = stdout;
  } else {
    char fname[strlen(ev) + 6 + 1];
    int fnameLen;
    fnameLen = snprintf(fname, sizeof(fname), "%s.%d", ev, (int) chpl_nodeID);
    CHK_TRUE(fnameLen < sizeof(fname));
    CHK_TRUE((chpl_comm_ofi_dbg_file = fopen(fname, "w")) != NULL);
    setbuf(chpl_comm_ofi_dbg_file, NULL);
  }
}


char* chpl_comm_ofi_dbg_prefix(void) {
  static __thread char buf[30];
  int len;

  if (buf[0] == '\0' || DBG_TEST_MASK(DBG_TSTAMP)) {
    buf[len = 0] = '\0';
    if (chpl_nodeID >= 0)
      len += snprintf(&buf[len], sizeof(buf) - len, "%d", chpl_nodeID);
    if (chpl_task_getId() == chpl_nullTaskID)
      len += snprintf(&buf[len], sizeof(buf) - len, ":_");
    else
      len += snprintf(&buf[len], sizeof(buf) - len, ":%ld",
                      (long int) chpl_task_getId());
    if (DBG_TEST_MASK(DBG_TSTAMP))
      len += snprintf(&buf[len], sizeof(buf) - len, "%s%.9f",
                      ((len == 0) ? "" : ": "), chpl_comm_ofi_time_get());
    if (len > 0)
      len += snprintf(&buf[len], sizeof(buf) - len, ": ");
  }
  return buf;
}


char* chpl_comm_ofi_dbg_val(const void* pV, enum fi_datatype ofiType) {
  static __thread char buf[5][32];
  static __thread int iBuf = 0;
  char* s = buf[iBuf];

  if (pV == NULL) {
    snprintf(s, sizeof(buf[0]), "NIL");
  } else {
    switch (ofiType) {
    case FI_INT32:
      snprintf(s, sizeof(buf[0]), "%" PRId32, *(const int_least32_t*) pV);
      break;
    case FI_UINT32:
      snprintf(s, sizeof(buf[0]), "%#" PRIx32, *(const uint_least32_t*) pV);
      break;
    case FI_INT64:
      snprintf(s, sizeof(buf[0]), "%" PRId64, *(const int_least64_t*) pV);
      break;
    case FI_FLOAT:
      snprintf(s, sizeof(buf[0]), "%.6g", (double) *(const float*) pV);
      break;
    case FI_DOUBLE:
      snprintf(s, sizeof(buf[0]), "%.16g", *(const double*) pV);
      break;
    default:
      snprintf(s, sizeof(buf[0]), "%#" PRIx64, *(const uint_least64_t*) pV);
      break;
    }
  }

  if (++iBuf >= sizeof(buf) / sizeof(buf[0]))
    iBuf = 0;

  return s;
}


static
const char* am_opName(amOp_t op) {
  switch (op) {
  case am_opExecOn: return "opExecOn";
  case am_opExecOnLrg: return "opExecOnLrg";
  case am_opGet: return "opGet";
  case am_opPut: return "opPut";
  case am_opAMO: return "opAMO";
  case am_opFree: return "opFree";
  case am_opNop: return "opNop";
  case am_opShutdown: return "opShutdown";
  default: return "op???";
  }
}


static
const char* amo_opName(enum fi_op ofiOp) {
  switch (ofiOp) {
  case FI_ATOMIC_WRITE: return "write";
  case FI_ATOMIC_READ: return "read";
  case FI_CSWAP: return "cswap";
  case FI_BAND: return "band";
  case FI_BOR: return "bor";
  case FI_BXOR: return "bxor";
  case FI_SUM: return "sum";
  default: return "amoOp???";
  }
}


static
const char* amo_typeName(enum fi_datatype ofiType) {
  switch (ofiType) {
  case FI_INT32: return "int32";
  case FI_UINT32: return "uint32";
  case FI_INT64: return "int64";
  case FI_UINT64: return "uint64";
  case FI_FLOAT: return "real32";
  case FI_DOUBLE: return "real64";
  default: return "amoType???";
  }
}


static
const char* am_seqIdStr(amRequest_t* req) {
  static __thread char buf[30];
  if (op_uses_on_bundle(req->b.op)) {
    (void) snprintf(buf, sizeof(buf), "%d:%" PRIu64,
                   req->xo.hdr.comm.node, req->xo.hdr.comm.seq);
  } else {
    (void) snprintf(buf, sizeof(buf), "%d:%" PRIu64,
                   req->b.node, req->b.seq);
  }
  return buf;
}


static
const char* am_reqStr(c_nodeid_t tgtNode, amRequest_t* req, size_t reqSize) {
  static __thread char buf[1000];
  int len;

  len = snprintf(buf, sizeof(buf), "seqId %s, %s, sz %zd",
                 am_seqIdStr(req), am_opName(req->b.op), reqSize);

  switch (req->b.op) {
  case am_opExecOn:
    len += snprintf(buf + len, sizeof(buf) - len, ", fid %d(arg %p, sz %zd)%s",
                    req->xo.hdr.comm.fid, &req->xo.hdr.payload,
                    reqSize - offsetof(chpl_comm_on_bundle_t, payload),
                    req->xo.hdr.comm.fast ? ", fast" : "");
    break;

  case am_opExecOnLrg:
    len += snprintf(buf + len, sizeof(buf) - len, ", fid %d(arg %p, sz %zd)",
                    req->xol.hdr.comm.fid, req->xol.pPayload,
                    req->xol.hdr.comm.argSize);
    break;

  case am_opGet:
    len += snprintf(buf + len, sizeof(buf) - len, ", %d:%p <- %d:%p, sz %zd",
                    (int) tgtNode, req->rma.addr,
                    req->rma.b.node, req->rma.raddr,
                    req->rma.size);
    break;
  case am_opPut:
    len += snprintf(buf + len, sizeof(buf) - len, ", %d:%p -> %d:%p, sz %zd",
                    (int) tgtNode, req->rma.addr,
                    req->rma.b.node, req->rma.raddr, req->rma.size);
    break;

  case am_opAMO:
    if (req->amo.ofiOp == FI_CSWAP) {
      len += snprintf(buf + len, sizeof(buf) - len,
                      ", obj %p, opnd1 %s, opnd2 %s, res %p"
                      ", ofiOp %s, ofiType %s, sz %d",
                      req->amo.obj,
                      DBG_VAL(&req->amo.operand1, req->amo.ofiType),
                      DBG_VAL(&req->amo.operand2, req->amo.ofiType),
                      req->amo.result, amo_opName(req->amo.ofiOp),
                      amo_typeName(req->amo.ofiType), req->amo.size);
    } else if (req->amo.result != NULL) {
      if (req->amo.ofiOp == FI_ATOMIC_READ) {
        len += snprintf(buf + len, sizeof(buf) - len,
                        ", obj %p, res %p"
                        ", ofiOp %s, ofiType %s, sz %d",
                        req->amo.obj,
                        req->amo.result, amo_opName(req->amo.ofiOp),
                        amo_typeName(req->amo.ofiType), req->amo.size);
      } else {
        len += snprintf(buf + len, sizeof(buf) - len,
                        ", obj %p, opnd %s, res %p"
                        ", ofiOp %s, ofiType %s, sz %d",
                        req->amo.obj,
                        DBG_VAL(&req->amo.operand1, req->amo.ofiType),
                        req->amo.result, amo_opName(req->amo.ofiOp),
                        amo_typeName(req->amo.ofiType), req->amo.size);
      }
    } else {
      len += snprintf(buf + len, sizeof(buf) - len,
                      ", obj %p, opnd %s"
                      ", ofiOp %s, ofiType %s, sz %d",
                      req->amo.obj,
                      DBG_VAL(&req->amo.operand1, req->amo.ofiType),
                      amo_opName(req->amo.ofiOp),
                      amo_typeName(req->amo.ofiType), req->amo.size);
    }
    break;

  case am_opFree:
    len += snprintf(buf + len, sizeof(buf) - len, ", %p",
                    req->free.p);
    break;

  default:
    break;
  }

  amDone_t* pAmDone = op_uses_on_bundle(req->b.op)
                      ? req->xo.hdr.comm.pAmDone
                      : req->b.pAmDone;
  if (pAmDone == NULL) {
    (void) snprintf(buf + len, sizeof(buf) - len, ", NB");
  } else {
    (void) snprintf(buf + len, sizeof(buf) - len, ", pAmDone %p", pAmDone);
  }

  return buf;
}


static
const char* am_reqDoneStr(amRequest_t* req) {
  static __thread char buf[100];
  amDone_t* pAmDone = op_uses_on_bundle(req->b.op)
                      ? req->xo.hdr.comm.pAmDone
                      : req->b.pAmDone;
  if (pAmDone == NULL) {
    (void) snprintf(buf, sizeof(buf),
                    "fini AM seqId %s, NB",
                    am_seqIdStr(req));
  } else {
    (void) snprintf(buf, sizeof(buf),
                    "fini AM seqId %s, set pAmDone %p",
                    am_seqIdStr(req), pAmDone);
  }
  return buf;
}

#endif
