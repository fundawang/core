/* Copyright (C) 2004 Timo Sirainen */

#include "lib.h"
#include "buffer.h"
#include "file-set-size.h"
#include "mmap-util.h"
#include "mail-index-view-private.h"
#include "mail-index-sync-private.h"
#include "mail-transaction-log.h"
#include "mail-transaction-util.h"

static void
mail_index_header_update_counts(struct mail_index_header *hdr,
				uint8_t old_flags, uint8_t new_flags)
{
	if (((old_flags ^ new_flags) & MAIL_RECENT) != 0) {
		/* different recent-flag */
		if ((old_flags & MAIL_RECENT) == 0)
			hdr->recent_messages_count++;
		else if (--hdr->recent_messages_count == 0)
			hdr->first_recent_uid_lowwater = hdr->next_uid;
	}

	if (((old_flags ^ new_flags) & MAIL_SEEN) != 0) {
		/* different seen-flag */
		if ((old_flags & MAIL_SEEN) != 0)
			hdr->seen_messages_count--;
		else if (++hdr->seen_messages_count == hdr->messages_count)
			hdr->first_unseen_uid_lowwater = hdr->next_uid;
	}

	if (((old_flags ^ new_flags) & MAIL_DELETED) != 0) {
		/* different deleted-flag */
		if ((old_flags & MAIL_DELETED) == 0)
			hdr->deleted_messages_count++;
		else if (--hdr->deleted_messages_count == 0)
			hdr->first_deleted_uid_lowwater = hdr->next_uid;
	}
}

static void
mail_index_header_update_lowwaters(struct mail_index_header *hdr,
				   const struct mail_index_record *rec)
{
	if ((rec->flags & MAIL_RECENT) != 0 &&
	    rec->uid < hdr->first_recent_uid_lowwater)
		hdr->first_recent_uid_lowwater = rec->uid;
	if ((rec->flags & MAIL_SEEN) == 0 &&
	    rec->uid < hdr->first_unseen_uid_lowwater)
		hdr->first_unseen_uid_lowwater = rec->uid;
	if ((rec->flags & MAIL_DELETED) != 0 &&
	    rec->uid < hdr->first_deleted_uid_lowwater)
		hdr->first_deleted_uid_lowwater = rec->uid;
}

static int sync_expunge(const struct mail_transaction_expunge *e, void *context)
{
	struct mail_index_view *view = context;
	struct mail_index *index = view->index;
	struct mail_index_map *map = view->map;
	struct mail_index_header *hdr = &map->hdr_copy;
	struct mail_index_record *rec;
	uint32_t count, seq, seq1, seq2;
	int ret;

	i_assert(MAIL_INDEX_MAP_IS_IN_MEMORY(map));

	ret = mail_index_lookup_uid_range(view, e->uid1, e->uid2, &seq1, &seq2);
	i_assert(ret == 0);

	if (seq1 == 0)
		return 1;

	for (seq = seq1; seq <= seq2; seq++) {
                rec = MAIL_INDEX_MAP_IDX(index, map, seq-1);
		mail_index_header_update_counts(hdr, rec->flags, 0);
	}

	/* @UNSAFE */
	count = seq2 - seq1 + 1;
	memmove(MAIL_INDEX_MAP_IDX(index, map, seq1-1),
		MAIL_INDEX_MAP_IDX(index, map, seq2),
		(map->records_count - seq2) * view->index->record_size);

	map->records_count -= count;
	hdr->messages_count -= count;
	view->messages_count -= count;

	if (map->buffer != NULL) {
		buffer_set_used_size(map->buffer, map->records_count *
				     view->index->record_size);
		map->records = buffer_get_modifyable_data(map->buffer, NULL);
	}
	return 1;
}

static int sync_append(const struct mail_index_record *rec, void *context)
{
	struct mail_index_view *view = context;
	struct mail_index *index = view->index;
	struct mail_index_map *map = view->map;

	if (rec->uid < map->hdr_copy.next_uid) {
		mail_transaction_log_view_set_corrupted(view->log_view,
			"Append with UID %u, but next_uid = %u",
			rec->uid, map->hdr_copy.next_uid);
		return -1;
	}

	if (MAIL_INDEX_MAP_IS_IN_MEMORY(map)) {
		i_assert(map->records_count * index->record_size ==
			 buffer_get_used_size(map->buffer));
		buffer_append(map->buffer, rec, index->record_size);
		map->records = buffer_get_modifyable_data(map->buffer, NULL);
	} else {
		i_assert((map->records_count+1) * index->record_size <=
			 map->mmap_size);
		memcpy(MAIL_INDEX_MAP_IDX(index, map, map->records_count),
		       rec, index->record_size);
	}

	map->hdr_copy.messages_count++;
	map->hdr_copy.next_uid = rec->uid+1;
	view->messages_count++;
	map->records_count++;

	mail_index_header_update_counts(&map->hdr_copy, 0, rec->flags);
	mail_index_header_update_lowwaters(&map->hdr_copy, rec);
	return 1;
}

