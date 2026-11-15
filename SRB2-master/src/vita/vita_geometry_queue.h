// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.

#ifndef SRB2_VITA_GEOMETRY_QUEUE_H
#define SRB2_VITA_GEOMETRY_QUEUE_H

#include "../doomtype.h"

typedef struct
{
	UINT32 head, open;
} __attribute__((aligned(64))) hwgen_queue_t;

typedef struct
{
	UINT32 tail, finished;
	precise_t busy;
} __attribute__((aligned(64))) hwgen_worker_t;

static UINT32 HWR_QueueHead(const hwgen_queue_t *queue)
{
	return __atomic_load_n(&queue->head, __ATOMIC_ACQUIRE);
}

static UINT32 HWR_QueueIsOpen(const hwgen_queue_t *queue)
{
	return __atomic_load_n(&queue->open, __ATOMIC_ACQUIRE);
}

static void HWR_QueuePublish(hwgen_queue_t *queue, UINT32 head)
{
	__atomic_store_n(&queue->head, head, __ATOMIC_RELEASE);
}

static void HWR_QueueClose(hwgen_queue_t *queue)
{
	__atomic_store_n(&queue->open, 0, __ATOMIC_RELEASE);
}

static UINT32 HWR_WorkerTail(const hwgen_worker_t *worker)
{
	return __atomic_load_n(&worker->tail, __ATOMIC_ACQUIRE);
}

static UINT32 HWR_WorkerFinished(const hwgen_worker_t *worker)
{
	return __atomic_load_n(&worker->finished, __ATOMIC_ACQUIRE);
}

static void HWR_WorkerPublishTail(hwgen_worker_t *worker, UINT32 tail)
{
	__atomic_store_n(&worker->tail, tail, __ATOMIC_RELEASE);
}

static void HWR_WorkerComplete(hwgen_worker_t *worker)
{
	__atomic_store_n(&worker->finished, 1, __ATOMIC_RELEASE);
}

#endif
