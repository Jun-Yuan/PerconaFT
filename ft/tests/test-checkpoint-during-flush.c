/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id: test-del-inorder.c 32975 2011-07-11 23:42:51Z leifwalsh $"
/* The goal of this test.  Make sure that inserts stay behind deletes. */


#include "test.h"
#include "includes.h"
#include <ft-cachetable-wrappers.h>
#include "ft-flusher.h"
#include "ft-flusher-internal.h"
#include "checkpoint.h"

static TOKUTXN const null_txn = 0;
static DB * const null_db = 0;

enum { NODESIZE = 1024, KSIZE=NODESIZE-100, TOKU_PSIZE=20 };

CACHETABLE ct;
FT_HANDLE t;

BOOL checkpoint_called;
BOOL checkpoint_callback_called;
toku_pthread_t checkpoint_tid;


// callback functions for flush_some_child
static bool
dont_destroy_bn(void* UU(extra))
{
    return false;
}
static void merge_should_not_happen(struct flusher_advice* UU(fa),
                              FT UU(h),
                              FTNODE UU(parent),
                              int UU(childnum),
                              FTNODE UU(child),
                              void* UU(extra))
{
    assert(FALSE);
}

static bool recursively_flush_should_not_happen(FTNODE UU(child), void* UU(extra)) {
    assert(FALSE);
}

static int child_to_flush(FT UU(h), FTNODE parent, void* UU(extra)) {
    assert(parent->height == 1);
    assert(parent->n_children == 1);
    return 0;
}

static void dummy_update_status(FTNODE UU(child), int UU(dirtied), void* UU(extra)) {
}


static void checkpoint_callback(void* UU(extra)) {
    usleep(1*1024*1024);
    checkpoint_callback_called = TRUE;
}


static void *do_checkpoint(void *arg) {
    // first verify that checkpointed_data is correct;
    if (verbose) printf("starting a checkpoint\n");
    int r = toku_checkpoint(ct, NULL, checkpoint_callback, NULL, NULL, NULL, CLIENT_CHECKPOINT);
    assert_zero(r);
    if (verbose) printf("completed a checkpoint\n");
    return arg;
}


static void flusher_callback(int state, void* extra) {
    BOOL after_child_pin = *(BOOL *)extra;
    if (verbose) {
        printf("state %d\n", state);
    }
    if ((state == flt_flush_before_child_pin && !after_child_pin) ||
        (state == ft_flush_aflter_child_pin && after_child_pin)) {
        checkpoint_called = TRUE;
        int r = toku_pthread_create(&checkpoint_tid, NULL, do_checkpoint, NULL); 
        assert_zero(r);
        while (!checkpoint_callback_called) {
            usleep(1*1024*1024);
        }
    }
}

