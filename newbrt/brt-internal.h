#ifndef BRT_INTERNAL_H
#define BRT_INTERNAL_H

#ident "Copyright (c) 2007, 2008 Tokutek Inc.  All rights reserved."

#include "cachetable.h"
#include "fifo.h"
#include "pma.h"
#include "brt.h"
#include "crc.h"
#include "list.h"

#ifndef BRT_FANOUT
#define BRT_FANOUT 16
#endif
enum { TREE_FANOUT = BRT_FANOUT };
enum { KEY_VALUE_OVERHEAD = 8 }; /* Must store the two lengths. */
enum { PMA_ITEM_OVERHEAD = 4 };
enum { BRT_CMD_OVERHEAD = 1 };
enum { BRT_DEFAULT_NODE_SIZE = 1 << 20 };

struct nodeheader_in_file {
    int n_in_buffer;
};
enum { BUFFER_HEADER_SIZE = (4 // height//
			     + 4 // n_children
			     + TREE_FANOUT * 8 // children
			     ) };

struct brtnode_nonleaf_pivotinfo {
    struct kv_pair *pivotkey; /* For DUPSORT keys, the keys are whole key-value pairs.
			       * For nonduplicate and DUPSORT keys we have
			       *  Child 0's keys <= pivotkey[0] < Child 1's keys <= pivotkey[1] < ...  pivotkey[N-1] < child N's keys <= pivotkey[N] ...
			       */
};
struct brtnode_nonleaf_childinfo {
    u_int32_t    subtree_fingerprint;
#if 0
    DISKOFF      diskoff;
    FIFO         htable;
    unsigned int n_bytes_in_buffer; /* How many bytes are in each buffer (including overheads for the disk-representation) */
#endif
};

typedef struct brtnode *BRTNODE;
/* Internal nodes. */
struct brtnode {
    enum typ_tag tag;
    unsigned int nodesize;
    unsigned int flags;
    DISKOFF thisnodename;   // The size of the node allocated on disk.  Not all is necessarily in use.
    //  These two LSNs are used to decide when to make a copy of a node instead of overwriting it.
    //  In the TOKULOGGER is a field called checkpoint_lsn which is the lsn of the most recent checkpoint
    LSN     disk_lsn;       // The LSN as of the most recent version on disk.  (Updated by brt-serialize)  This lsn is saved in the node.
    LSN     log_lsn;        // The LSN of the youngest log entry that affects the current in-memory state.   The log write may not have actually made it to disk.  This lsn is not saved in disk (since the two lsns are the same for any node not in main memory.)
    //  The checkpointing works as follows:
    //      When we unpin a node: if it is dirty and disk_lsn<checkpoint_lsn then we need to make a new copy.
    //      When we checkpoint:  Create a checkpoint record, and cause every dirty node to be written to disk.  The new checkpoint record is *not* incorporated into the disk_lsn of the written nodes.
    //      While we are checkpointing, someone may modify a dirty node that has not yet been written.   In that case, when we unpin the node, we make the new copy (because the disk_lsn<checkpoint_lsn), just as we would usually.
    //
    int     layout_version; // What version of the data structure?
    int    height; /* height is always >= 0.  0 for leaf, >0 for nonleaf. */
    u_int32_t rand4fingerprint;
    u_int32_t local_fingerprint; /* For leaves this is everything in the buffer.  For nonleaves, this is everything in the buffers, but does not include child subtree fingerprints. */
    int    dirty;
    union node {
	struct nonleaf {
	    // Don't actually store the subree fingerprint in the in-memory data structure.
	    int             n_children;  /* if n_children==TREE_FANOUT+1 then the tree needs to be rebalanced. */
	    unsigned int    totalchildkeylens;
	    unsigned int    n_bytes_in_buffers;

	    struct brtnode_nonleaf_childinfo childinfos[TREE_FANOUT+1]; /* One extra so we can grow */

#if 0
	    u_int32_t       child_subtree_fingerprints[TREE_FANOUT+1];
#define BRTNODE_CHILD_SUBTREE_FINGERPRINTS(node,i) ((node)->u.n.child_subtree_fingerprints[i])
#else
#define BRTNODE_CHILD_SUBTREE_FINGERPRINTS(node,i) ((node)->u.n.childinfos[i].subtree_fingerprint)
#endif

//#define CHSTRUCT
#ifdef CHSTRUCT
	    struct brtnode_nonleaf_pivotinfo pivots[TREE_FANOUT]; /* One extra one so we can grow. */
#else
	    struct kv_pair *childkeys[TREE_FANOUT];   /* Pivot keys.  Child 0's keys are <= childkeys[0].  Child 1's keys are <= childkeys[1].
							 Note: It is possible that Child 1's keys are == to child 0's key's, so it is
							 not necessarily true that child 1's keys are > childkeys[0].
						         However, in the absense of duplicate keys, child 1's keys *are* > childkeys[0]. */
	    DISKOFF         children[TREE_FANOUT+1];  /* unused if height==0 */   /* Note: The last element of these arrays is used only temporarily while splitting a node. */
#define BRTNODE_CHILD_DISKOFF(node,i) ((node)->u.n.children[i])
	    FIFO            buffers[TREE_FANOUT+1];
	    unsigned int    n_bytes_in_buffer[TREE_FANOUT+1]; /* how many bytes are in each buffer (including overheads) */
#endif
        } n;
	struct leaf {
	    PMA buffer;
	    unsigned int n_bytes_in_buffer; /* How many bytes to represent the PMA (including the per-key overheads, but not including the overheads for the node. */
	} l;
    } u;
};

