/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."
#include <math.h>
#include <stdio.h>
#include "cachetable/checkpoint.h"
#include "test.h"
#define NUM_WORKER 8
static TOKUTXN const null_txn = 0;
static size_t valsize = 4*1024; 
static size_t keysize = 1024/8; //1k
static const size_t numrows = 4*1024*1024/5; 
static double epsilon = 0.5;  
static size_t nodesize;
static size_t basementsize;
static size_t shuffled[numrows];
static FT_HANDLE t;
static CACHETABLE ct;
 
static int uint64_dbt_cmp (DB *db, const DBT *a, const DBT *b) {
  assert(db && a && b);
  assert(a->size == sizeof(uint64_t)*keysize);
  assert(b->size == sizeof(uint64_t)*keysize);

  uint64_t x = *(uint64_t *) a->data;
  uint64_t y = *(uint64_t *) b->data;

    if (x<y) return -1;
    if (x>y) return 1;
    return 0;
}

static inline double
parse_args_for_nodesize (int argc, const char *argv[]) {
    const char *progname=argv[0];
    if(argc != 2) {
	fprintf(stderr, "Usage:\n %s [nodesize.MBs]\n", progname);
	exit(1);
    }
    argc--; argv++;
    double nodeMB;
    sscanf(argv[0], "%lf", &nodeMB);  
    return nodeMB;
}

typedef uint64_t __attribute__((__may_alias__)) uint64_t_a;

static void * random_insert(void * arg) {
  char val[valsize];
  ZERO_ARRAY(val);
  uint64_t key[keysize]; // key is 16k
  ZERO_ARRAY(key);
  DBT k, v;
  dbt_init(&k, key, keysize * 8);
  dbt_init(&v, val, valsize);
  uint64_t * array = (uint64_t *) arg;
  for (size_t i = 0; i < numrows/NUM_WORKER; i++) {
    key[0] = toku_htod64(array[i]);
    *((uint64_t_a *) val) = key [0];
    toku_ft_insert(t, &k, &v, null_txn);
  }
  return arg;
}

static inline void shuffle_array() {
  srand(time(NULL));
  for (size_t i = numrows - 1; i > 0; i--) {
    size_t j = rand() % (i + 1);
    // swap
    size_t temp = shuffled[j];
    shuffled[j] = shuffled[i];
    shuffled[i] = temp;
  }
}

int test_main(int argc, const char *argv[]) {
  // set up the node size and basement size for benchmarking
  double nodeMB = parse_args_for_nodesize(argc, argv);
  // if user supply 4, it means the nodesize = 4MB
  nodesize = nodeMB * (1 << 20);
  size_t B = nodesize / (keysize * 8 + valsize);
  int fanout = pow(B, epsilon);
  basementsize = nodesize / fanout;
  printf("Benchmarking fractal tree based on nodesize = %zu bytes(%lfMBs) \n "
         "\t key: %zu bytes (%zu KBs); value: %zu bytes (%zu KBs) \n\t B = "
         "%zu, epsilon=0.5, fanout = %d\n Preparing 4 GBs data...\n",
         nodesize, nodeMB, keysize * 8, (keysize * 8) / 1024, valsize,
         valsize / 1024, B, fanout);

  // set up the data file
  const char *n = "./negative/affine_benchmark_data";
  int r;
  unlink(n);
  toku_cachetable_create(&ct, 1024*1024*1024, ZERO_LSN, nullptr);
  //toku_cachetable_create(&ct, 0, ZERO_LSN, nullptr);
  r = toku_open_ft_handle(n, 1, &t, nodesize, basementsize, TOKU_NO_COMPRESSION,
                          ct, null_txn, uint64_dbt_cmp);
  assert(r == 0);
  // set up the fanout so the B^e =  fanout.
  toku_ft_handle_set_fanout(t, fanout);

  for (size_t i = 0; i < numrows; i++) {
    shuffled[i] = i;
  }
  shuffle_array();

  toku_pthread_t insert_tid[NUM_WORKER];
  for (int i = 0; i < NUM_WORKER; i++) {
    uint64_t *args = shuffled + i * (numrows / NUM_WORKER);
    r = toku_pthread_create(toku_uninstrumented, &insert_tid[i], nullptr,
                            random_insert, args);
    assert_zero(r);
  }
  for (int i = 0; i < NUM_WORKER; i++) {
    void *ret;
    r = toku_pthread_join(insert_tid[i], &ret);
    assert_zero(r);
  }

  CHECKPOINTER cp = toku_cachetable_get_checkpointer(ct);
  r = toku_checkpoint(cp, NULL, NULL, NULL, NULL, NULL, CLIENT_CHECKPOINT);
  assert_zero(r);
  r = toku_close_ft_handle_nolsn(t, 0);
  assert(r == 0);
  toku_cachetable_close(&ct);
  return 0;
}