static int sync_flag_update(const struct mail_transaction_flag_update *u,
			    void *context)
{
        struct mail_index_view *view = context;
	struct mail_index_record *rec;
	struct mail_index_header *hdr;
	uint8_t flag_mask, old_flags;
	keywords_mask_t keyword_mask;
	uint32_t i, idx, seq1, seq2;
	int update_keywords, ret;

	ret = mail_index_lookup_uid_range(view, u->uid1, u->uid2, &seq1, &seq2);
	i_assert(ret == 0);

	if (seq1 == 0)
		return 1;

	hdr = &view->map->hdr_copy;

	if ((u->add_flags & MAIL_INDEX_MAIL_FLAG_DIRTY) != 0)
		hdr->flags |= MAIL_INDEX_HDR_FLAG_HAVE_DIRTY;

	update_keywords = FALSE;
	for (i = 0; i < INDEX_KEYWORDS_BYTE_COUNT; i++) {
		if (u->add_keywords[i] != 0 ||
		    u->remove_keywords[i] != 0)
			update_keywords = TRUE;
		keyword_mask[i] = ~u->remove_keywords[i];
	}
        flag_mask = ~u->remove_flags;

	for (idx = seq1-1; idx < seq2; idx++) {
                rec = MAIL_INDEX_MAP_IDX(view->index, view->map, idx);

		old_flags = rec->flags;
		rec->flags = (rec->flags & flag_mask) | u->add_flags;
		if (update_keywords) {
			for (i = 0; i < INDEX_KEYWORDS_BYTE_COUNT; i++) {
				rec->keywords[i] = u->add_keywords[i] |
					(rec->keywords[i] & keyword_mask[i]);
			}
		}

		mail_index_header_update_counts(hdr, old_flags, rec->flags);
                mail_index_header_update_lowwaters(hdr, rec);
	}
	return 1;
}

static int sync_cache_reset(const struct mail_transaction_cache_reset *u,
			    void *context)
{
	struct mail_index_view *view = context;
	uint32_t i;

	view->map->hdr_copy.cache_file_seq = u->new_file_seq;

	for (i = 0; i < view->messages_count; i++)
		MAIL_INDEX_MAP_IDX(view->index, view->map, i)->cache_offset = 0;
	return 1;
}

static int sync_cache_update(const struct mail_transaction_cache_update *u,
			     void *context)
{
	struct mail_index_view *view = context;
	uint32_t seq;
	int ret;

	ret = mail_index_lookup_uid_range(view, u->uid, u->uid,
					  &seq, &seq);
	i_assert(ret == 0);

	if (seq != 0) {
		MAIL_INDEX_MAP_IDX(view->index, view->map, seq-1)->
			cache_offset = u->cache_offset;
	}
	return 1;
}

static int sync_header_update(const struct mail_transaction_header_update *u,
			      void *context)
{
	struct mail_index_view *view = context;
	void *data;

	data = PTR_OFFSET(&view->map->hdr_copy, u->offset);
	memcpy(data, u->data, u->size);
	return 1;
}

static int
sync_extra_rec_update(const struct mail_transaction_extra_rec_header *hdr,
		      const struct mail_transaction_extra_rec_update *u,
		      void *context)
{
	struct mail_index_view *view = context;
	struct mail_index_record *rec;
	uint32_t seq;
	uint16_t offset, size;
	int ret;

	/* FIXME: do data_id mapping conversion */

	ret = mail_index_lookup_uid_range(view, u->uid, u->uid,
					  &seq, &seq);
	i_assert(ret == 0);

	if (seq != 0) {
		offset = view->index->extra_records[hdr->idx].offset;
		size = view->index->extra_records[hdr->idx].size;

		rec = MAIL_INDEX_MAP_IDX(view->index, view->map, seq-1);
		memcpy(PTR_OFFSET(rec, offset), u->data, size);
	}
	return 1;
}

