/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "vinyl_space.h"
#include "vinyl_engine.h"
#include "vinyl_index.h"
#include "xrow.h"
#include "tuple.h"
#include "scoped_guard.h"
#include "txn.h"
#include "index.h"
#include "space.h"
#include "schema.h"
#include "port.h"
#include "request.h"
#include "iproto_constants.h"
#include "vinyl.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

VinylSpace::VinylSpace(Engine *e)
	:Handler(e)
{}

void
VinylSpace::applySnapshotRow(struct space *space, struct request *request)
{
	assert(request->type == IPROTO_INSERT);
	assert(request->header != NULL);
	struct vy_env *env = ((VinylEngine *)space->handler->engine)->env;

	struct vy_tx *tx = vy_begin(env);
	if (tx == NULL)
		diag_raise();

	int64_t signature = request->header->lsn;
	VinylPrimaryIndex *pk = (VinylPrimaryIndex *) index_find(space, 0);
	if (vy_replace(tx, pk->db, request->tuple, request->tuple_end, NULL))
		diag_raise();

	if (vy_prepare(env, tx)) {
		vy_rollback(env, tx);
		diag_raise();
	}
	if (vy_commit(env, tx, signature))
		panic("failed to commit vinyl transaction");
}

/*
 * Four cases:
 *  - insert in one index
 *  - insert in multiple indexes
 *  - replace in one index
 *  - replace in multiple indexes.
 */
struct tuple *
VinylSpace::executeReplace(struct txn *txn, struct space *space,
			   struct request *request)
{
	assert(request->index_id == 0);
	struct vy_tx *tx = (struct vy_tx *)txn->engine_tx;
	VinylEngine *engine = (VinylEngine *)space->handler->engine;
	struct txn_stmt *stmt = txn_current_stmt(txn);
	VinylIndex *pk = (VinylIndex *) index_find_unique(space, 0);
	const char *tuple = request->tuple;
	const char *tuple_end = request->tuple_end;

	if (request->type == IPROTO_INSERT && engine->recovery_complete) {
		if (vy_insert(tx, pk->db, tuple, tuple_end))
			diag_raise();
	} else {
		if (!rlist_empty(&space->on_replace)) {
			if (vy_replace(tx, pk->db, tuple, tuple_end,
				       &stmt->old_tuple))
				diag_raise();
			if (stmt->old_tuple != NULL)
				tuple_ref(stmt->old_tuple);
		} else {
			if (vy_replace(tx, pk->db, tuple, tuple_end, NULL))
				diag_raise();
		}
	}

	struct tuple *new_tuple = tuple_new(space->format, request->tuple,
					    request->tuple_end);
	if (new_tuple == NULL)
		diag_raise();
	/* GC the new tuple if there is an exception below. */
	TupleRef ref(new_tuple);
	tuple_ref(new_tuple);
	stmt->new_tuple = new_tuple;
	return tuple_bless(new_tuple);
}

struct tuple *
VinylSpace::executeDelete(struct txn *txn, struct space *space,
                          struct request *request)
{
	VinylIndex *index;
	index = (VinylIndex *)index_find_unique(space, request->index_id);

	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	struct txn_stmt *stmt = txn_current_stmt(txn);

	struct vy_tx *tx = (struct vy_tx *)txn->engine_tx;
	/**
	 * If there is more than one index, then get the old tuple and use it
	 * to extract key parts for all secondary indexes. The old tuple is
	 * also used if the space has triggers, in which case we need to pass
	 * it into the trigger.
	 */
	if (!rlist_empty(&space->on_replace)) {
		if (vy_delete(tx, index->db, key, part_count, &stmt->old_tuple))
			diag_raise();
		if (stmt->old_tuple != NULL)
			tuple_ref(stmt->old_tuple);
	} else {
		if (vy_delete(tx, index->db, key, part_count, NULL))
			diag_raise();
	}
	return NULL;
}

