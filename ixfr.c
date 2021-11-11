/*
 * ixfr.c -- generating IXFR responses.
 *
 * Copyright (c) 2021, NLnet Labs. All rights reserved.
 *
 * See LICENSE for the license.
 *
 */

#include "config.h"

#include <errno.h>
#include <string.h>
#include <ctype.h>
#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#  include <sys/stat.h>
#endif
#include <unistd.h>

#include "ixfr.h"
#include "packet.h"
#include "rdata.h"
#include "axfr.h"
#include "options.h"
#include "zonec.h"

/*
 * For optimal compression IXFR response packets are limited in size
 * to MAX_COMPRESSION_OFFSET.
 */
#define IXFR_MAX_MESSAGE_LEN MAX_COMPRESSION_OFFSET

/* draft-ietf-dnsop-rfc2845bis-06, section 5.3.1 says to sign every packet */
#define IXFR_TSIG_SIGN_EVERY_NTH	0	/* tsig sign every N packets. */

/* initial space in rrs data for storing records */
#define IXFR_STORE_INITIAL_SIZE 4096

/* parse the serial number from the IXFR query */
static int parse_qserial(struct buffer* packet, uint32_t* qserial,
	size_t* snip_pos)
{
	unsigned int i;
	uint16_t type, rdlen;
	/* we must have a SOA in the authority section */
	if(NSCOUNT(packet) == 0)
		return 0;
	/* skip over the question section, we want only one */
	buffer_set_position(packet, QHEADERSZ);
	if(QDCOUNT(packet) != 1)
		return 0;
	if(!packet_skip_rr(packet, 1))
		return 0;
	/* set position to snip off the authority section */
	*snip_pos = buffer_position(packet);
	/* skip over the authority section RRs until we find the SOA */
	for(i=0; i<NSCOUNT(packet); i++) {
		/* is this the SOA record? */
		if(!packet_skip_dname(packet))
			return 0; /* malformed name */
		if(!buffer_available(packet, 10))
			return 0; /* no type,class,ttl,rdatalen */
		type = buffer_read_u16(packet);
		buffer_skip(packet, 6);
		rdlen = buffer_read_u16(packet);
		if(!buffer_available(packet, rdlen))
			return 0;
		if(type == TYPE_SOA) {
			/* read serial from rdata, skip two dnames, then
			 * read the 32bit value */
			if(!packet_skip_dname(packet))
				return 0; /* malformed nsname */
			if(!packet_skip_dname(packet))
				return 0; /* malformed rname */
			if(!buffer_available(packet, 4))
				return 0;
			*qserial = buffer_read_u32(packet);
			return 1;
		}
		buffer_skip(packet, rdlen);
	}
	return 0;
}

/* get serial from SOA RR */
static uint32_t soa_rr_get_serial(struct rr* rr)
{
	if(rr->rdata_count < 3)
		return 0;
	if(rr->rdatas[2].data[0] < 4)
		return 0;
	return read_uint32(&rr->rdatas[2].data[1]);
}

/* get the current serial from the zone */
static uint32_t zone_get_current_serial(struct zone* zone)
{
	if(!zone || !zone->soa_rrset)
		return 0;
	if(zone->soa_rrset->rr_count == 0)
		return 0;
	if(zone->soa_rrset->rrs[0].rdata_count < 3)
		return 0;
	if(zone->soa_rrset->rrs[0].rdatas[2].data[0] < 4)
		return 0;
	return read_uint32(&zone->soa_rrset->rrs[0].rdatas[2].data[1]);
}

/* connect IXFRs, return true if connected, false if not. Return last serial */
static int connect_ixfrs(struct ixfr_data* data, uint32_t* end_serial)
{
	struct rbnode* p = &data->node;
	while(p && p != RBTREE_NULL) {
		struct rbnode* next = rbtree_next(p);
		struct ixfr_data* pdata = (struct ixfr_data*)p;
		if(next && next != RBTREE_NULL) {
			struct ixfr_data* n = (struct ixfr_data*)next;
			if(pdata->newserial != n->oldserial) {
				/* These ixfrs are not connected,
				 * during IXFR processing that could already
				 * have been deleted, but we check here
				 * in any case */
				return 0;
			}
		} else {
			/* the chain of IXFRs ends in this serial number */
			*end_serial = pdata->newserial;
		}
		p = next;
	}
	return 1;
}

/* Count length of next record in data */
static size_t count_rr_length(uint8_t* data, size_t data_len, size_t current)
{
	uint8_t label_size;
	uint16_t rdlen;
	size_t i = current;
	if(current >= data_len)
		return 0;
	/* pass the owner dname */
	while(1) {
		if(i+1 > data_len)
			return 0;
		label_size = data[i++];
		if(label_size == 0) {
			break;
		} else if((label_size &0xc0) != 0) {
			return 0; /* uncompressed dnames in IXFR store */
		} else if(i+label_size > data_len) {
			return 0;
		} else {
			i += label_size;
		}
	}
	/* after dname, we pass type, class, ttl, rdatalen */
	if(i+10 > data_len)
		return 0;
	i += 8;
	rdlen = read_uint16(data+i);
	i += 2;
	/* pass over the rdata */
	if(i+((size_t)rdlen) > data_len)
		return 0;
	i += ((size_t)rdlen);
	return i-current;
}

/* Copy RRs into packet until packet full, return number RRs added */
static uint16_t ixfr_copy_rrs_into_packet(struct query* query)
{
	uint16_t total_added = 0;

	/* Copy RRs into the packet until the answer is full,
	 * when an RR does not fit, we return and add no more. */

	/* Add first SOA */
	if(query->ixfr_count_newsoa < query->ixfr_end_data->newsoa_len) {
		/* the new SOA is added from the end_data segment, it is
		 * the final SOA of the result of the IXFR */
		if(buffer_position(query->packet) < query->maxlen &&
			buffer_position(query->packet) +
			query->ixfr_end_data->newsoa_len < query->maxlen) {
			buffer_write(query->packet, query->ixfr_end_data->newsoa,
				query->ixfr_end_data->newsoa_len);
			query->ixfr_count_newsoa = query->ixfr_end_data->newsoa_len;
			total_added++;
			query->ixfr_pos_of_newsoa = buffer_position(query->packet);
		} else {
			/* cannot add another RR, so return */
			return total_added;
		}
	}

	/* Add second SOA */
	if(query->ixfr_count_oldsoa < query->ixfr_data->oldsoa_len) {
		if(buffer_position(query->packet) < query->maxlen &&
			buffer_position(query->packet) +
			query->ixfr_data->oldsoa_len < query->maxlen) {
			buffer_write(query->packet, query->ixfr_data->oldsoa,
				query->ixfr_data->oldsoa_len);
			query->ixfr_count_oldsoa = query->ixfr_data->oldsoa_len;
			total_added++;
		} else {
			/* cannot add another RR, so return */
			return total_added;
		}
	}

	/* Add del data, with deleted RRs and a SOA */
	while(query->ixfr_count_del < query->ixfr_data->del_len) {
		size_t rrlen = count_rr_length(query->ixfr_data->del,
			query->ixfr_data->del_len, query->ixfr_count_del);
		if(rrlen && buffer_position(query->packet) < query->maxlen &&
			buffer_position(query->packet) + rrlen <
			query->maxlen) {
			buffer_write(query->packet, query->ixfr_data->del +
				query->ixfr_count_del, rrlen);
			query->ixfr_count_del += rrlen;
			total_added++;
		} else {
			/* the next record does not fit in the remaining
			 * space of the packet */
			return total_added;
		}
	}

	/* Add add data, with added RRs and a SOA */
	while(query->ixfr_count_add < query->ixfr_data->add_len) {
		size_t rrlen = count_rr_length(query->ixfr_data->add,
			query->ixfr_data->add_len, query->ixfr_count_add);
		if(rrlen && buffer_position(query->packet) < query->maxlen &&
			buffer_position(query->packet) + rrlen <
			query->maxlen) {
			buffer_write(query->packet, query->ixfr_data->add +
				query->ixfr_count_add, rrlen);
			query->ixfr_count_add += rrlen;
			total_added++;
		} else {
			/* the next record does not fit in the remaining
			 * space of the packet */
			return total_added;
		}
	}
	return total_added;
}