static int mail_index_grow(struct mail_index *index, struct mail_index_map *map,
			   unsigned int count)
{
	size_t size;

	if (MAIL_INDEX_MAP_IS_IN_MEMORY(map))
		return 0;

	i_assert(map == index->map);

	size = map->hdr->header_size +
		(map->records_count + count) * index->record_size;
	if (size <= map->mmap_size)
		return 0;

	/* when we grow fast, do it exponentially */
	if (count < index->last_grow_count)
		count = index->last_grow_count;
	if (count < MAIL_INDEX_MAX_POWER_GROW)
		count = nearest_power(count);
	index->last_grow_count = count;

	size = map->hdr->header_size +
		(map->records_count + count) * index->record_size;
	if (file_set_size(index->fd, (off_t)size) < 0)
		return mail_index_set_syscall_error(index, "file_set_size()");

	if (mail_index_map(index, TRUE) <= 0)
		return -1;

	i_assert(map->mmap_size >= size);
	return 0;
}

int mail_index_sync_update_index(struct mail_index_sync_ctx *sync_ctx)
{
	struct mail_index *index = sync_ctx->index;
	struct mail_index_view *view = sync_ctx->view;
	struct mail_index_map *map;
	const struct mail_transaction_header *hdr;
	const void *data;
	unsigned int lock_id, count;
	uint32_t seq, i;
	uoff_t offset;
	int ret, had_dirty, skipped;

	if (mail_index_lock_exclusive(index, &lock_id) < 0)
		return -1;

	/* NOTE: locking may change index->map so make sure assignment
	   after locking */
	map = index->map;
	if (MAIL_INDEX_MAP_IS_IN_MEMORY(map))
		map->write_to_disk = TRUE;

	map->hdr_copy = *map->hdr;
	map->hdr = &map->hdr_copy;

	mail_index_unmap(index, view->map);
	view->map = map;
	view->map->refcount++;

	had_dirty = (map->hdr_copy.flags & MAIL_INDEX_HDR_FLAG_HAVE_DIRTY) != 0;
	if (had_dirty)
		map->hdr_copy.flags &= ~MAIL_INDEX_HDR_FLAG_HAVE_DIRTY;

	while ((ret = mail_transaction_log_view_next(view->log_view, &hdr,
						     &data, &skipped)) > 0) {
		if ((hdr->type & MAIL_TRANSACTION_EXPUNGE) != 0 &&
		    !map->write_to_disk) {
			/* expunges have to be atomic. so we'll have to copy
			   the mapping, do the changes there and then finally
			   replace the whole index file. to avoid extra disk
			   I/O we copy the index into memory rather than to
			   temporary file */
			map = mail_index_map_to_memory(index, map);
			mail_index_unmap(index, view->map);
			view->map = map;
			view->map->refcount++;
			mail_index_unmap(index, index->map);
			index->map = map;
			index->hdr = map->hdr;
			map->write_to_disk = TRUE;
		}

		if ((hdr->type & MAIL_TRANSACTION_APPEND) != 0) {
			count = hdr->size / index->record_size;
			if (mail_index_grow(index, view->map, count) < 0)
				return -1;
		}

		if (mail_transaction_map(index, hdr, data,
					 &mail_index_map_sync_funcs,
					 view) < 0) {
			ret = -1;
			break;
		}
	}

	if (ret < 0)
		return -1;

	mail_transaction_log_get_head(index->log, &seq, &offset);

	map->hdr_copy.log_file_seq = seq;
	map->hdr_copy.log_file_offset = offset;

	if ((map->hdr_copy.flags & MAIL_INDEX_HDR_FLAG_HAVE_DIRTY) == 0 &&
	    had_dirty) {
		/* do we have dirty flags anymore? */
		const struct mail_index_record *rec;

		for (i = 0; i < map->records_count; i++) {
			rec = MAIL_INDEX_MAP_IDX(index, map, i);
			if ((rec->flags & MAIL_INDEX_MAIL_FLAG_DIRTY) != 0) {
				map->hdr_copy.flags |=
					MAIL_INDEX_HDR_FLAG_HAVE_DIRTY;
				break;
			}
		}
	}

	if (!MAIL_INDEX_MAP_IS_IN_MEMORY(map)) {
		map->mmap_used_size = index->hdr->header_size +
			map->records_count * index->record_size;

		memcpy(map->mmap_base, &map->hdr_copy, sizeof(map->hdr_copy));
		if (msync(map->mmap_base, map->mmap_used_size, MS_SYNC) < 0) {
			mail_index_set_syscall_error(index, "msync()");
			ret = -1;
		}
		map->hdr = map->mmap_base;
	}

	mail_index_unlock(index, lock_id);
	return ret;
}

struct mail_transaction_map_functions mail_index_map_sync_funcs = {
	sync_expunge, sync_append, sync_flag_update,
	sync_cache_reset, sync_cache_update, sync_header_update,
	sync_extra_rec_update
};