struct tuple *
VinylSpace::executeUpdate(struct txn *txn, struct space *space,
                          struct request *request)
{
	uint32_t index_id = request->index_id;
	VinylIndex *index = (VinylIndex *)index_find_unique(space, index_id);
	struct vy_tx *tx = (struct vy_tx *)txn->engine_tx;
	const char *key = request->key;
	uint32_t part_count = mp_decode_array(&key);
	struct txn_stmt *stmt = txn_current_stmt(txn);
	if (vy_update(tx, index->db, key, part_count, request->tuple,
		      request->tuple_end, request->index_base, &stmt->old_tuple,
		      &stmt->new_tuple))
		diag_raise();
	if (stmt->old_tuple == NULL)
		return NULL;
	tuple_ref(stmt->old_tuple);
	tuple_ref(stmt->new_tuple);
	return tuple_bless(stmt->new_tuple);
}

void
VinylSpace::executeUpsert(struct txn *txn, struct space *space,
                           struct request *request)
{
	assert(request->index_id == 0);
	VinylIndex *pk;
	pk = (VinylIndex *)index_find_unique(space, 0);
	/*
	 * Check exact field count here, because vy_index.primary_format
	 * doesn't support it.
	 */
	const char *tuple = request->tuple;
	uint32_t field_count = mp_decode_array(&tuple);
	uint32_t exact_count = space->format->exact_field_count;
	if (exact_count > 0 && exact_count != field_count)
		tnt_raise(ClientError, ER_EXACT_FIELD_COUNT,
			  (unsigned) field_count, (unsigned) exact_count);

	struct vy_tx *tx = (struct vy_tx *)txn->engine_tx;
	if (tuple_update_check_ops(region_aligned_alloc_xc_cb, &fiber()->gc,
				   request->ops, request->ops_end,
				   request->index_base))
		diag_raise();
	if (request->index_base != 0)
		request_normalize_ops(request);
	assert(request->index_base == 0);

	struct txn_stmt *stmt = txn_current_stmt(txn);
	struct tuple *new_tuple = NULL, *old_tuple = NULL;
	if (!rlist_empty(&space->on_replace)) {
		if (vy_upsert(tx, pk->db, request->tuple, request->tuple_end,
			      request->ops, request->ops_end, &old_tuple,
			      &new_tuple)) {
			diag_raise();
		}
		if (old_tuple) {
			tuple_ref(old_tuple);
			stmt->old_tuple = old_tuple;
		}
		if (new_tuple) {
			tuple_ref(new_tuple);
			stmt->new_tuple = new_tuple;
		}
	} else {
		if (vy_upsert(tx, pk->db, request->tuple, request->tuple_end,
			      request->ops, request->ops_end, NULL, NULL)) {
			diag_raise();
		}
	}
}

Index *
VinylSpace::createIndex(struct space *space, struct key_def *key_def)
{
	VinylEngine *engine = (VinylEngine *) this->engine;
	if (key_def->type != TREE) {
		unreachable();
		return NULL;
	}
	if (key_def->iid == 0)
		return new VinylPrimaryIndex(engine->env, key_def);
	/**
	 * Get pointer to the primary key from space->index_map, because
	 * the array space->index can be empty.
	 */
	VinylPrimaryIndex *pk = (VinylPrimaryIndex *) space_index(space, 0);
	return new VinylSecondaryIndex(engine->env, pk, key_def);
}

void
VinylSpace::dropIndex(Index *index)
{
	VinylIndex *i = (VinylIndex *)index;
	/* schedule asynchronous drop */
	int rc = vy_index_drop(i->db);
	if (rc == -1)
		diag_raise();
	i->db  = NULL;
	i->env = NULL;
}

void
VinylSpace::prepareAlterSpace(struct space *old_space, struct space *new_space)
{
	if (old_space->index_count &&
	    old_space->index_count <= new_space->index_count) {
		VinylEngine *engine = (VinylEngine *)old_space->handler->engine;
		Index *primary_index = index_find(old_space, 0);
		if (engine->recovery_complete && primary_index->min(NULL, 0)) {
			/**
			 * If space is not empty then forbid new indexes creating
			 */
			tnt_raise(ClientError, ER_UNSUPPORTED, "Vinyl",
				  "altering not empty space");
		}
	}
}

void
VinylSpace::commitAlterSpace(struct space *old_space, struct space *new_space)
{
	if (new_space == NULL || new_space->index_count == 0) {
		/* This is drop space. */
		return;
	}
	(void)old_space;
	VinylPrimaryIndex *primary = (VinylPrimaryIndex *)index_find(new_space, 0);
	for (uint32_t i = 1; i < new_space->index_count; ++i) {
		((VinylSecondaryIndex *)new_space->index[i])->primary_index = primary;
	}
}