query_state_type query_ixfr(struct nsd *nsd, struct query *query)
{
	uint16_t total_added = 0;

	if (query->ixfr_is_done)
		return QUERY_PROCESSED;

	if (query->maxlen > IXFR_MAX_MESSAGE_LEN)
		query->maxlen = IXFR_MAX_MESSAGE_LEN;

	assert(!query_overflow(query));
	/* only keep running values for most packets */
	query->tsig_prepare_it = 0;
	query->tsig_update_it = 1;
	if(query->tsig_sign_it) {
		/* prepare for next updates */
		query->tsig_prepare_it = 1;
		query->tsig_sign_it = 0;
	}

	if (query->ixfr_data == NULL) {
		/* This is the first packet, process the query further */
		uint32_t qserial = 0, current_serial = 0, end_serial = 0;
		struct zone* zone;
		struct ixfr_data* ixfr_data;
		size_t oldpos;

		/* parse the serial number from the IXFR request */
		oldpos = QHEADERSZ;
		if(!parse_qserial(query->packet, &qserial, &oldpos)) {
			NSCOUNT_SET(query->packet, 0);
			ARCOUNT_SET(query->packet, 0);
			buffer_set_position(query->packet, oldpos);
			RCODE_SET(query->packet, RCODE_FORMAT);
			return QUERY_PROCESSED;
		}
		NSCOUNT_SET(query->packet, 0);
		ARCOUNT_SET(query->packet, 0);
		buffer_set_position(query->packet, oldpos);
		DEBUG(DEBUG_XFRD,1, (LOG_INFO, "ixfr query routine, %s IXFR=%u",
			dname_to_string(query->qname, NULL), (unsigned)qserial));

		/* do we have an IXFR with this serial number? If not, serve AXFR */
		zone = namedb_find_zone(nsd->db, query->qname);
		if(!zone) {
			/* no zone is present */
			RCODE_SET(query->packet, RCODE_NOTAUTH);
			return QUERY_PROCESSED;
		}

		/* if the query is for same or newer serial than our current
		 * serial, then serve a single SOA with our current serial */
		current_serial = zone_get_current_serial(zone);
		if(compare_serial(qserial, current_serial) >= 0) {
			if(!zone->soa_rrset || zone->soa_rrset->rr_count != 1){
				RCODE_SET(query->packet, RCODE_SERVFAIL);
				return QUERY_PROCESSED;
			}
			query_add_compression_domain(query, zone->apex,
				QHEADERSZ);
			if(packet_encode_rr(query, zone->apex,
				&zone->soa_rrset->rrs[0],
				zone->soa_rrset->rrs[0].ttl)) {
				ANCOUNT_SET(query->packet, 1);
			} else {
				RCODE_SET(query->packet, RCODE_SERVFAIL);
			}
			AA_SET(query->packet);
			query_clear_compression_tables(query);
			return QUERY_PROCESSED;
		}

		if(!zone->ixfr) {
			/* we have no ixfr information for the zone, make an AXFR */
			return query_axfr(nsd, query);
		}
		ixfr_data = zone_ixfr_find_serial(zone->ixfr, qserial);
		if(!ixfr_data) {
			/* the specific version is not available, make an AXFR */
			return query_axfr(nsd, query);
		}
		/* see if the IXFRs connect to the next IXFR, and if it ends
		 * at the current served zone, if not, AXFR */
		if(!connect_ixfrs(ixfr_data, &end_serial) ||
			end_serial != current_serial) {
			return query_axfr(nsd, query);
		}

		query->ixfr_data = ixfr_data;
		query->ixfr_is_done = 0;
		/* set up to copy the last version's SOA as first SOA */
		query->ixfr_end_data = (struct ixfr_data*)rbtree_last(
			zone->ixfr->data);
		query->ixfr_count_newsoa = 0;
		query->ixfr_count_oldsoa = 0;
		query->ixfr_count_del = 0;
		query->ixfr_count_add = 0;
		query->ixfr_pos_of_newsoa = 0;
		if(query->tsig.status == TSIG_OK) {
			query->tsig_sign_it = 1; /* sign first packet in stream */
		}
	} else {
		/*
		 * Query name need not be repeated after the
		 * first response packet.
		 */
		buffer_set_limit(query->packet, QHEADERSZ);
		QDCOUNT_SET(query->packet, 0);
		query_prepare_response(query);
	}

	total_added = ixfr_copy_rrs_into_packet(query);

	while(query->ixfr_count_add >= query->ixfr_data->add_len) {
		struct rbnode* nextdata = rbtree_next(&query->ixfr_data->node);
		/* finished the ixfr_data */
		if(nextdata && nextdata != RBTREE_NULL) {
			struct ixfr_data* n = (struct ixfr_data*)nextdata;
			/* move to the next IXFR */
			query->ixfr_data = n;
			/* we need to skip the SOA records, set len to done*/
			/* the newsoa count is already done, at end_data len */
			query->ixfr_count_oldsoa = n->oldsoa_len;
			/* and then set up to copy the del and add sections */
			query->ixfr_count_del = 0;
			query->ixfr_count_add = 0;
			total_added += ixfr_copy_rrs_into_packet(query);
		} else {
			/* we finished the IXFR */
			/* sign the last packet */
			query->tsig_sign_it = 1;
			query->ixfr_is_done = 1;
			break;
		}
	}

	/* return the answer */
	AA_SET(query->packet);
	ANCOUNT_SET(query->packet, total_added);
	NSCOUNT_SET(query->packet, 0);
	ARCOUNT_SET(query->packet, 0);

	if(!query->tcp && !query->ixfr_is_done) {
		TC_SET(query->packet);
		if(query->ixfr_pos_of_newsoa) {
			/* if we recorded the newsoa in the result, snip off
			 * the rest of the response, the RFC1995 response for
			 * when it does not fit is only the latest SOA */
			buffer_set_position(query->packet, query->ixfr_pos_of_newsoa);
			ANCOUNT_SET(query->packet, 1);
		}
		query->ixfr_is_done = 1;
	}

	/* check if it needs tsig signatures */
	if(query->tsig.status == TSIG_OK) {
#if IXFR_TSIG_SIGN_EVERY_NTH > 0
		if(query->tsig.updates_since_last_prepare >= IXFR_TSIG_SIGN_EVERY_NTH) {
#endif
			query->tsig_sign_it = 1;
#if IXFR_TSIG_SIGN_EVERY_NTH > 0
		}
#endif
	}
	return QUERY_IN_IXFR;
}

/* free ixfr_data structure */
static void ixfr_data_free(struct ixfr_data* data)
{
	if(!data)
		return;
	free(data->newsoa);
	free(data->oldsoa);
	free(data->del);
	free(data->add);
	free(data->log_str);
	free(data);
}

/* size of the ixfr data */
static size_t ixfr_data_size(struct ixfr_data* data)
{
	return sizeof(struct ixfr_data) + data->newsoa_len + data->oldsoa_len
		+ data->del_len + data->add_len;
}

struct ixfr_store* ixfr_store_start(struct zone* zone,
	struct ixfr_store* ixfr_store_mem, uint32_t old_serial,
	uint32_t new_serial)
{
	struct ixfr_store* ixfr_store = ixfr_store_mem;
	memset(ixfr_store, 0, sizeof(*ixfr_store));
	ixfr_store->zone = zone;
	ixfr_store->data = xalloc_zero(sizeof(*ixfr_store->data));
	ixfr_store->data->oldserial = old_serial;
	ixfr_store->data->newserial = new_serial;
	return ixfr_store;
}

void ixfr_store_cancel(struct ixfr_store* ixfr_store)
{
	ixfr_store->cancelled = 1;
	ixfr_data_free(ixfr_store->data);
	ixfr_store->data = NULL;
}

void ixfr_store_free(struct ixfr_store* ixfr_store)
{
	if(!ixfr_store)
		return;
	ixfr_data_free(ixfr_store->data);
}

/* make space in record data for the new size, grows the allocation */
static void ixfr_rrs_make_space(uint8_t** rrs, size_t* len, size_t* capacity,
	size_t added)
{
	size_t newsize = 0;
	if(*rrs == NULL) {
		newsize = IXFR_STORE_INITIAL_SIZE;
	} else {
		if(*len + added <= *capacity)
			return; /* already enough space */
		newsize = (*capacity)*2;
	}
	if(*len + added > newsize)
		newsize = *len + added;
	if(*rrs == NULL) {
		*rrs = xalloc(newsize);
	} else {
		*rrs = xrealloc(*rrs, newsize);
	}
	*capacity = newsize;
}

