/*
This file is part of GUFI, which is part of MarFS, which is released
under the BSD license.


Copyright (c) 2017, Los Alamos National Security (LANS), LLC
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-----
NOTE:
-----

GUFI uses the C-Thread-Pool library.  The original version, written by
Johan Hanssen Seferidis, is found at
https://github.com/Pithikos/C-Thread-Pool/blob/master/LICENSE, and is
released under the MIT License.  LANS, LLC added functionality to the
original work.  The original work, plus LANS, LLC added functionality is
found at https://github.com/jti-lanl/C-Thread-Pool, also under the MIT
License.  The MIT License can be found at
https://opensource.org/licenses/MIT.


From Los Alamos National Security, LLC:
LA-CC-15-039

Copyright (c) 2017, Los Alamos National Security, LLC All rights reserved.
Copyright 2017. Los Alamos National Security, LLC. This software was produced
under U.S. Government contract DE-AC52-06NA25396 for Los Alamos National
Laboratory (LANL), which is operated by Los Alamos National Security, LLC for
the U.S. Department of Energy. The U.S. Government has rights to use,
reproduce, and distribute this software.  NEITHER THE GOVERNMENT NOR LOS
ALAMOS NATIONAL SECURITY, LLC MAKES ANY WARRANTY, EXPRESS OR IMPLIED, OR
ASSUMES ANY LIABILITY FOR THE USE OF THIS SOFTWARE.  If software is
modified to produce derivative works, such modified software should be
clearly marked, so as not to confuse it with the version available from
LANL.

THIS SOFTWARE IS PROVIDED BY LOS ALAMOS NATIONAL SECURITY, LLC AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL LOS ALAMOS NATIONAL SECURITY, LLC OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
OF SUCH DAMAGE.
*/



#ifndef QUEUE_PER_THREAD_POOL
#define QUEUE_PER_THREAD_POOL

#include <pthread.h>

#include "bf.h"

// Singly linked list that is used like a queue
struct node;
struct sll {
    struct node * head;
    struct node * tail;
};

struct sll * sll_init(struct sll * sll);
struct sll * sll_push(struct sll * sll, void * data);
struct sll * sll_move(struct sll * dst, struct sll * src);

// functions for looping over a sll
struct node * sll_head_node(struct sll * sll);
struct node * sll_next_node(struct node * node);
void * sll_node_data(struct node * node);

void sll_destroy(struct sll * sll);


/* The context for a single thread in QPTPool */
struct QPTPoolData {
    size_t id;
    struct sll queue;
    pthread_mutex_t mutex;
    pthread_cond_t cv;
    pthread_t thread;
    size_t threads_started;
    size_t threads_successful;
};

// The Queue Per Thread Pool context
struct QPTPool {
    struct QPTPoolData * data;
    size_t size;

    pthread_mutex_t mutex;
    size_t incomplete;
};

/* User defined function to pass into QPTPool_start
 *
 *     QPTPool   to provide context
 *     void *    for the data this thread is going to work on
 *     size_t    for id of this thread
 *     size_t *  for next queue to push on to
 *     void *    extra args
 */
typedef int (*QPTPoolFunc_t)(struct QPTPool *, void *, const size_t, size_t *, void *);

struct QPTPool * QPTPool_init(const size_t threads);
void QPTPool_enqueue_external(struct QPTPool * ctx, void * new_work);
void QPTPool_enqueue_internal(struct QPTPool * ctx, void * new_work, size_t * next_queue);
size_t QPTPool_start(struct QPTPool * ctx, QPTPoolFunc_t func, void * args);
void QPTPool_wait(struct QPTPool * ctx);
void QPTPool_destroy(struct QPTPool * ctx);

// utility functions
size_t QPTPool_get_index(struct QPTPool * ctx, const pthread_t id);      // get a number in the range [0, # of threads), or a value outside of that range on error
size_t QPTPool_threads_started(struct QPTPool * ctx);
size_t QPTPool_threads_completed(struct QPTPool * ctx);


#endif
