#ifndef INCLUDES_TARANTOOL_BOX_VINYL_H
#define INCLUDES_TARANTOOL_BOX_VINYL_H
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

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include <small/region.h>

#include "index.h"

#ifdef __cplusplus
extern "C" {
#endif

struct vy_env;
struct vy_tx;
struct vy_cursor;
struct vy_index;
struct key_def;
struct tuple;
struct tuple_format;
struct region;
struct vclock;

/*
 * Environment
 */

struct vy_env *
vy_env_new(void);

void
vy_env_delete(struct vy_env *e);

/*
 * Recovery
 */

void
vy_bootstrap(struct vy_env *e);

void
vy_begin_initial_recovery(struct vy_env *e, struct vclock *vclock);

void
vy_begin_final_recovery(struct vy_env *e);

void
vy_end_recovery(struct vy_env *e);

int
vy_checkpoint(struct vy_env *env);

int
vy_wait_checkpoint(struct vy_env *env, struct vclock *vlock);

/*
 * Introspection
 */

enum vy_info_type {
	VY_INFO_TABLE_BEGIN,
	VY_INFO_TABLE_END,
	VY_INFO_STRING,
	VY_INFO_U32,
	VY_INFO_U64,
};

struct vy_info_node {
	enum vy_info_type type;
	const char *key;
	union {
		const char *str;
		uint32_t u32;
		uint64_t u64;
	} value;
};

struct vy_info_handler {
	void (*fn)(struct vy_info_node *node, void *ctx);
	void *ctx;
};

void
vy_info_gather(struct vy_env *env, struct vy_info_handler *h);

/*
 * Transaction
 */

struct vy_tx *
vy_begin(struct vy_env *e);

/**
 * Get a tuple from a vinyl space.
 * @param tx          Current transaction.
 * @param index       Vinyl index.
 * @param key         MessagePack'ed data, the array without a header.
 * @param part_count  Part count of the key.
 * @param[out] result Is set to the the found tuple.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
int
vy_get(struct vy_tx *tx, struct vy_index *index,
       const char *key, uint32_t part_count, struct tuple **result);

/**
 * Execute REPLACE in indexes of the one space.
 * @param tx             Current transaction.
 * @param pk             Primary index.
 * @param tuple          MessagePack array.
 * @param tuple_end      End of MessagePack array.
 * @param[out] old_tuple If not NULL then is set to the replaced statement.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
int
vy_replace(struct vy_tx *tx, struct vy_index *pk,
	   const char *tuple, const char *tuple_end, struct tuple **old_tuple);

/**
 * Execute INSERT in all indexes of one space.
 * @param tx        Current transaction.
 * @param pk        Primary index.
 * @param tuple     MessagePack array.
 * @param tuple_end End of MessagePack array.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
int
vy_insert(struct vy_tx *tx, struct vy_index *pk,
	  const char *tuple, const char *tuple_end);

/**
 * Execute UPDATE in all indexes of one space.
 * @param tx             Current transaction.
 * @param index          Vinyl index.
 * @param key            MessagePack'ed data, the array without a header.
 * @param part_count     Part count of the key.
 * @param ops            MessagePack array of update operations.
 * @param ops_end        End of the operations array.
 * @param index_base     Index base.
 * @param[out] old_tuple Is set to the old statement.
 * @param[out] new_tuple Is set to the new statement.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
int
vy_update(struct vy_tx *tx, struct vy_index *index,
	  const char *key, uint32_t part_count,
	  const char *ops, const char *ops_end, int index_base,
	  struct tuple **old_tuple, struct tuple **new_tuple);

/**
 * Execute UPSERT in all indexes of one space.
 * @param tx             Current transaction.
 * @param pk             Primary index.
 * @param tuple          MessagePack array.
 * @param tuple_end      End of the tuple.
 * @param ops            MessagePack array of update operations.
 * @param ops_end        End of the operations array.
 * @param[out] old_tuple If not NULL then is set to the old statement.
 * @param[out] new_tuple If not NULL then is set to the new statement.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
int
vy_upsert(struct vy_tx *tx, struct vy_index *pk,
	  const char *tuple, const char *tuple_end,
	  const char *ops, const char *ops_end,
	  struct tuple **old_tuple, struct tuple **new_tuple);

/**
 * Execute DELETE in all indexes of the one space.
 * @param tx             Current transaction.
 * @param index          Vinyl index.
 * @param key            MessagePack'ed data, the array without a header.
 * @param part_count     Part count of the key.
 * @param[out] old_tuple If not NULL then is set to the deleted statement.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
int
vy_delete(struct vy_tx *tx, struct vy_index *index,
	  const char *key, uint32_t part_count, struct tuple **old_tuple);

int
vy_prepare(struct vy_env *e, struct vy_tx *tx);

int
vy_commit(struct vy_env *e, struct vy_tx *tx, int64_t lsn);

void
vy_rollback(struct vy_env *e, struct vy_tx *tx);

void *
vy_savepoint(struct vy_tx *tx);

void
vy_rollback_to_savepoint(struct vy_tx *tx, void *svp);

/*
 * Index
 */

struct key_def *
vy_index_key_def(struct vy_index *index);

/**
 * Create a new vinyl primary index without opening it.
 * @param env     Vinyl environment.
 * @param key_def Key definition of the new primary index.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
struct vy_index *
vy_index_new_primary(struct vy_env *env, const struct key_def *key_def);

/**
 * Create a new vinyl secondary index without opening it.
 * @param env     Vinyl environment.
 * @param key_def Key definition of the new secondary index.
 * @param pk      Primary index of the space, for which the new index is
 *                created.
 *
 * @retval  0 Success.
 * @retval -1 Memory error.
 */
struct vy_index *
vy_index_new_secondary(struct vy_env *env, const struct key_def *key_def,
		       struct vy_index *pk);

int
vy_index_open(struct vy_index *index);

/**
 * Close index and drop all data
 */
int
vy_index_drop(struct vy_index *index);

size_t
vy_index_bsize(struct vy_index *db);

/*
 * Index Cursor
 */

/**
 * Create a cursor. If tx is not NULL, the cursor life time is
 * bound by the transaction life time. Otherwise, the cursor
 * allocates its own transaction.
 */
struct vy_cursor *
vy_cursor_new(struct vy_tx *tx, struct vy_index *index, const char *key,
	      uint32_t part_count, enum iterator_type type);

/**
 * Fetch the transaction used in the cursor.
 */
int
vy_cursor_tx(struct vy_cursor *cursor, struct vy_tx **tx);

void
vy_cursor_delete(struct vy_cursor *cursor);

int
vy_cursor_next(struct vy_cursor *cursor, struct tuple **result);

/*
 * Replication
 */

typedef int
(*vy_send_row_f)(void *, const char *tuple, uint32_t tuple_size, int64_t lsn);
int
vy_index_send(struct vy_index *index, vy_send_row_f sendrow, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDES_TARANTOOL_BOX_VINYL_H */