/* put new SOA record after delrrs and addrrs */
static void ixfr_put_newsoa(struct ixfr_store* ixfr_store, uint8_t** rrs,
	size_t* len, size_t* capacity)
{
	uint8_t* soa = ixfr_store->data->newsoa;
	size_t soa_len = ixfr_store->data->newsoa_len;
	ixfr_rrs_make_space(rrs, len, capacity, soa_len);
	if(!*rrs || *len + soa_len > *capacity) {
		log_msg(LOG_ERR, "ixfr_store addrr: cannot allocate space");
		ixfr_store_cancel(ixfr_store);
		return;
	}
	memmove(*rrs + *len, soa, soa_len);
	*len += soa_len;
}

/* trim unused storage from the rrs data */
static void ixfr_trim_capacity(uint8_t** rrs, size_t* len, size_t* capacity)
{
	if(*rrs == NULL)
		return;
	if(*capacity == *len)
		return;
	*rrs = xrealloc(*rrs, *len);
	*capacity = *len;
}

void ixfr_store_finish(struct ixfr_store* ixfr_store, struct nsd* nsd,
	char* log_buf, uint64_t time_start_0, uint32_t time_start_1,
	uint64_t time_end_0, uint32_t time_end_1)
{
	if(ixfr_store->cancelled) {
		ixfr_store_free(ixfr_store);
		return;
	}

	/* put new serial SOA record after delrrs and addrrs */
	ixfr_put_newsoa(ixfr_store, &ixfr_store->data->del,
		&ixfr_store->data->del_len, &ixfr_store->del_capacity);
	ixfr_put_newsoa(ixfr_store, &ixfr_store->data->add,
		&ixfr_store->data->add_len, &ixfr_store->add_capacity);

	/* trim the data in the store, the overhead from capacity is
	 * removed */
	ixfr_trim_capacity(&ixfr_store->data->del,
		&ixfr_store->data->del_len, &ixfr_store->del_capacity);
	ixfr_trim_capacity(&ixfr_store->data->add,
		&ixfr_store->data->add_len, &ixfr_store->add_capacity);

	if(ixfr_store->cancelled) {
		ixfr_store_free(ixfr_store);
		return;
	}

	if(log_buf)
		ixfr_store->data->log_str = strdup(log_buf);

	/* store the data in the zone */
	if(!ixfr_store->zone->ixfr)
		ixfr_store->zone->ixfr = zone_ixfr_create(nsd);
	zone_ixfr_make_space(ixfr_store->zone->ixfr, ixfr_store->zone,
		ixfr_store->data, ixfr_store);
	if(ixfr_store->cancelled) {
		ixfr_store_free(ixfr_store);
		return;
	}
	zone_ixfr_add(ixfr_store->zone->ixfr, ixfr_store->data);
	ixfr_store->data = NULL;

	(void)time_start_0;
	(void)time_start_1;
	(void)time_end_0;
	(void)time_end_1;

	/* free structure */
	ixfr_store_free(ixfr_store);
}

/* read SOA rdata section for SOA storage */
static int read_soa_rdata(struct buffer* packet, uint8_t* primns,
	int* primns_len, uint8_t* email, int* email_len,
	uint32_t* serial, uint32_t* refresh, uint32_t* retry,
	uint32_t* expire, uint32_t* minimum, size_t* sz)
{
	if(!(*primns_len = dname_make_wire_from_packet(primns, packet, 1))) {
		log_msg(LOG_ERR, "ixfr_store: cannot parse soa nsname in packet");
		return 0;
	}
	*sz += *primns_len;
	if(!(*email_len = dname_make_wire_from_packet(email, packet, 1))) {
		log_msg(LOG_ERR, "ixfr_store: cannot parse soa maintname in packet");
		return 0;
	}
	*sz += *email_len;
	*serial = buffer_read_u32(packet);
	*sz += 4;
	*refresh = buffer_read_u32(packet);
	*sz += 4;
	*retry = buffer_read_u32(packet);
	*sz += 4;
	*expire = buffer_read_u32(packet);
	*sz += 4;
	*minimum = buffer_read_u32(packet);
	*sz += 4;
	return 1;
}

/* store SOA record data in memory buffer */
static void store_soa(uint8_t* soa, struct zone* zone, uint32_t ttl,
	uint16_t rdlen_uncompressed, uint8_t* primns, int primns_len,
	uint8_t* email, int email_len, uint32_t serial, uint32_t refresh,
	uint32_t retry, uint32_t expire, uint32_t minimum)
{
	uint8_t* sp = soa;
	memmove(sp, dname_name(domain_dname(zone->apex)),
		domain_dname(zone->apex)->name_size);
	sp += domain_dname(zone->apex)->name_size;
	write_uint16(sp, TYPE_SOA);
	sp += 2;
	write_uint16(sp, CLASS_IN);
	sp += 2;
	write_uint32(sp, ttl);
	sp += 4;
	write_uint16(sp, rdlen_uncompressed);
	sp += 2;
	memmove(sp, primns, primns_len);
	sp += primns_len;
	memmove(sp, email, email_len);
	sp += email_len;
	write_uint32(sp, serial);
	sp += 4;
	write_uint32(sp, refresh);
	sp += 4;
	write_uint32(sp, retry);
	sp += 4;
	write_uint32(sp, expire);
	sp += 4;
	write_uint32(sp, minimum);
}

void ixfr_store_add_newsoa(struct ixfr_store* ixfr_store,
	struct buffer* packet, size_t ttlpos)
{
	size_t oldpos, sz = 0;
	uint32_t ttl, serial, refresh, retry, expire, minimum;
	uint16_t rdlen_uncompressed, rdlen_wire;
	int primns_len = 0, email_len = 0;
	uint8_t primns[MAXDOMAINLEN + 1], email[MAXDOMAINLEN + 1];

	if(ixfr_store->cancelled)
		return;
	if(ixfr_store->data->newsoa) {
		free(ixfr_store->data->newsoa);
		ixfr_store->data->newsoa = NULL;
		ixfr_store->data->newsoa_len = 0;
	}
	oldpos = buffer_position(packet);
	buffer_set_position(packet, ttlpos);

	/* calculate the length */
	sz = domain_dname(ixfr_store->zone->apex)->name_size;
	sz += 2 /* type */ + 2 /* class */;
	/* read ttl */
	if(!buffer_available(packet, 4/*ttl*/+2/*rdlen*/)) {
		/* not possible already parsed, but fail nicely anyway */
		log_msg(LOG_ERR, "ixfr_store: not enough space in packet");
		ixfr_store_cancel(ixfr_store);
		buffer_set_position(packet, oldpos);
		return;
	}
	ttl = buffer_read_u32(packet);
	sz += 4;
	rdlen_wire = buffer_read_u16(packet);
	sz += 2;
	if(!buffer_available(packet, rdlen_wire)) {
		/* not possible already parsed, but fail nicely anyway */
		log_msg(LOG_ERR, "ixfr_store: not enough rdata space in packet");
		ixfr_store_cancel(ixfr_store);
		buffer_set_position(packet, oldpos);
		return;
	}
	if(!read_soa_rdata(packet, primns, &primns_len, email, &email_len,
		&serial, &refresh, &retry, &expire, &minimum, &sz)) {
		log_msg(LOG_ERR, "ixfr_store newsoa: cannot parse packet");
		ixfr_store_cancel(ixfr_store);
		buffer_set_position(packet, oldpos);
		return;
	}
	rdlen_uncompressed = primns_len + email_len + 4 + 4 + 4 + 4 + 4;

	/* store the soa record */
	ixfr_store->data->newsoa = xalloc(sz);
	ixfr_store->data->newsoa_len = sz;
	store_soa(ixfr_store->data->newsoa, ixfr_store->zone, ttl,
		rdlen_uncompressed, primns, primns_len, email, email_len,
		serial, refresh, retry, expire, minimum);

	buffer_set_position(packet, oldpos);
}

void ixfr_store_add_oldsoa(struct ixfr_store* ixfr_store, uint32_t ttl,
	struct buffer* packet, size_t rrlen)
{
	size_t oldpos, sz = 0;
	uint32_t serial, refresh, retry, expire, minimum;
	uint16_t rdlen_uncompressed;
	int primns_len = 0, email_len = 0;
	uint8_t primns[MAXDOMAINLEN + 1], email[MAXDOMAINLEN + 1];

	if(ixfr_store->cancelled)
		return;
	if(ixfr_store->data->oldsoa) {
		free(ixfr_store->data->oldsoa);
		ixfr_store->data->oldsoa = NULL;
		ixfr_store->data->oldsoa_len = 0;
	}
	/* we have the old SOA and thus we are sure it is an IXFR, make space*/
	zone_ixfr_make_space(ixfr_store->zone->ixfr, ixfr_store->zone,
		ixfr_store->data, ixfr_store);
	if(ixfr_store->cancelled)
		return;
	oldpos = buffer_position(packet);

