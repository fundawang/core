/* Copyright (C) 2003-2006 Timo Sirainen */

#include "lib.h"
#include "istream.h"
#include "array.h"
#include "buffer.h"
#include "str.h"
#include "message-date.h"
#include "message-parser.h"
#include "istream-header-filter.h"
#include "imap-envelope.h"
#include "imap-bodystructure.h"
#include "index-storage.h"
#include "index-mail.h"

#include <stdlib.h>

struct index_header_lookup_ctx {
	struct mailbox_header_lookup_ctx ctx;
	pool_t pool;

	unsigned int count;
	unsigned int *idx;
	const char **name;
};

static int header_line_cmp(const void *p1, const void *p2)
{
	const struct index_mail_line *l1 = p1, *l2 = p2;
	int diff;

	diff = (int)l1->field_idx - (int)l2->field_idx;
	return diff != 0 ? diff :
		(int)l1->line_num - (int)l2->line_num;
}

static void index_mail_parse_header_finish(struct index_mail *mail)
{
	struct index_mail_line *lines;
	const unsigned char *header, *data;
	const uint8_t *match;
	buffer_t *buf;
	size_t data_size;
	unsigned int i, j, count, match_idx, match_count;
	bool noncontiguous;

	t_push();

	lines = array_get_modifyable(&mail->header_lines, &count);

	/* sort it first so fields are grouped together and ordered by
	   line number */
	qsort(lines, count, sizeof(*lines), header_line_cmp);

	match = array_get(&mail->header_match, &match_count);
	header = buffer_get_data(mail->header_data, NULL);
	buf = buffer_create_dynamic(pool_datastack_create(), 256);

	/* go through all the header lines we found */
	for (i = match_idx = 0; i < count; i = j) {
		/* matches and header lines are both sorted, all matches
		   until lines[i] weren't found */
		while (match_idx < lines[i].field_idx &&
		       match_idx < match_count) {
			/* if match[] doesn't have header_match_value,
			   it belongs to some older header parsing and we
			   just want to ignore it. */
			i_assert(match[match_idx] !=
				 mail->header_match_value + 1);
			if (match[match_idx] == mail->header_match_value &&
			    mail_cache_field_exists(mail->trans->cache_view,
						    mail->data.seq,
						    match_idx) == 0) {
				/* this header doesn't exist. remember that. */
				index_mail_cache_add(mail, match_idx, NULL, 0);
			}
			match_idx++;
		}

		if (match_idx < match_count) {
			/* save index to first header line */
			j = i + 1;
			array_idx_set(&mail->header_match_lines, match_idx, &j);
			match_idx++;
		}

		if (!lines[i].cache) {
			/* header is already cached */
			j = i + 1;
			continue;
		}

		/* buffer contains: { uint32_t line_num[], 0, header texts }
		   noncontiguous is just a small optimization.. */
		buffer_set_used_size(buf, 0);
		buffer_append(buf, &lines[i].line_num,
			      sizeof(lines[i].line_num));

		noncontiguous = FALSE;
		for (j = i+1; j < count; j++) {
			if (lines[j].field_idx != lines[i].field_idx)
				break;

			if (lines[j].start_pos != lines[j-1].end_pos)
				noncontiguous = TRUE;
			buffer_append(buf, &lines[j].line_num,
				      sizeof(lines[j].line_num));
		}
		buffer_append_zero(buf, sizeof(uint32_t));

		if (noncontiguous) {
			for (; i < j; i++) {
				buffer_append(buf, header + lines[i].start_pos,
					      lines[i].end_pos -
					      lines[i].start_pos);
			}
			i--;
		} else {
			buffer_append(buf, header + lines[i].start_pos,
				      lines[j-1].end_pos - lines[i].start_pos);
		}

		data = buffer_get_data(buf, &data_size);
		index_mail_cache_add(mail, lines[i].field_idx, data, data_size);
	}

	for (; match_idx < match_count; match_idx++) {
		if (match[match_idx] == mail->header_match_value &&
		    mail_cache_field_exists(mail->trans->cache_view,
					    mail->data.seq, match_idx) == 0) {
			/* this header doesn't exist. remember that. */
			index_mail_cache_add(mail, match_idx, NULL, 0);
		}
	}

	/* set all non-found headers registered in cache */
	for (i = 0; i < mail->data.all_cache_fields_count; i++) {
		unsigned int cache_field = mail->data.all_cache_fields[i].idx;

		/* first check that it isn't already added in this session */
		if (cache_field < match_count &&
		    (match[cache_field] & ~1) == mail->header_match_value)
			continue;

		if (strncasecmp(mail->data.all_cache_fields[i].name,
				"hdr.", 4) != 0)
			continue;

		/* check that it hadn't been added in some older session */
		if (mail_cache_field_exists(mail->trans->cache_view,
					    mail->data.seq, cache_field) == 0)
			index_mail_cache_add(mail, cache_field, NULL, 0);
	}
	t_pop();
}