static void
doit (BOOL after_child_pin) {
    BLOCKNUM node_leaf, node_root;

    int r;
    checkpoint_called = FALSE;
    checkpoint_callback_called = FALSE;

    toku_flusher_thread_set_callback(flusher_callback, &after_child_pin);
    
    r = toku_create_cachetable(&ct, 500*1024*1024, ZERO_LSN, NULL_LOGGER); assert(r==0);
    unlink("foo1.ft_handle");
    r = toku_open_ft_handle("foo1.ft_handle", 1, &t, NODESIZE, NODESIZE/2, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);

    toku_testsetup_initialize();  // must precede any other toku_testsetup calls

    r = toku_testsetup_leaf(t, &node_leaf, 1, NULL, NULL);
    assert(r==0);

    r = toku_testsetup_nonleaf(t, 1, &node_root, 1, &node_leaf, 0, 0);
    assert(r==0);

    r = toku_testsetup_root(t, node_root);
    assert(r==0);


    r = toku_testsetup_insert_to_nonleaf(
        t, 
        node_root, 
        FT_INSERT,
        "a",
        2,
        NULL,
        0
        );

    // at this point, we have inserted a message into
    // the root, and we wish to flush it, the leaf
    // should be empty

    struct flusher_advice fa;
    flusher_advice_init(
        &fa,
        child_to_flush,
        dont_destroy_bn,
        recursively_flush_should_not_happen,
        merge_should_not_happen,
        dummy_update_status,
        default_pick_child_after_split,
        NULL
        );
    
    FTNODE node = NULL;
    struct ftnode_fetch_extra bfe;
    fill_bfe_for_min_read(&bfe, t->h);
    toku_pin_ftnode_off_client_thread(
        t->h, 
        node_root,
        toku_cachetable_hash(t->h->cf, node_root),
        &bfe,
        TRUE, 
        0,
        NULL,
        &node
        );
    assert(node->height == 1);
    assert(node->n_children == 1);
    assert(toku_bnc_nbytesinbuf(BNC(node, 0)) > 0);

    // do the flush
    flush_some_child(t->h, node, &fa);
    assert(checkpoint_callback_called);

    // now let's pin the root again and make sure it is flushed
    toku_pin_ftnode_off_client_thread(
        t->h, 
        node_root,
        toku_cachetable_hash(t->h->cf, node_root),
        &bfe,
        TRUE, 
        0,
        NULL,
        &node
        );
    assert(node->height == 1);
    assert(node->n_children == 1);
    assert(toku_bnc_nbytesinbuf(BNC(node, 0)) == 0);
    toku_unpin_ftnode(t->h, node);

    void *ret;
    r = toku_pthread_join(checkpoint_tid, &ret); 
    assert_zero(r);

    //
    // now the dictionary has been checkpointed
    // copy the file to something with a new name,
    // open it, and verify that the state of what is
    // checkpointed is what we expect
    //

    r = system("cp foo1.ft_handle bar1.ft_handle ");
    assert_zero(r);

    FT_HANDLE c_ft;
    r = toku_open_ft_handle("bar1.ft_handle", 0, &c_ft, NODESIZE, NODESIZE/2, TOKU_DEFAULT_COMPRESSION_METHOD, ct, null_txn, toku_builtin_compare_fun);
    assert(r==0);

    //
    // now pin the root, verify that we have a message in there, and that it is clean
    //
    fill_bfe_for_full_read(&bfe, c_ft->h);
    toku_pin_ftnode_off_client_thread(
        c_ft->h, 
        node_root,
        toku_cachetable_hash(c_ft->h->cf, node_root),
        &bfe,
        TRUE, 
        0,
        NULL,
        &node
        );
    assert(node->height == 1);
    assert(!node->dirty);
    assert(node->n_children == 1);
    if (after_child_pin) {
        assert(toku_bnc_nbytesinbuf(BNC(node, 0)) == 0);
    }
    else {
        assert(toku_bnc_nbytesinbuf(BNC(node, 0)) > 0);
    }
    toku_unpin_ftnode_off_client_thread(c_ft->h, node);

    toku_pin_ftnode_off_client_thread(
        c_ft->h, 
        node_leaf,
        toku_cachetable_hash(c_ft->h->cf, node_root),
        &bfe,
        TRUE, 
        0,
        NULL,
        &node
        );
    assert(node->height == 0);
    assert(!node->dirty);
    assert(node->n_children == 1);
    if (after_child_pin) {
        assert(BLB_NBYTESINBUF(node,0) > 0);
    }
    else {
        assert(BLB_NBYTESINBUF(node,0) == 0);
    }
    toku_unpin_ftnode_off_client_thread(c_ft->h, node);

    struct check_pair pair1 = {2, "a", 0, NULL, 0};
    DBT k;
    r = toku_ft_lookup(c_ft, toku_fill_dbt(&k, "a", 2), lookup_checkf, &pair1);
    assert(r==0);


    r = toku_close_ft_handle_nolsn(t, 0);    assert(r==0);
    r = toku_close_ft_handle_nolsn(c_ft, 0);    assert(r==0);
    r = toku_cachetable_close(&ct); assert(r==0);
}

int
test_main (int argc __attribute__((__unused__)), const char *argv[] __attribute__((__unused__))) {
    default_parse_args(argc, argv);
    doit(FALSE);
    doit(TRUE);
    return 0;
}