	/* calculate the length */
	sz = domain_dname(ixfr_store->zone->apex)->name_size;
	sz += 2 /*type*/ + 2 /*class*/ + 4 /*ttl*/ + 2 /*rdlen*/;
	if(!buffer_available(packet, rrlen)) {
		/* not possible already parsed, but fail nicely anyway */
		log_msg(LOG_ERR, "ixfr_store oldsoa: not enough rdata space in packet");
		ixfr_store_cancel(ixfr_store);
		buffer_set_position(packet, oldpos);
		return;
	}
	if(!read_soa_rdata(packet, primns, &primns_len, email, &email_len,
		&serial, &refresh, &retry, &expire, &minimum, &sz)) {
		log_msg(LOG_ERR, "ixfr_store oldsoa: cannot parse packet");
		ixfr_store_cancel(ixfr_store);
		buffer_set_position(packet, oldpos);
		return;
	}
	rdlen_uncompressed = primns_len + email_len + 4 + 4 + 4 + 4 + 4;

	/* store the soa record */
	ixfr_store->data->oldsoa = xalloc(sz);
	ixfr_store->data->oldsoa_len = sz;
	store_soa(ixfr_store->data->oldsoa, ixfr_store->zone, ttl,
		rdlen_uncompressed, primns, primns_len, email, email_len,
		serial, refresh, retry, expire, minimum);

	buffer_set_position(packet, oldpos);
}

/* store RR in data segment */
static int ixfr_putrr(const struct dname* dname, uint16_t type, uint16_t klass,
	uint32_t ttl, rdata_atom_type* rdatas, ssize_t rdata_num,
	uint8_t** rrs, size_t* rrs_len, size_t* rrs_capacity)
{
	size_t rdlen_uncompressed, sz;
	uint8_t* sp;
	int i;

	/* find rdatalen */
	rdlen_uncompressed = 0;
	for(i=0; i<rdata_num; i++) {
		if(rdata_atom_is_domain(type, i)) {
			rdlen_uncompressed += domain_dname(rdatas[i].domain)
				->name_size;
		} else {
			rdlen_uncompressed += rdatas[i].data[0];
		}
	}
	sz = dname->name_size + 2 /*type*/ + 2 /*class*/ + 4 /*ttl*/ +
		2 /*rdlen*/ + rdlen_uncompressed;

	/* store RR in IXFR data */
	ixfr_rrs_make_space(rrs, rrs_len, rrs_capacity, sz);
	if(!*rrs || *rrs_len + sz > *rrs_capacity) {
		return 0;
	}
	/* copy data into add */
	sp = *rrs + *rrs_len;
	*rrs_len += sz;
	memmove(sp, dname_name(dname), dname->name_size);
	sp += dname->name_size;
	write_uint16(sp, type);
	sp += 2;
	write_uint16(sp, klass);
	sp += 2;
	write_uint32(sp, ttl);
	sp += 4;
	write_uint16(sp, rdlen_uncompressed);
	sp += 2;
	for(i=0; i<rdata_num; i++) {
		if(rdata_atom_is_domain(type, i)) {
			memmove(sp, dname_name(domain_dname(rdatas[i].domain)),
				domain_dname(rdatas[i].domain)->name_size);
			sp += domain_dname(rdatas[i].domain)->name_size;
		} else {
			memmove(sp, &rdatas[i].data[1], rdatas[i].data[0]);
			sp += rdatas[i].data[0];
		}
	}
	return 1;
}

void ixfr_store_putrr(struct ixfr_store* ixfr_store, const struct dname* dname,
	uint16_t type, uint16_t klass, uint32_t ttl, struct buffer* packet,
	uint16_t rrlen, struct region* temp_region, uint8_t** rrs,
	size_t* rrs_len, size_t* rrs_capacity)
{
	domain_table_type *temptable;
	rdata_atom_type *rdatas;
	ssize_t rdata_num;
	size_t oldpos;

	if(ixfr_store->cancelled)
		return;

	/* The SOA data is stored with separate calls. And then appended
	 * during the finish operation. We do not have to store it here
	 * when called from difffile's IXFR processing with type SOA. */
	if(type == TYPE_SOA)
		return;
	/* make space for these RRs we have now; basically once we
	 * grow beyond the current allowed amount an older IXFR is deleted. */
	zone_ixfr_make_space(ixfr_store->zone->ixfr, ixfr_store->zone,
		ixfr_store->data, ixfr_store);
	if(ixfr_store->cancelled)
		return;

	/* parse rdata */
	oldpos = buffer_position(packet);
	temptable = domain_table_create(temp_region);
	rdata_num = rdata_wireformat_to_rdata_atoms(temp_region, temptable,
		type, rrlen, packet, &rdatas);
	buffer_set_position(packet, oldpos);
	if(rdata_num == -1) {
		log_msg(LOG_ERR, "ixfr_store addrr: cannot parse packet");
		ixfr_store_cancel(ixfr_store);
		return;
	}

	if(!ixfr_putrr(dname, type, klass, ttl, rdatas, rdata_num,
		rrs, rrs_len, rrs_capacity)) {
		log_msg(LOG_ERR, "ixfr_store addrr: cannot allocate space");
		ixfr_store_cancel(ixfr_store);
		return;
	}
}

void ixfr_store_delrr(struct ixfr_store* ixfr_store, const struct dname* dname,
	uint16_t type, uint16_t klass, uint32_t ttl, struct buffer* packet,
	uint16_t rrlen, struct region* temp_region)
{
	ixfr_store_putrr(ixfr_store, dname, type, klass, ttl, packet, rrlen,
		temp_region, &ixfr_store->data->del,
		&ixfr_store->data->del_len, &ixfr_store->del_capacity);
}

void ixfr_store_addrr(struct ixfr_store* ixfr_store, const struct dname* dname,
	uint16_t type, uint16_t klass, uint32_t ttl, struct buffer* packet,
	uint16_t rrlen, struct region* temp_region)
{
	ixfr_store_putrr(ixfr_store, dname, type, klass, ttl, packet, rrlen,
		temp_region, &ixfr_store->data->add,
		&ixfr_store->data->add_len, &ixfr_store->add_capacity);
}

int zone_is_ixfr_enabled(struct zone* zone)
{
	return zone->opts->pattern->store_ixfr;
}

/* compare ixfr elements */
static int ixfrcompare(const void* x, const void* y)
{
	uint32_t* serial_x = (uint32_t*)x;
	uint32_t* serial_y = (uint32_t*)y;
	if(*serial_x < *serial_y)
		return -1;
	if(*serial_x > *serial_y)
		return 1;
	return 0;
}

struct zone_ixfr* zone_ixfr_create(struct nsd* nsd)
{
	struct zone_ixfr* ixfr = xalloc_zero(sizeof(struct zone_ixfr));
	ixfr->data = rbtree_create(nsd->region, &ixfrcompare);
	return ixfr;
}

/* traverse tree postorder */
static void ixfr_tree_del(struct rbnode* node)
{
	if(node == NULL || node == RBTREE_NULL)
		return;
	ixfr_tree_del(node->left);
	ixfr_tree_del(node->right);
	ixfr_data_free((struct ixfr_data*)node);
}

/* clear the ixfr data elements */
static void zone_ixfr_clear(struct zone_ixfr* ixfr)
{
	if(!ixfr)
		return;
	if(ixfr->data) {
		ixfr_tree_del(ixfr->data->root);
		ixfr->data->root = RBTREE_NULL;
		ixfr->data->count = 0;
	}
	ixfr->total_size = 0;
}

void zone_ixfr_free(struct zone_ixfr* ixfr)
{
	if(!ixfr)
		return;
	if(ixfr->data) {
		ixfr_tree_del(ixfr->data->root);
		ixfr->data = NULL;
	}
	free(ixfr);
}

/* remove the oldest data entry from the ixfr versions */
static void zone_ixfr_remove_oldest(struct zone_ixfr* ixfr)
{
	if(ixfr->data->count > 0) {
		struct ixfr_data* oldest = (struct ixfr_data*)rbtree_first(
			ixfr->data);
		zone_ixfr_remove(ixfr, oldest);
	}
}

void zone_ixfr_make_space(struct zone_ixfr* ixfr, struct zone* zone,
	struct ixfr_data* data, struct ixfr_store* ixfr_store)
{
	size_t addsize;
	if(!ixfr || !data)
		return;
	if(zone->opts->pattern->ixfr_number == 0) {
		ixfr_store_cancel(ixfr_store);
		return;
	}