static unsigned int
get_header_field_idx(struct index_mailbox *ibox, const char *field)
{
	struct mail_cache_field header_field = {
		NULL, 0, MAIL_CACHE_FIELD_HEADER, 0,
		MAIL_CACHE_DECISION_TEMP
	};

	t_push();
	header_field.name = t_strconcat("hdr.", field, NULL);
	mail_cache_register_fields(ibox->cache, &header_field, 1);
	t_pop();
	return header_field.idx;
}

void index_mail_parse_header_init(struct index_mail *mail,
				  struct mailbox_header_lookup_ctx *_headers)
{
	struct index_header_lookup_ctx *headers =
		(struct index_header_lookup_ctx *)_headers;
	size_t i;

	mail->header_seq = mail->data.seq;
	if (mail->header_data == NULL) {
		mail->header_data = buffer_create_dynamic(default_pool, 4096);
		ARRAY_CREATE(&mail->header_lines, default_pool,
			     struct index_mail_line, 32);
		ARRAY_CREATE(&mail->header_match, default_pool, uint8_t, 32);
		ARRAY_CREATE(&mail->header_match_lines, default_pool,
			     unsigned int, 32);
	} else {
		buffer_set_used_size(mail->header_data, 0);
		array_clear(&mail->header_lines);
		array_clear(&mail->header_match_lines);
	}

        mail->header_match_value += 2;
	if (mail->header_match_value == 0) {
		/* wrapped, we'll have to clear the buffer */
		array_clear(&mail->header_match);
		mail->header_match_value = 2;
	}

	if (headers != NULL) {
		for (i = 0; i < headers->count; i++) {
			array_idx_set(&mail->header_match, headers->idx[i],
				      &mail->header_match_value);
		}
	}

	if (mail->wanted_headers != NULL && mail->wanted_headers != headers) {
		headers = mail->wanted_headers;
		for (i = 0; i < headers->count; i++) {
			array_idx_set(&mail->header_match, headers->idx[i],
				      &mail->header_match_value);
		}
	}

	if (mail->data.save_sent_date) {
                mail->data.save_sent_date = FALSE;

		array_idx_set(&mail->header_match,
			      get_header_field_idx(mail->ibox, "Date"),
			      &mail->header_match_value);
	}

	/* get a list of all currently cached fields. we'll later set those
	   headers that weren't found to empty. if this list is get later,
	   it's possible that it has already changed (cache has no permanent
	   write locks, remember!) and we set some headers to empty even
	   though they really exist. */
	mail->data.all_cache_fields =
		mail_cache_register_get_list(mail->ibox->cache,
			mail->data_pool, &mail->data.all_cache_fields_count);
}

static void index_mail_parse_finish_imap_envelope(struct index_mail *mail)
{
	string_t *str;

	str = str_new(mail->data_pool, 256);
	imap_envelope_write_part_data(mail->data.envelope_data, str);
	mail->data.envelope = str_c(str);

	index_mail_cache_add(mail, MAIL_CACHE_IMAP_ENVELOPE,
			     str_data(str), str_len(str));
}

void index_mail_parse_header(struct message_part *part,
			     struct message_header_line *hdr,
			     struct index_mail *mail)
{
	struct index_mail_data *data = &mail->data;
	const char *cache_field_name;
	unsigned int field_idx, count;
	uint8_t *match;

        data->parse_line_num++;

	if (data->save_bodystructure_header) {
		i_assert(part != NULL);
		imap_bodystructure_parse_header(mail->data_pool, part, hdr);
	}

	if (data->save_envelope) {
		imap_envelope_parse_header(mail->data_pool,
					   &data->envelope_data, hdr);

		if (hdr == NULL)
                        index_mail_parse_finish_imap_envelope(mail);
	}