/* pivot flags  (must fit in 8 bits) */
enum {
    BRT_PIVOT_TRUNC = 4,
    BRT_PIVOT_FRONT_COMPRESS = 8,
};

struct brt_header {
    int dirty;
    unsigned int nodesize;
    DISKOFF freelist;
    DISKOFF unused_memory;
    DISKOFF unnamed_root;
    int n_named_roots; /* -1 if the only one is unnamed */
    char  **names;
    DISKOFF *roots;
    unsigned int flags;
};

enum brt_header_flags {
    TOKU_DB_DUP = 1,
    TOKU_DB_DUPSORT = 2,
};

struct brt {
    CACHEFILE cf;
    char *database_name;
    // The header is shared.  It is also ephemeral.
    struct brt_header *h;

    struct list cursors;

    unsigned int nodesize;
    unsigned int flags;
    int (*compare_fun)(DB*,const DBT*,const DBT*);
    int (*dup_compare)(DB*,const DBT*,const DBT*);
    DB *db;           // To pass to the compare fun

    void *skey,*sval; /* Used for DBT return values. */
};

/* serialization code */
void toku_serialize_brtnode_to(int fd, DISKOFF off, DISKOFF size, BRTNODE node);
int toku_deserialize_brtnode_from (int fd, DISKOFF off, BRTNODE *brtnode, int flags, int nodesize, int (*bt_compare)(DB *, const DBT*, const DBT*), int (*dup_compare)(DB *, const DBT *, const DBT *), DB *db, FILENUM filenum);
unsigned int toku_serialize_brtnode_size(BRTNODE node); /* How much space will it take? */
int toku_keycompare (bytevec key1, ITEMLEN key1len, bytevec key2, ITEMLEN key2len);

void toku_verify_counts(BRTNODE);

int toku_serialize_brt_header_size (struct brt_header *h);
int toku_serialize_brt_header_to (int fd, struct brt_header *h);
int toku_serialize_brt_header_to_wbuf (struct wbuf *, struct brt_header *h);
int toku_deserialize_brtheader_from (int fd, DISKOFF off, struct brt_header **brth);

void toku_brtnode_free (BRTNODE *node);

#if 1
#define DEADBEEF ((void*)0xDEADBEEF)
#else
#define DEADBEEF ((void*)0xDEADBEEFDEADBEEF)
#endif

/* tree command types */
enum brt_cmd_type {
    BRT_NONE = 0,
    BRT_INSERT = 1,
    BRT_DELETE = 2,
    BRT_DELETE_BOTH = 3,
};

/* tree commands */
struct brt_cmd {
    enum brt_cmd_type type;
    union {
        /* insert or delete */
        struct brt_cmd_insert_delete {
            DBT *key;
            DBT *val;
        } id;
    } u;
};
typedef struct brt_cmd BRT_CMD;

struct brtenv {
    CACHETABLE ct;
    TOKULOGGER logger;
    long long checksum_number;
//    SPINLOCK  checkpointing;
};

extern cachetable_flush_func_t toku_brtnode_flush_callback, toku_brtheader_flush_callback;
extern cachetable_fetch_func_t toku_brtnode_fetch_callback, toku_brtheader_fetch_callback;
extern int toku_read_and_pin_brt_header (CACHEFILE cf, struct brt_header **header);
extern int toku_unpin_brt_header (BRT brt);
extern CACHEKEY* toku_calculate_root_offset_pointer (BRT brt);

static const BRTNODE null_brtnode=0;

extern u_int32_t toku_calccrc32_kvpair (const void *key, int keylen, const void *val, int vallen);
extern u_int32_t toku_calccrc32_cmd (int type, const void *key, int keylen, const void *val, int vallen);
extern u_int32_t toku_calccrc32_cmdstruct (BRT_CMD *cmd);

// How long is the pivot key?
unsigned int toku_brt_pivot_key_len (BRT, struct kv_pair *); // Given the tree
unsigned int toku_brtnode_pivot_key_len (BRTNODE, struct kv_pair *); // Given the node

/* a brt cursor is represented as a kv pair in a tree */
struct brt_cursor {
    struct list cursors_link;
    BRT brt;
    DBT key;
    DBT val;
};

#endif