	/* Check the number of IXFRs allowed for this zone, if too many,
	 * shorten the number to make space for another one */
	while(ixfr->data->count >= zone->opts->pattern->ixfr_number) {
		zone_ixfr_remove_oldest(ixfr);
	}

	if(zone->opts->pattern->ixfr_size == 0) {
		/* no size limits imposed */
		return;
	}

	/* Check the size of the current added data element 'data', and
	 * see if that overflows the maximum storage size for IXFRs for
	 * this zone, and if so, delete the oldest IXFR to make space */
	addsize = ixfr_data_size(data);
	while(ixfr->data->count > 0 && ixfr->total_size + addsize >
		zone->opts->pattern->ixfr_size) {
		zone_ixfr_remove_oldest(ixfr);
	}

	/* if deleting the oldest elements does not work, then this
	 * IXFR is too big to store and we cancel it */
	if(ixfr->data->count == 0 && ixfr->total_size + addsize >
		zone->opts->pattern->ixfr_size) {
		ixfr_store_cancel(ixfr_store);
		return;
	}
}

void zone_ixfr_remove(struct zone_ixfr* ixfr, struct ixfr_data* data)
{
	rbtree_delete(ixfr->data, &data->node.key);
	ixfr->total_size -= ixfr_data_size(data);
	ixfr_data_free(data);
}

void zone_ixfr_add(struct zone_ixfr* ixfr, struct ixfr_data* data)
{
	memset(&data->node, 0, sizeof(data->node));
	data->node.key = &data->oldserial;
	rbtree_insert(ixfr->data, &data->node);
	ixfr->total_size += ixfr_data_size(data);
}

struct ixfr_data* zone_ixfr_find_serial(struct zone_ixfr* ixfr,
	uint32_t qserial)
{
	struct ixfr_data* data;
	if(!ixfr)
		return NULL;
	if(!ixfr->data)
		return NULL;
	data = (struct ixfr_data*)rbtree_search(ixfr->data, &qserial);
	if(data) {
		assert(data->oldserial == qserial);
		return data;
	}
	/* not found */
	return NULL;
}

/* calculate the number of files we want */
static int ixfr_target_number_files(struct zone* zone)
{
	int dest_num_files;
	if(!zone->ixfr || !zone->ixfr->data)
		return 0;
	if(!zone_is_ixfr_enabled(zone))
		return 0;
	/* if we store ixfr, it is the configured number of files */
	dest_num_files = (int)zone->opts->pattern->ixfr_number;
	/* but if the number of available transfers is smaller, store less */
	if(dest_num_files > (int)zone->ixfr->data->count)
		dest_num_files = (int)zone->ixfr->data->count;
	return dest_num_files;
}

/* create ixfrfile name in buffer for file_num. The num is 1 .. number. */
static void make_ixfr_name(char* buf, size_t len, const char* zfile,
	int file_num)
{
	if(file_num == 1)
		snprintf(buf, len, "%s.ixfr", zfile);
	else snprintf(buf, len, "%s.ixfr.%d", zfile, file_num);
}

/* see if ixfr file exists */
static int ixfr_file_exists(const char* zfile, int file_num)
{
	struct stat statbuf;
	char ixfrfile[1024+24];
	make_ixfr_name(ixfrfile, sizeof(ixfrfile), zfile, file_num);
	memset(&statbuf, 0, sizeof(statbuf));
	if(stat(ixfrfile, &statbuf) < 0) {
		if(errno == ENOENT)
			return 0;
		/* file is not usable */
		return 0;
	}
	return 1;
}

/* unlink an ixfr file */
static int ixfr_unlink_it(struct zone* zone, const char* zfile, int file_num,
	int ignore_enoent)
{
	char ixfrfile[1024+24];
	make_ixfr_name(ixfrfile, sizeof(ixfrfile), zfile, file_num);
	VERBOSITY(3, (LOG_INFO, "delete zone %s IXFR data file %s",
		zone->opts->name, ixfrfile));
	if(unlink(ixfrfile) < 0) {
		if(ignore_enoent && errno == ENOENT)
			return 0;
		log_msg(LOG_ERR, "error to delete file %s: %s", ixfrfile,
			strerror(errno));
		return 0;
	}
	return 1;
}

/* delete rest ixfr files, that are after the current item */
static void ixfr_delete_rest_files(struct zone* zone, struct ixfr_data* from,
	const char* zfile)
{
	struct ixfr_data* data = from;
	while(data && (struct rbnode*)data != RBTREE_NULL &&
		data->file_num == 0) {
		if(data->file_num != 0) {
			(void)ixfr_unlink_it(zone, zfile, data->file_num, 0);
			data->file_num = 0;
		}
		data = (struct ixfr_data*)rbtree_previous(&data->node);
	}
}

/* delete the ixfr files that are too many */
static void ixfr_delete_superfluous_files(struct zone* zone, const char* zfile,
	int dest_num_files)
{
	int i = dest_num_files + 1;
	if(!ixfr_file_exists(zfile, i))
		return;
	while(ixfr_unlink_it(zone, zfile, i, 1)) {
		i++;
	}
}

/* rename the file */
static int ixfr_rename_it(struct zone* zone, const char* zfile, int oldnum,
	int newnum)
{
	char ixfrfile_old[1024+24];
	char ixfrfile_new[1024+24];
	make_ixfr_name(ixfrfile_old, sizeof(ixfrfile_old), zfile, oldnum);
	make_ixfr_name(ixfrfile_new, sizeof(ixfrfile_new), zfile, newnum);
	VERBOSITY(3, (LOG_INFO, "rename zone %s IXFR data file %s to %s",
		zone->opts->name, ixfrfile_old, ixfrfile_new));
	if(rename(ixfrfile_old, ixfrfile_new) < 0) {
		log_msg(LOG_ERR, "error to rename file %s: %s", ixfrfile_old,
			strerror(errno));
		return 0;
	}
	return 1;
}

/* delete if we have too many items in memory */
static void ixfr_delete_memory_items(struct zone* zone, int dest_num_files)
{
	if(!zone->ixfr || !zone->ixfr->data)
		return;
	if(dest_num_files == (int)zone->ixfr->data->count)
		return;
	if(dest_num_files > (int)zone->ixfr->data->count) {
		/* impossible, dest_num_files should be smaller */
		return;
	}

	/* delete oldest ixfr, until we have dest_num_files entries */
	while(dest_num_files < (int)zone->ixfr->data->count) {
		zone_ixfr_remove_oldest(zone->ixfr);
	}
}

/* rename the ixfr files that need to change name */
static int ixfr_rename_files(struct zone* zone, const char* zfile,
	int dest_num_files)
{
	struct ixfr_data* data;
	int destnum;
	if(!zone->ixfr || !zone->ixfr->data)
		return 1;

	/* the oldest file is at the largest number */
	data = (struct ixfr_data*)rbtree_first(zone->ixfr->data);
	destnum = dest_num_files;
	while(data && (struct rbnode*)data != RBTREE_NULL &&
		data->file_num != 0) {
		if(data->file_num == destnum) {
			/* nothing to do for rename */
			return 1;
		}

		/* if there is an existing file, delete it */
		if(ixfr_file_exists(zfile, destnum)) {
			(void)ixfr_unlink_it(zone, zfile, destnum, 0);
		}

		if(!ixfr_rename_it(zone, zfile, data->file_num, destnum)) {
			/* failure, we cannot store files */
			struct ixfr_data* prev;
			/* delete the previously renamed files */
			prev = (struct ixfr_data*)rbtree_previous(&data->node);
			if(prev && (struct rbnode*)prev != RBTREE_NULL) {
				ixfr_delete_rest_files(zone, prev, zfile);
			}
			return 0;
		}
		data->file_num = destnum;

		data = (struct ixfr_data*)rbtree_next(&data->node);
		destnum--;
		if(destnum == 0)
			return 1; /* not possible to hit 0 file num */
	}
	return 1;
}

/* write the ixfr data file header */
static int ixfr_write_file_header(struct zone* zone, struct ixfr_data* data,
	FILE* out)
{
	if(!fprintf(out, "; IXFR data file\n"))
		return 0;
	if(!fprintf(out, "; zone %s\n", zone->opts->name))
		return 0;
	if(!fprintf(out, "; from_serial %u\n", (unsigned)data->oldserial))
		return 0;
	if(!fprintf(out, "; to_serial %u\n", (unsigned)data->newserial))
		return 0;
	if(data->log_str) {
		if(!fprintf(out, "; %s\n", data->log_str))
			return 0;
	}
	return 1;
}