	if (hdr == NULL) {
		/* end of headers */
		if (data->sent_date.time != (time_t)-1) {
			index_mail_cache_add(mail, MAIL_CACHE_SENT_DATE,
					     &data->sent_date,
					     sizeof(data->sent_date));
		}
		index_mail_parse_header_finish(mail);
                data->save_bodystructure_header = FALSE;
		return;
	}

	if (!hdr->continued) {
		t_push();
		cache_field_name = t_strconcat("hdr.", hdr->name, NULL);
		data->parse_line.field_idx =
			mail_cache_register_lookup(mail->ibox->cache,
						   cache_field_name);
		t_pop();
	}
	field_idx = data->parse_line.field_idx;

	if (field_idx == (unsigned int)-1) {
		/* we don't want this field */
		return;
	}

	if (!hdr->continued) {
		data->parse_line.cache =
			mail_cache_field_want_add(mail->trans->cache_trans,
						  data->seq, field_idx);
	}

	match = array_get_modifyable(&mail->header_match, &count);
	if (field_idx < count && match[field_idx] == mail->header_match_value) {
		/* first header */
		match[field_idx]++;
	} else if (!data->parse_line.cache &&
		   (field_idx >= count ||
		    (match[field_idx] & ~1) != mail->header_match_value)) {
		/* we don't need to do anything with this header */
		return;
	}

	if (!hdr->continued) {
		data->parse_line.start_pos = str_len(mail->header_data);
		data->parse_line.line_num = data->parse_line_num;
		str_append(mail->header_data, hdr->name);
		str_append_n(mail->header_data, hdr->middle, hdr->middle_len);
	}
	str_append_n(mail->header_data, hdr->value, hdr->value_len);
	if (!hdr->no_newline)
		str_append(mail->header_data, "\n");
	if (!hdr->continues) {
		data->parse_line.end_pos = str_len(mail->header_data);
		array_append(&mail->header_lines, &data->parse_line, 1);
	}
}

static void
index_mail_parse_part_header_cb(struct message_part *part,
				struct message_header_line *hdr, void *context)
{
	struct index_mail *mail = context;

	index_mail_parse_header(part, hdr, mail);
}

static void
index_mail_parse_header_cb(struct message_header_line *hdr, void *context)
{
	struct index_mail *mail = context;

	index_mail_parse_header(mail->data.parts, hdr, mail);
}

int index_mail_parse_headers(struct index_mail *mail,
			     struct mailbox_header_lookup_ctx *headers)
{
	struct index_mail_data *data = &mail->data;

	if (mail_get_stream(&mail->mail.mail, NULL, NULL) == NULL)
		return -1;

	index_mail_parse_header_init(mail, headers);

	if (data->parts == NULL && data->parser_ctx == NULL) {
		/* initialize bodystructure parsing in case we read the whole
		   message. */
		data->parser_ctx =
			message_parser_init(mail->data_pool, data->stream);
		message_parser_parse_header(data->parser_ctx, &data->hdr_size,
					    index_mail_parse_part_header_cb,
					    mail);
	} else {
		/* just read the header */
		message_parse_header(data->stream, &data->hdr_size,
				     index_mail_parse_header_cb, mail);
	}
	data->hdr_size_set = TRUE;
	data->access_part &= ~PARSE_HDR;

	return 0;
}

static void
imap_envelope_parse_callback(struct message_header_line *hdr, void *context)
{
	struct index_mail *mail = context;

	imap_envelope_parse_header(mail->data_pool,
				   &mail->data.envelope_data, hdr);

	if (hdr == NULL)
		index_mail_parse_finish_imap_envelope(mail);
}

void index_mail_headers_get_envelope(struct index_mail *mail)
{
	struct mailbox_header_lookup_ctx *header_ctx;
	struct istream *stream;

	mail->data.save_envelope = TRUE;
	header_ctx = mailbox_header_lookup_init(&mail->ibox->box,
						imap_envelope_headers);
	stream = mail_get_header_stream(&mail->mail.mail, header_ctx);
	if (mail->data.envelope == NULL && stream != NULL) {
		/* we got the headers from cache - parse them to get the
		   envelope */
		message_parse_header(stream, NULL,
				     imap_envelope_parse_callback, mail);
		mail->data.save_envelope = FALSE;
	}
	mailbox_header_lookup_deinit(&header_ctx);
}

static size_t get_header_size(buffer_t *buffer, size_t pos)
{
	const unsigned char *data;
	size_t i, size;

	data = buffer_get_data(buffer, &size);
	i_assert(pos <= size);

	for (i = pos; i < size; i++) {
		if (data[i] == '\n') {
			if (i+1 == size ||
			    (data[i+1] != ' ' && data[i+1] != '\t'))
				return i - pos;
		}
	}
	return size - pos;
}