/* print rdata on one line */
static int
oneline_print_rdata(buffer_type *output, rrtype_descriptor_type *descriptor,
	rr_type* record)
{
	size_t i;
	size_t saved_position = buffer_position(output);

	for (i = 0; i < record->rdata_count; ++i) {
		if (i == 0) {
			buffer_printf(output, "\t");
		} else {
			buffer_printf(output, " ");
		}
		if (!rdata_atom_to_string(
			    output,
			    (rdata_zoneformat_type) descriptor->zoneformat[i],
			    record->rdatas[i], record))
		{
			buffer_set_position(output, saved_position);
			return 0;
		}
	}

	return 1;
}

/* parse wireformat RR into a struct RR in temp region */
static int parse_wirerr_into_temp(struct zone* zone, char* fname,
	struct region* temp, uint8_t* buf, size_t len,
	const dname_type** dname, struct rr* rr)
{
	size_t bufpos = 0;
	uint16_t rdlen;
	ssize_t rdata_num;
	buffer_type packet;
	domain_table_type* owners;
	owners = domain_table_create(temp);
	memset(rr, 0, sizeof(*rr));
	*dname = dname_make(temp, buf, 1);
	if(!*dname) {
		log_msg(LOG_ERR, "failed to write zone %s IXFR data %s: failed to parse dname", zone->opts->name, fname);
		return 0;
	}
	bufpos = (*dname)->name_size;
	if(bufpos+10 > len) {
		log_msg(LOG_ERR, "failed to write zone %s IXFR data %s: buffer too short", zone->opts->name, fname);
		return 0;
	}
	rr->type = read_uint16(buf+bufpos);
	bufpos += 2;
	rr->klass = read_uint16(buf+bufpos);
	bufpos += 2;
	rr->ttl = read_uint32(buf+bufpos);
	bufpos += 4;
	rdlen = read_uint16(buf+bufpos);
	bufpos += 2;
	if(bufpos + rdlen > len) {
		log_msg(LOG_ERR, "failed to write zone %s IXFR data %s: buffer too short for rdatalen", zone->opts->name, fname);
		return 0;
	}
	buffer_create_from(&packet, buf+bufpos, rdlen);
	rdata_num = rdata_wireformat_to_rdata_atoms(
		temp, owners, rr->type, rdlen, &packet, &rr->rdatas);
	if(rdata_num == -1) {
		log_msg(LOG_ERR, "failed to write zone %s IXFR data %s: cannot parse rdata", zone->opts->name, fname);
		return 0;
	}
	rr->rdata_count = rdata_num;
	return 1;
}

/* print RR on one line in output buffer. caller must zeroterminate, if
 * that is needed. */
static int print_rr_oneline(struct buffer* rr_buffer, const dname_type* dname,
	struct rr* rr)
{
	rrtype_descriptor_type *descriptor;
	descriptor = rrtype_descriptor_by_type(rr->type);
	buffer_printf(rr_buffer, "%s", dname_to_string(dname, NULL));
	buffer_printf(rr_buffer, "\t%lu\t%s\t%s", (unsigned long)rr->ttl,
		rrclass_to_string(rr->klass), rrtype_to_string(rr->type));
	if(!oneline_print_rdata(rr_buffer, descriptor, rr)) {
		if(!rdata_atoms_to_unknown_string(rr_buffer,
			descriptor, rr->rdata_count, rr->rdatas)) {
			return 0;
		}
	}
	return 1;
}

/* write one RR to file, on one line */
static int ixfr_write_rr(struct zone* zone, FILE* out, char* fname,
	uint8_t* buf, size_t len, struct region* temp, buffer_type* rr_buffer)
{
	const dname_type* dname;
	struct rr rr;

	if(!parse_wirerr_into_temp(zone, fname, temp, buf, len, &dname, &rr)) {
		region_free_all(temp);
		return 0;
	}

	buffer_clear(rr_buffer);
	if(!print_rr_oneline(rr_buffer, dname, &rr)) {
		log_msg(LOG_ERR, "failed to write zone %s IXFR data %s: cannot spool RR string into buffer", zone->opts->name, fname);
		region_free_all(temp);
		return 0;
	}
	buffer_write_u8(rr_buffer, 0);
	buffer_flip(rr_buffer);

	if(!fprintf(out, "%s\n", buffer_begin(rr_buffer))) {
		log_msg(LOG_ERR, "failed to write zone %s IXFR data %s: cannot print RR string to file: %s", zone->opts->name, fname, strerror(errno));
		region_free_all(temp);
		return 0;
	}
	region_free_all(temp);
	return 1;
}

/* write ixfr RRs to file */
static int ixfr_write_rrs(struct zone* zone, FILE* out, char* fname,
	uint8_t* buf, size_t len, struct region* temp, buffer_type* rr_buffer)
{
	size_t current = 0;
	if(!buf || len == 0)
		return 1;
	while(current < len) {
		size_t rrlen = count_rr_length(buf, len, current);
		if(rrlen == 0)
			return 0;
		if(current + rrlen > len)
			return 0;
		if(!ixfr_write_rr(zone, out, fname, buf+current, rrlen,
			temp, rr_buffer))
			return 0;
		current += rrlen;
	}
	return 1;
}

/* write the ixfr data file data */
static int ixfr_write_file_data(struct zone* zone, struct ixfr_data* data,
	FILE* out, char* fname)
{
	struct region* temp, *rrtemp;
	buffer_type* rr_buffer;
	temp = region_create(xalloc, free);
	rrtemp = region_create(xalloc, free);
	rr_buffer = buffer_create(rrtemp, MAX_RDLENGTH);

	if(!ixfr_write_rrs(zone, out, fname, data->newsoa, data->newsoa_len,
		temp, rr_buffer)) {
		region_destroy(temp);
		region_destroy(rrtemp);
		return 0;
	}
	if(!ixfr_write_rrs(zone, out, fname, data->oldsoa, data->oldsoa_len,
		temp, rr_buffer)) {
		region_destroy(temp);
		region_destroy(rrtemp);
		return 0;
	}
	if(!ixfr_write_rrs(zone, out, fname, data->del, data->del_len,
		temp, rr_buffer)) {
		region_destroy(temp);
		region_destroy(rrtemp);
		return 0;
	}
	if(!ixfr_write_rrs(zone, out, fname, data->add, data->add_len,
		temp, rr_buffer)) {
		region_destroy(temp);
		region_destroy(rrtemp);
		return 0;
	}
	region_destroy(temp);
	region_destroy(rrtemp);
	return 1;
}

/* write the ixfr data to file */
static int ixfr_write_file(struct zone* zone, struct ixfr_data* data,
	const char* zfile, int file_num)
{
	char ixfrfile[1024+24];
	FILE* out;
	make_ixfr_name(ixfrfile, sizeof(ixfrfile), zfile, file_num);
	VERBOSITY(1, (LOG_INFO, "writing zone %s IXFR data to file %s",
		zone->opts->name, ixfrfile));
	out = fopen(ixfrfile, "w");
	if(!out) {
		log_msg(LOG_ERR, "could not open for writing zone %s IXFR file %s: %s",
			zone->opts->name, ixfrfile, strerror(errno));
		return 0;
	}

	if(!ixfr_write_file_header(zone, data, out)) {
		log_msg(LOG_ERR, "could not write file header for zone %s IXFR file %s: %s",
			zone->opts->name, ixfrfile, strerror(errno));
		fclose(out);
		return 0;
	}
	if(!ixfr_write_file_data(zone, data, out, ixfrfile)) {
		fclose(out);
		return 0;
	}

	fclose(out);
	data->file_num = file_num;
	return 1;
}

/* write the ixfr files that need to be stored on disk */
static void ixfr_write_files(struct zone* zone, const char* zfile)
{
	int num;
	struct ixfr_data* data;
	if(!zone->ixfr || !zone->ixfr->data)
		return; /* nothing to write */

	/* write unwritten files to disk */
	data = (struct ixfr_data*)rbtree_last(zone->ixfr->data);
	num=1;
	while(data && (struct rbnode*)data != RBTREE_NULL &&
		data->file_num == 0) {
		if(!ixfr_write_file(zone, data, zfile, num)) {
			/* there could be more files that are sitting on the
			 * disk, remove them, they are not used without
			 * this ixfr file */
			ixfr_delete_rest_files(zone, data, zfile);
			return;
		}
		num++;
		data = (struct ixfr_data*)rbtree_previous(&data->node);
	}
}