static int index_mail_header_is_parsed(struct index_mail *mail,
				       unsigned int field_idx)
{
	const uint8_t *match;
	unsigned int count;

	match = array_get(&mail->header_match, &count);
	if (field_idx >= count)
		return -1;

	if (match[field_idx] == mail->header_match_value)
		return 0;
	else if (match[field_idx] == mail->header_match_value + 1)
		return 1;
	return -1;
}

static bool skip_header(const unsigned char **data, size_t len)
{
	const unsigned char *p = *data;
	size_t i;

	for (i = 0; i < len; i++) {
		if (p[i] == ':')
			break;
	}
	if (i == len)
		return FALSE;

	for (i++; i < len; i++) {
		if (!IS_LWSP(p[i]))
			break;
	}

	*data = p + i;
	return TRUE;
}

static const char *const *
index_mail_get_parsed_header(struct index_mail *mail, unsigned int field_idx)
{
	array_t ARRAY_DEFINE(header_values, const char *);
        const struct index_mail_line *lines;
	const unsigned char *header, *value_start, *value_end;
	const unsigned int *line_idx;
	const char *value;
	unsigned int i, lines_count, first_line_idx;

	line_idx = array_idx(&mail->header_match_lines, field_idx);
	i_assert(*line_idx != 0);
	first_line_idx = *line_idx - 1;

	ARRAY_CREATE(&header_values, mail->data_pool, const char *, 4);
	header = buffer_get_data(mail->header_data, NULL);

	lines = array_get(&mail->header_lines, &lines_count);
	for (i = first_line_idx; i < lines_count; i++) {
		if (lines[i].field_idx != lines[first_line_idx].field_idx)
			break;

		/* skip header: and drop ending LF */
		value_start = header + lines[i].start_pos;
		value_end = header + lines[i].end_pos;
		if (skip_header(&value_start, value_end - value_start)) {
			if (value_start != value_end && value_end[-1] == '\n')
				value_end--;
			value = p_strndup(mail->data_pool, value_start,
					  value_end - value_start);
			array_append(&header_values, &value, 1);
		}
	}

	value = NULL;
	array_append(&header_values, &value, sizeof(value));
	return array_idx(&header_values, 0);
}

const char *const *index_mail_get_headers(struct mail *_mail, const char *field)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	const char *headers[2], *value;
	struct mailbox_header_lookup_ctx *headers_ctx;
	unsigned char *data;
	unsigned int field_idx;
	string_t *dest;
	size_t i, len;
	int ret;
	array_t ARRAY_DEFINE(header_values, const char *);

	i_assert(field != NULL);

	field_idx = get_header_field_idx(mail->ibox, field);

	dest = str_new(mail->data_pool, 128);
	if (mail_cache_lookup_headers(mail->trans->cache_view, dest,
				      mail->data.seq, &field_idx, 1) <= 0) {
		/* not in cache / error - first see if it's already parsed */
		p_free(mail->data_pool, dest);
		if (mail->header_seq == mail->data.seq) {
			ret = index_mail_header_is_parsed(mail, field_idx);
			if (ret != -1) {
				return ret == 0 ? NULL :
					index_mail_get_parsed_header(mail,
								     field_idx);
			}
		}

		/* parse */
		headers[0] = field; headers[1] = NULL;
		headers_ctx = mailbox_header_lookup_init(&mail->ibox->box,
							 headers);
		ret = index_mail_parse_headers(mail, headers_ctx);
		mailbox_header_lookup_deinit(&headers_ctx);
		if (ret < 0)
			return NULL;

		ret = index_mail_header_is_parsed(mail, field_idx);
		i_assert(ret != -1);
		return ret == 0 ? NULL :
			index_mail_get_parsed_header(mail, field_idx);
	}
	data = buffer_get_modifyable_data(dest, &len);

	if (len == 0) {
		/* cached as non-existing. */
		return p_new(mail->data_pool, const char *, 1);
	}

	ARRAY_CREATE(&header_values, mail->data_pool, const char *, 4);

	/* cached. skip "header name: " parts in dest. */
	for (i = 0; i < len; i++) {
		if (data[i] == ':') {
			if (i+1 != len && data[++i] == ' ') i++;

			/* @UNSAFE */
			len = get_header_size(dest, i);
			data[i + len] = '\0';
			value = (const char *)data + i;
			i += len + 1;

			array_append(&header_values, &value, sizeof(value));
		}
	}

	value = NULL;
	array_append(&header_values, &value, sizeof(value));
	return array_idx(&header_values, 0);
}

const char *index_mail_get_first_header(struct mail *mail, const char *field)
{
	const char *const *list = index_mail_get_headers(mail, field);

	return list == NULL ? NULL : list[0];
}

static void header_cache_callback(struct message_header_line *hdr,
				  bool *matched, void *context)
{
	struct index_mail *mail = context;

	if (hdr != NULL && hdr->eoh)
		*matched = FALSE;

	index_mail_parse_header(NULL, hdr, mail);
}

struct istream *
index_mail_get_header_stream(struct mail *_mail,
			     struct mailbox_header_lookup_ctx *_headers)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_header_lookup_ctx *headers =
		(struct index_header_lookup_ctx *)_headers;
	string_t *dest;

	if (mail->data.save_bodystructure_header) {
		/* we have to parse the header. */
		if (index_mail_parse_headers(mail, _headers) < 0)
			return NULL;
	}

	dest = str_new(mail->data_pool, 256);
	if (mail_cache_lookup_headers(mail->trans->cache_view, dest,
				      mail->data.seq, headers->idx,
				      headers->count) > 0) {
		if (mail->data.filter_stream != NULL)
			i_stream_destroy(&mail->data.filter_stream);
		mail->data.filter_stream =
			i_stream_create_from_data(default_pool,
						  str_data(dest),
						  str_len(dest));
		return mail->data.filter_stream;
	}
	/* not in cache / error */
	p_free(mail->data_pool, dest);

	if (mail_get_stream(&mail->mail.mail, NULL, NULL) == NULL)
		return NULL;

	if (mail->data.filter_stream != NULL)
		i_stream_destroy(&mail->data.filter_stream);

	index_mail_parse_header_init(mail, _headers);
	mail->data.filter_stream =
		i_stream_create_header_filter(mail->data.stream,
					      HEADER_FILTER_INCLUDE |
					      HEADER_FILTER_HIDE_BODY,
					      headers->name, headers->count,
					      header_cache_callback, mail);
	return mail->data.filter_stream;
}

struct mailbox_header_lookup_ctx *
index_header_lookup_init(struct mailbox *box, const char *const headers[])
{
	struct index_mailbox *ibox = (struct index_mailbox *)box;
	struct mail_cache_field *fields, header_field = {
		NULL, 0, MAIL_CACHE_FIELD_HEADER, 0,
		MAIL_CACHE_DECISION_TEMP
	};
	struct index_header_lookup_ctx *ctx;
	const char *const *name;
	const char **sorted_headers;
	pool_t pool;
	unsigned int i, count;

	i_assert(*headers != NULL);

	for (count = 0, name = headers; *name != NULL; name++)
		count++;

	t_push();

	/* @UNSAFE: headers need to be sorted for filter stream. */
	sorted_headers = t_new(const char *, count);
	memcpy(sorted_headers, headers, count * sizeof(*sorted_headers));
	qsort(sorted_headers, count, sizeof(*sorted_headers), strcasecmp_p);
	headers = sorted_headers;

	/* @UNSAFE */
	fields = t_new(struct mail_cache_field, count);
	for (i = 0; i < count; i++) {
		header_field.name = t_strconcat("hdr.", headers[i], NULL);
		fields[i] = header_field;
	}
	mail_cache_register_fields(ibox->cache, fields, count);

	pool = pool_alloconly_create("index_header_lookup_ctx", 512);
	ctx = p_new(pool, struct index_header_lookup_ctx, 1);
	ctx->ctx.box = box;
	ctx->pool = pool;
	ctx->count = count;

	ctx->idx = p_new(pool, unsigned int, count);
	ctx->name = p_new(pool, const char *, count);

	/* @UNSAFE */
	for (i = 0; i < count; i++) {
		ctx->idx[i] = fields[i].idx;
		ctx->name[i] = p_strdup(pool, headers[i]);
	}

	t_pop();
	return &ctx->ctx;
}

void index_header_lookup_deinit(struct mailbox_header_lookup_ctx *_ctx)
{
	struct index_header_lookup_ctx *ctx =
		(struct index_header_lookup_ctx *)_ctx;

	pool_unref(ctx->pool);
}