void ixfr_write_to_file(struct zone* zone, const char* zfile)
{
	int dest_num_files = 0;
	/* we just wrote the zonefile zfile, and it is time to write
	 * the IXFR contents to the disk too. */
	/* find out what the target number of files is that we want on
	 * the disk */
	dest_num_files = ixfr_target_number_files(zone);

	/* delete if we have more than we need */
	ixfr_delete_superfluous_files(zone, zfile, dest_num_files);

	/* delete if we have too much in memory */
	ixfr_delete_memory_items(zone, dest_num_files);

	/* rename the transfers that we have that already have a file */
	if(!ixfr_rename_files(zone, zfile, dest_num_files))
		return;

	/* write the transfers that are not written yet */
	ixfr_write_files(zone, zfile);
}

/* skip whitespace */
static char* skipwhite(char* str)
{
	while(isspace((unsigned char)*str))
		str++;
	return str;
}

/* read one RR from file */
static int ixfr_data_readrr(struct zone* zone, FILE* in, const char* ixfrfile,
	struct region* tempregion, struct domain_table* temptable,
	struct zone* tempzone, struct rr** rr)
{
	char line[65536];
	char* str;
	struct domain* domain_parsed = NULL;
	int num_rrs = 0;
	line[sizeof(line)-1]=0;
	while(!feof(in)) {
		if(!fgets(line, sizeof(line), in)) {
			if(errno == 0) {
				log_msg(LOG_ERR, "zone %s IXFR data %s: "
					"unexpected end of file", zone->opts->name, ixfrfile);
				return 0;
			}
			log_msg(LOG_ERR, "zone %s IXFR data %s: "
				"cannot read: %s", zone->opts->name, ixfrfile,
				strerror(errno));
			return 0;
		}
		str = skipwhite(line);
		if(str[0] == 0) {
			/* empty line */
			continue;
		}
		if(str[0] == ';') {
			/* comment line */
			continue;
		}
		if(zonec_parse_string(tempregion, temptable, tempzone,
			line, &domain_parsed, &num_rrs)) {
			log_msg(LOG_ERR, "zone %s IXFR data %s: parse error",
				zone->opts->name, ixfrfile);
			return 0;
		}
		if(num_rrs != 1) {
			log_msg(LOG_ERR, "zone %s IXFR data %s: parse error",
				zone->opts->name, ixfrfile);
			return 0;
		}
		*rr = &domain_parsed->rrsets->rrs[0];
		return 1;
	}
	log_msg(LOG_ERR, "zone %s IXFR data %s: file too short, no newsoa",
		zone->opts->name, ixfrfile);
	return 0;
}

/* delete from domain table */
static void domain_table_delete(struct domain_table* table,
	struct domain* domain)
{
#ifdef USE_RADIX_TREE
	radix_delete(table->nametree, domain->rnode);
#else
	rbtree_delete(table->names_to_domains, domain->node.key);
#endif
}

/* can we delete temp domain */
static int can_del_temp_domain(struct domain* domain)
{
	struct domain* n;
	/* we want to keep the zone apex */
	if(domain->is_apex)
		return 0;
	if(domain->rrsets)
		return 0;
	if(domain->usage)
		return 0;
	/* check if there are domains under it */
	n = domain_next(domain);
	if(n && domain_is_subdomain(n, domain))
		return 0;
	return 1;
}

/* delete temporary domain */
static void ixfr_temp_deldomain(struct domain_table* temptable,
	struct domain* domain)
{
	struct domain* p;
	if(!can_del_temp_domain(domain))
		return;
	p = domain->parent;
	domain_table_delete(temptable, domain);
	while(p) {
		struct domain* up = p->parent;
		if(!can_del_temp_domain(p))
			break;
		domain_table_delete(temptable, p);
		p = up;
	}
}

/* clear out the just read RR from the temp table */
static void clear_temp_table_of_rr(struct domain_table* temptable,
	struct zone* tempzone, struct rr* rr)
{
	/* clear domains in the rdata */
	unsigned i;
	for(i=0; i<rr->rdata_count; i++) {
		if(rdata_atom_is_domain(rr->type, i)) {
			/* clear out that dname */
			struct domain* domain =
				rdata_atom_domain(rr->rdatas[i]);
			domain->usage --;
			if(domain != tempzone->apex && domain->usage == 0)
				ixfr_temp_deldomain(temptable, domain);
		}
	}

	/* clear domain_parsed */
	if(rr->owner == tempzone->apex) {
		tempzone->apex->rrsets = NULL;
	} else {
		rr->owner->usage --;
		if(rr->owner->usage == 0) {
			ixfr_temp_deldomain(temptable, rr->owner);
		}
	}
}

/* read ixfr data new SOA */
static int ixfr_data_readnewsoa(struct ixfr_data* data, struct zone* zone,
	FILE* in, const char* ixfrfile, struct region* tempregion,
	struct domain_table* temptable, struct zone* tempzone,
	uint32_t dest_serial)
{
	struct rr* rr;
	size_t capacity = 0;
	if(!ixfr_data_readrr(zone, in, ixfrfile, tempregion, temptable,
		tempzone, &rr))
		return 0;
	if(rr->type != TYPE_SOA) {
		log_msg(LOG_ERR, "zone %s ixfr data %s: IXFR data does not start with SOA",
			zone->opts->name, ixfrfile);
		return 0;
	}
	if(rr->klass != CLASS_IN) {
		log_msg(LOG_ERR, "zone %s ixfr data %s: IXFR data is not class IN",
			zone->opts->name, ixfrfile);
		return 0;
	}
	if(!zone->apex) {
		log_msg(LOG_ERR, "zone %s ixfr data %s: zone has no apex, no zone data",
			zone->opts->name, ixfrfile);
		return 0;
	}
	if(dname_compare(domain_dname(zone->apex), domain_dname(rr->owner)) != 0) {
		log_msg(LOG_ERR, "zone %s ixfr data %s: IXFR data wrong SOA for zone %s",
			zone->opts->name, ixfrfile, domain_to_string(rr->owner));
		return 0;
	}
	data->newserial = soa_rr_get_serial(rr);
	if(data->newserial != dest_serial) {
		log_msg(LOG_ERR, "zone %s ixfr data %s: IXFR data contains the wrong version, serial %u but want destination serial %u",
			zone->opts->name, ixfrfile, data->newserial,
			dest_serial);
		return 0;
	}
	if(!ixfr_putrr(domain_dname(rr->owner), rr->type, rr->klass, rr->ttl, rr->rdatas, rr->rdata_count, &data->newsoa, &data->newsoa_len, &capacity)) {
		log_msg(LOG_ERR, "zone %s ixfr data %s: cannot allocate space",
			zone->opts->name, ixfrfile);
		return 0;
	}
	ixfr_trim_capacity(&data->newsoa, &data->newsoa_len, &capacity);
	clear_temp_table_of_rr(temptable, tempzone, rr);
	region_free_all(tempregion);
	return 1;
}

/* read ixfr data old SOA */
static int ixfr_data_readoldsoa(struct ixfr_data* data, struct zone* zone,
	FILE* in, const char* ixfrfile, struct region* tempregion,
	struct domain_table* temptable, struct zone* tempzone,
	uint32_t* dest_serial)
{
	struct rr* rr;
	size_t capacity = 0;
	if(!ixfr_data_readrr(zone, in, ixfrfile, tempregion, temptable,
		tempzone, &rr))
		return 0;
	if(rr->type != TYPE_SOA) {
		log_msg(LOG_ERR, "zone %s ixfr data %s: IXFR data 2nd RR is not SOA",
			zone->opts->name, ixfrfile);
		return 0;
	}
	if(rr->klass != CLASS_IN) {
		log_msg(LOG_ERR, "zone %s ixfr data %s: IXFR data 2ndSOA is not class IN",
			zone->opts->name, ixfrfile);
		return 0;
	}
	if(!zone->apex) {
		log_msg(LOG_ERR, "zone %s ixfr data %s: zone has no apex, no zone data",
			zone->opts->name, ixfrfile);
		return 0;
	}
	if(dname_compare(domain_dname(zone->apex), domain_dname(rr->owner)) != 0) {
		log_msg(LOG_ERR, "zone %s ixfr data %s: IXFR data wrong 2nd SOA for zone %s",
			zone->opts->name, ixfrfile, domain_to_string(rr->owner));
		return 0;
	}
	data->oldserial = soa_rr_get_serial(rr);
	if(!ixfr_putrr(domain_dname(rr->owner), rr->type, rr->klass, rr->ttl, rr->rdatas, rr->rdata_count, &data->oldsoa, &data->oldsoa_len, &capacity)) {
		log_msg(LOG_ERR, "zone %s ixfr data %s: cannot allocate space",
			zone->opts->name, ixfrfile);
		return 0;
	}
	ixfr_trim_capacity(&data->oldsoa, &data->oldsoa_len, &capacity);
	clear_temp_table_of_rr(temptable, tempzone, rr);
	region_free_all(tempregion);
	*dest_serial = data->oldserial;
	return 1;
}

/* read ixfr data del section */
static int ixfr_data_readdel(struct ixfr_data* data, struct zone* zone,
	FILE* in, const char* ixfrfile, struct region* tempregion,
	struct domain_table* temptable, struct zone* tempzone)
{
	struct rr* rr;
	size_t capacity = 0;
	while(1) {
		if(!ixfr_data_readrr(zone, in, ixfrfile, tempregion, temptable,
			tempzone, &rr))
			return 0;
		if(!ixfr_putrr(domain_dname(rr->owner), rr->type, rr->klass, rr->ttl, rr->rdatas, rr->rdata_count, &data->del, &data->del_len, &capacity)) {
			log_msg(LOG_ERR, "zone %s ixfr data %s: cannot allocate space",
				zone->opts->name, ixfrfile);
			return 0;
		}
		/* check SOA and also serial, because there could be other
		 * add and del sections from older versions collated, we can
		 * see this del section end when it has the serial */
		if(rr->type == TYPE_SOA &&
			soa_rr_get_serial(rr) == data->newserial) {
			/* end of del section. */
			clear_temp_table_of_rr(temptable, tempzone, rr);
			region_free_all(tempregion);
			break;
		}
		clear_temp_table_of_rr(temptable, tempzone, rr);
		region_free_all(tempregion);
	}
	ixfr_trim_capacity(&data->del, &data->del_len, &capacity);
	return 1;
}

/* read ixfr data add section */
static int ixfr_data_readadd(struct ixfr_data* data, struct zone* zone,
	FILE* in, const char* ixfrfile, struct region* tempregion,
	struct domain_table* temptable, struct zone* tempzone)
{
	struct rr* rr;
	size_t capacity = 0;
	while(1) {
		if(!ixfr_data_readrr(zone, in, ixfrfile, tempregion, temptable,
			tempzone, &rr))
			return 0;
		if(!ixfr_putrr(domain_dname(rr->owner), rr->type, rr->klass, rr->ttl, rr->rdatas, rr->rdata_count, &data->add, &data->add_len, &capacity)) {
			log_msg(LOG_ERR, "zone %s ixfr data %s: cannot allocate space",
				zone->opts->name, ixfrfile);
			return 0;
		}
		if(rr->type == TYPE_SOA &&
			soa_rr_get_serial(rr) == data->newserial) {
			/* end of add section. */
			clear_temp_table_of_rr(temptable, tempzone, rr);
			region_free_all(tempregion);
			break;
		}
		clear_temp_table_of_rr(temptable, tempzone, rr);
		region_free_all(tempregion);
	}
	ixfr_trim_capacity(&data->add, &data->add_len, &capacity);
	return 1;
}

/* read ixfr data from file */
static int ixfr_data_read(struct nsd* nsd, struct zone* zone, FILE* in,
	const char* ixfrfile, uint32_t* dest_serial, int file_num)
{
	struct ixfr_data* data = NULL;
	struct region* tempregion, *stayregion;
	struct domain_table* temptable;
	struct zone* tempzone;

	if(zone->ixfr &&
		zone->ixfr->data->count == zone->opts->pattern->ixfr_number) {
		VERBOSITY(3, (LOG_INFO, "zone %s skip %s IXFR data because only %d ixfr-number configured",
			zone->opts->name, ixfrfile, (int)zone->opts->pattern->ixfr_number));
		return 0;
	}

	/* the file has header comments, new soa, old soa, delsection,
	 * addsection. The delsection and addsection end in a SOA of oldver
	 * and newver respectively. */
	data = xalloc_zero(sizeof(*data));
	data->file_num = file_num;

	/* the temp region is cleared after every RR */
	tempregion = region_create(xalloc, free);
	/* the stay region holds the temporary data that stays between RRs */
	stayregion = region_create(xalloc, free);
	temptable = domain_table_create(stayregion);
	tempzone = region_alloc_zero(stayregion, sizeof(zone_type));
	if(!zone->apex) {
		ixfr_data_free(data);
		region_destroy(tempregion);
		region_destroy(stayregion);
		return 0;
	}
	tempzone->apex = domain_table_insert(temptable,
		domain_dname(zone->apex));
	tempzone->opts = zone->opts;
	/* switch to per RR region for new allocations in temp domain table */
	temptable->region = tempregion;

	if(!ixfr_data_readnewsoa(data, zone, in, ixfrfile, tempregion,
		temptable, tempzone, *dest_serial)) {
		ixfr_data_free(data);
		region_destroy(tempregion);
		region_destroy(stayregion);
		return 0;
	}
	if(!ixfr_data_readoldsoa(data, zone, in, ixfrfile, tempregion,
		temptable, tempzone, dest_serial)) {
		ixfr_data_free(data);
		region_destroy(tempregion);
		region_destroy(stayregion);
		return 0;
	}
	if(!ixfr_data_readdel(data, zone, in, ixfrfile, tempregion, temptable,
		tempzone)) {
		ixfr_data_free(data);
		region_destroy(tempregion);
		region_destroy(stayregion);
		return 0;
	}
	if(!ixfr_data_readadd(data, zone, in, ixfrfile, tempregion, temptable,
		tempzone)) {
		ixfr_data_free(data);
		region_destroy(tempregion);
		region_destroy(stayregion);
		return 0;
	}

	region_destroy(tempregion);
	region_destroy(stayregion);

	if(!zone->ixfr)
		zone->ixfr = zone_ixfr_create(nsd);
	if(zone->opts->pattern->ixfr_size != 0 &&
		zone->ixfr->total_size + ixfr_data_size(data) >
		zone->opts->pattern->ixfr_size) {
		VERBOSITY(3, (LOG_INFO, "zone %s skip %s IXFR data because only ixfr-size: %u configured, and it is %u size",
			zone->opts->name, ixfrfile, (unsigned)zone->opts->pattern->ixfr_size, (unsigned)ixfr_data_size(data)));
		ixfr_data_free(data);
		return 0;
	}
	zone_ixfr_add(zone->ixfr, data);
	VERBOSITY(3, (LOG_INFO, "zone %s read %s IXFR data of %u bytes",
		zone->opts->name, ixfrfile, (unsigned)ixfr_data_size(data)));
	return 1;
}

/* try to read the next ixfr file. returns false if it fails or if it
 * does not fit in the configured sizes */
static int ixfr_read_one_more_file(struct nsd* nsd, struct zone* zone,
	const char* zfile, int num_files, uint32_t *dest_serial)
{
	char ixfrfile[1024+24];
	FILE* in;
	int file_num = num_files+1;
	make_ixfr_name(ixfrfile, sizeof(ixfrfile), zfile, file_num);
	in = fopen(ixfrfile, "r");
	if(!in) {
		if(errno == ENOENT) {
			/* the file does not exist, we reached the end
			 * of the list of IXFR files */
			return 0;
		}
		log_msg(LOG_ERR, "could not read zone %s IXFR file %s: %s",
			zone->opts->name, ixfrfile, strerror(errno));
		return 0;
	}
	warn_if_directory("IXFR data", in, ixfrfile);
	if(!ixfr_data_read(nsd, zone, in, ixfrfile, dest_serial, file_num)) {
		fclose(in);
		return 0;
	}
	fclose(in);
	return 1;
}

void ixfr_read_from_file(struct nsd* nsd, struct zone* zone, const char* zfile)
{
	uint32_t serial;
	int num_files = 0;
	/* delete the existing data, the zone data in memory has likely
	 * changed, eg. due to reading a new zonefile. So that needs new
	 * IXFRs */
	zone_ixfr_clear(zone->ixfr);

	/* track the serial number that we need to end up with, and check
	 * that the IXFRs match up and result in the required version */
	serial = zone_get_current_serial(zone);

	while(ixfr_read_one_more_file(nsd, zone, zfile, num_files, &serial)) {
		num_files++;
	}
	if(num_files > 0) {
		VERBOSITY(1, (LOG_INFO, "zone %s read %d IXFR transfers with success",
			zone->opts->name, num_files));
		zone->ixfr->num_files = num_files;
	}
}