/*****************************************************************************
Copyright (C) 2013, 2015, Google Inc. All Rights Reserved.
Copyright (C) 2014, 2015, MariaDB Corporation. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

*****************************************************************************/
/**************************************************//**
@file fil0crypt.cc
Innodb file space encrypt/decrypt

Created            Jonas Oreland Google
Modified           Jan Lindström jan.lindstrom@mariadb.com
*******************************************************/

#include "fil0fil.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "mach0data.h"
#include "log0recv.h"
#include "mtr0mtr.h"
#include "mtr0log.h"
#include "page0zip.h"
#include "ut0ut.h"
#include "btr0scrub.h"
#include "fsp0fsp.h"
#include "fil0pagecompress.h"
#include "fil0pageencryption.h"
#include "ha_prototypes.h" // IB_LOG_

#include <my_crypt.h>

#include <my_aes.h>
#include <math.h>


/** Mutex for keys */
UNIV_INTERN ib_mutex_t fil_crypt_key_mutex;

#ifdef UNIV_PFS_MUTEX
UNIV_INTERN mysql_pfs_key_t fil_crypt_key_mutex_key;
#endif

/** Is encryption enabled/disabled */
UNIV_INTERN my_bool srv_encrypt_tables = FALSE;

/** No of key rotation threads requested */
UNIV_INTERN uint srv_n_fil_crypt_threads = 0;

/** No of key rotation threads started */
static uint srv_n_fil_crypt_threads_started = 0;

/** At this age or older a space/page will be rotated */
UNIV_INTERN uint srv_fil_crypt_rotate_key_age = 1;

/** Event to signal FROM the key rotation threads. */
UNIV_INTERN os_event_t fil_crypt_event;

/** Event to signal TO the key rotation threads. */
UNIV_INTERN os_event_t fil_crypt_threads_event;

/** Event for waking up threads throttle */
UNIV_INTERN os_event_t fil_crypt_throttle_sleep_event;

/** Mutex for key rotation threads */
UNIV_INTERN ib_mutex_t fil_crypt_threads_mutex;

#ifdef UNIV_PFS_MUTEX
UNIV_INTERN mysql_pfs_key_t fil_crypt_threads_mutex_key;
#endif

/** Variable ensuring only 1 thread at time does initial conversion */
static bool fil_crypt_start_converting = false;

/** Variables for throttling */
UNIV_INTERN uint srv_n_fil_crypt_iops = 100;	 // 10ms per iop
static uint srv_alloc_time = 3;		    // allocate iops for 3s at a time
static uint n_fil_crypt_iops_allocated = 0;

/** Variables for scrubbing */
extern uint srv_background_scrub_data_interval;
extern uint srv_background_scrub_data_check_interval;

#define DEBUG_KEYROTATION_THROTTLING 0

/** Statistics variables */
static fil_crypt_stat_t crypt_stat;
static ib_mutex_t crypt_stat_mutex;

#ifdef UNIV_PFS_MUTEX
UNIV_INTERN mysql_pfs_key_t fil_crypt_stat_mutex_key;
#endif

/**
 * key for crypt data mutex
*/
#ifdef UNIV_PFS_MUTEX
UNIV_INTERN mysql_pfs_key_t fil_crypt_data_mutex_key;
#endif

/**
* Magic pattern in start of crypt data on page 0
*/
#define MAGIC_SZ 6

static const unsigned char CRYPT_MAGIC[MAGIC_SZ] = {
	's', 0xE, 0xC, 'R', 'E', 't' };

static const unsigned char EMPTY_PATTERN[MAGIC_SZ] = {
	0x0, 0x0, 0x0, 0x0, 0x0, 0x0 };

/**
 * CRYPT_SCHEME_UNENCRYPTED
 *
 * Used as intermediate state when convering a space from unencrypted
 * to encrypted
 */
#define CRYPT_SCHEME_UNENCRYPTED 0

/**
 * CRYPT_SCHEME_1
 *
 * L = AES_ECB(KEY, IV)
 * CRYPT(PAGE) = AES_CRT(KEY=L, IV=C, PAGE)
 */
#define CRYPT_SCHEME_1 1
#define CRYPT_SCHEME_1_IV_LEN 16
// cached L given key_version
struct key_struct
{
	uint key_version;
	byte key[CRYPT_SCHEME_1_IV_LEN];
};

struct fil_space_rotate_state_t
{
	time_t start_time;    // time when rotation started
	ulint active_threads; // active threads in space
	ulint next_offset;    // next "free" offset
	ulint max_offset;     // max offset needing to be rotated
	uint  min_key_version_found; // min key version found but not rotated
	lsn_t end_lsn;		     // max lsn created when rotating this space
	bool starting;		     // initial write of IV
	bool flushing;		     // space is being flushed at end of rotate
	struct {
		bool is_active; // is scrubbing active in this space
		time_t last_scrub_completed; // when was last scrub completed
	} scrubbing;
};

struct fil_space_crypt_struct
{
	ulint type;	    // CRYPT_SCHEME
	uint keyserver_requests; // no of key requests to key server
	uint key_count;	    // No of initalized key-structs
	key_struct keys[3]; // cached L = AES_ECB(KEY, IV)
	uint min_key_version; // min key version for this space
	ulint page0_offset;   // byte offset on page 0 for crypt data

	ib_mutex_t mutex;   // mutex protecting following variables
	bool closing;	    // is tablespace being closed
	fil_space_rotate_state_t rotate_state;

	uint iv_length;	    // length of IV
	byte iv[1];	    // IV-data
};

/*********************************************************************
Init space crypt */
UNIV_INTERN
void
fil_space_crypt_init()
{
	mutex_create(fil_crypt_key_mutex_key,
		     &fil_crypt_key_mutex, SYNC_NO_ORDER_CHECK);

	fil_crypt_throttle_sleep_event = os_event_create();

	mutex_create(fil_crypt_stat_mutex_key,
		     &crypt_stat_mutex, SYNC_NO_ORDER_CHECK);
	memset(&crypt_stat, 0, sizeof(crypt_stat));
}

/*********************************************************************
Cleanup space crypt */
UNIV_INTERN
void
fil_space_crypt_cleanup()
{
	os_event_free(fil_crypt_throttle_sleep_event);
}

/******************************************************************
Get key bytes for a space/key-version */
static
void
fil_crypt_get_key(byte *dst, uint* key_length,
	fil_space_crypt_t* crypt_data, uint version, bool page_encrypted)
{
        unsigned char keybuf[MY_AES_MAX_KEY_LENGTH];
	unsigned char iv[CRYPT_SCHEME_1_IV_LEN];
	ulint iv_len = sizeof(iv);

        if (!page_encrypted) {
		mutex_enter(&crypt_data->mutex);

		// Check if we already have key
		for (uint i = 0; i < crypt_data->key_count; i++) {
			if (crypt_data->keys[i].key_version == version) {
				memcpy(dst, crypt_data->keys[i].key,
					sizeof(crypt_data->keys[i].key));
				mutex_exit(&crypt_data->mutex);
				return;
			}
		}
		// Not found!
		crypt_data->keyserver_requests++;

		// Rotate keys to make room for a new
		for (uint i = 1; i < array_elements(crypt_data->keys); i++) {
			crypt_data->keys[i] = crypt_data->keys[i - 1];
		}
        }
        else
        {
            // load iv

	    int rc = get_encryption_iv(version, (unsigned char*)iv, iv_len);

	    if (rc != CRYPT_KEY_OK) {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"IV %d can not be found. Reason=%d", version, rc);
		ut_error;
	    }
        }

	if (has_encryption_key(version)) {
	    *key_length = get_encryption_key_size(version);

	    int rc = get_encryption_key(version, (unsigned char*)keybuf, *key_length);

	    if (rc != CRYPT_KEY_OK) {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Key %d can not be found. Reason=%d", version, rc);
		ut_error;
	    }
	} else {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Key %d not found", version);
		ut_error;
	}


	// do ctr key initialization
	if (current_aes_dynamic_method == MY_AES_ALGORITHM_CTR)
	{
	  // Now compute L by encrypting IV using this key
	  const unsigned char* src = page_encrypted ? iv : crypt_data->iv;
	  const int srclen = page_encrypted ? iv_len : crypt_data->iv_length;
	  unsigned char* buf = page_encrypted ? keybuf : crypt_data->keys[0].key;
	  uint32 buflen = page_encrypted ? *key_length : sizeof(crypt_data->keys[0].key);

	  // call ecb explicit
	  my_aes_encrypt_dynamic_type func = get_aes_encrypt_func(MY_AES_ALGORITHM_ECB);
	  int rc = (*func)(src, srclen,
		       buf, &buflen,
		       (unsigned char*)keybuf, *key_length,
		       NULL, 0,
		       1);

	  if (rc != AES_OK) {
		  ib_logf(IB_LOG_LEVEL_FATAL,
			  "Unable to encrypt key-block "
			  " src: %p srclen: %d buf: %p buflen: %d."
			  " return-code: %d. Can't continue!\n",
			  src, srclen, buf, buflen, rc);
		  ut_error;
	  }

	  if (!page_encrypted) {
		  crypt_data->keys[0].key_version = version;
		  crypt_data->key_count++;

		  if (crypt_data->key_count > array_elements(crypt_data->keys)) {
			  crypt_data->key_count = array_elements(crypt_data->keys);
		  }
	  }

	  // set the key size to the aes block size because this encrypted data is the key
	  *key_length = MY_AES_BLOCK_SIZE;
	  memcpy(dst, buf, buflen);
	}
	else
	{
	    // otherwise keybuf contains the right key
	    memcpy(dst, keybuf, *key_length);
	}

	if (!page_encrypted) {
		mutex_exit(&crypt_data->mutex);
	}
}

/******************************************************************
Get key bytes for a space/latest(key-version) */
static inline
void
fil_crypt_get_latest_key(byte *dst, uint* key_length,
			 fil_space_crypt_t* crypt_data, uint *version)
{
	if (srv_encrypt_tables) {
	        // used for key rotation - get the next key id from the key provider
		int rc = get_latest_encryption_key_version();

		// if no new key was created use the last one
		if (rc >= 0)
		{
		    *version = rc;
		}

		return fil_crypt_get_key(dst, key_length, crypt_data, *version, false);
	}
        return fil_crypt_get_key(dst, key_length, NULL, *version, true);
}

/******************************************************************
Create a fil_space_crypt_t object */
UNIV_INTERN
fil_space_crypt_t*
fil_space_create_crypt_data()
{
	const uint iv_length = CRYPT_SCHEME_1_IV_LEN;
	const uint sz = sizeof(fil_space_crypt_t) + iv_length;
	fil_space_crypt_t* crypt_data =
		static_cast<fil_space_crypt_t*>(malloc(sz));
	memset(crypt_data, 0, sz);

	if (srv_encrypt_tables == FALSE) {
		crypt_data->type = CRYPT_SCHEME_UNENCRYPTED;
		crypt_data->min_key_version = 0;
	} else {
		crypt_data->type = CRYPT_SCHEME_1;
		crypt_data->min_key_version = get_latest_encryption_key_version();
	}

	mutex_create(fil_crypt_data_mutex_key,
		     &crypt_data->mutex, SYNC_NO_ORDER_CHECK);
	crypt_data->iv_length = iv_length;
	my_random_bytes(crypt_data->iv, iv_length);
	return crypt_data;
}

/******************************************************************
Compare two crypt objects */
UNIV_INTERN
int
fil_space_crypt_compare(const fil_space_crypt_t* crypt_data1,
			const fil_space_crypt_t* crypt_data2)
{
	ut_a(crypt_data1->type == CRYPT_SCHEME_UNENCRYPTED ||
	     crypt_data1->type == CRYPT_SCHEME_1);
	ut_a(crypt_data2->type == CRYPT_SCHEME_UNENCRYPTED ||
	     crypt_data2->type == CRYPT_SCHEME_1);

	ut_a(crypt_data1->iv_length == CRYPT_SCHEME_1_IV_LEN);
	ut_a(crypt_data2->iv_length == CRYPT_SCHEME_1_IV_LEN);

	/* no support for changing iv (yet?) */
	ut_a(memcmp(crypt_data1->iv, crypt_data2->iv,
		    crypt_data1->iv_length) == 0);

	return 0;
}

/******************************************************************
Read crypt data from a page (0) */
UNIV_INTERN
fil_space_crypt_t*
fil_space_read_crypt_data(ulint space, const byte* page, ulint offset)
{
	if (memcmp(page + offset, EMPTY_PATTERN, MAGIC_SZ) == 0) {
		/* crypt is not stored */
		return NULL;
	}

	if (memcmp(page + offset, CRYPT_MAGIC, MAGIC_SZ) != 0) {
		ib_logf(IB_LOG_LEVEL_WARN,
			"Found potentially bogus bytes on "
			"page 0 offset %lu for space %lu : "
			"[ %.2x %.2x %.2x %.2x %.2x %.2x ]. "
			"Assuming space is not encrypted!.",
			offset, space,
			page[offset + 0],
			page[offset + 1],
			page[offset + 2],
			page[offset + 3],
			page[offset + 4],
			page[offset + 5]);
		return NULL;
	}

	ulint type = mach_read_from_1(page + offset + MAGIC_SZ + 0);

	if (! (type == CRYPT_SCHEME_UNENCRYPTED ||
	       type == CRYPT_SCHEME_1)) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Found non sensible crypt scheme: %lu for space %lu "
			" offset: %lu bytes: "
			"[ %.2x %.2x %.2x %.2x %.2x %.2x ].",
			type, space, offset,
			page[offset + 0 + MAGIC_SZ],
			page[offset + 1 + MAGIC_SZ],
			page[offset + 2 + MAGIC_SZ],
			page[offset + 3 + MAGIC_SZ],
			page[offset + 4 + MAGIC_SZ],
			page[offset + 5 + MAGIC_SZ]);
		ut_error;
	}

	ulint iv_length = mach_read_from_1(page + offset + MAGIC_SZ + 1);
	if (! (iv_length == CRYPT_SCHEME_1_IV_LEN)) {
		ib_logf(IB_LOG_LEVEL_ERROR,
			"Found non sensible iv length: %lu for space %lu "
			" offset: %lu type: %lu bytes: "
			"[ %.2x %.2x %.2x %.2x %.2x %.2x ].",
			iv_length, space, offset, type,
			page[offset + 0 + MAGIC_SZ],
			page[offset + 1 + MAGIC_SZ],
			page[offset + 2 + MAGIC_SZ],
			page[offset + 3 + MAGIC_SZ],
			page[offset + 4 + MAGIC_SZ],
			page[offset + 5 + MAGIC_SZ]);
		ut_error;
	}

	uint min_key_version = mach_read_from_4
		(page + offset + MAGIC_SZ + 2 + iv_length);

	const uint sz = sizeof(fil_space_crypt_t) + iv_length;
	fil_space_crypt_t* crypt_data = static_cast<fil_space_crypt_t*>(
		malloc(sz));
	memset(crypt_data, 0, sz);

	crypt_data->type = type;
	crypt_data->min_key_version = min_key_version;
	crypt_data->page0_offset = offset;
	mutex_create(fil_crypt_data_mutex_key,
		     &crypt_data->mutex, SYNC_NO_ORDER_CHECK);
	crypt_data->iv_length = iv_length;
	memcpy(crypt_data->iv, page + offset + MAGIC_SZ + 2, iv_length);

	return crypt_data;
}

/******************************************************************
Free a crypt data object */
UNIV_INTERN
void
fil_space_destroy_crypt_data(fil_space_crypt_t **crypt_data)
{
	if (crypt_data != NULL && (*crypt_data) != NULL) {
		/* lock (and unlock) mutex to make sure no one has it locked
		* currently */
		mutex_enter(& (*crypt_data)->mutex);
		mutex_exit(& (*crypt_data)->mutex);
		mutex_free(& (*crypt_data)->mutex);
		free(*crypt_data);
		(*crypt_data) = NULL;
	}
}

/******************************************************************
Write crypt data to a page (0) */
static
void
fil_space_write_crypt_data_low(fil_space_crypt_t *crypt_data,
			       ulint type,
			       byte* page, ulint offset,
			       ulint maxsize, mtr_t* mtr)
{
	ut_a(offset > 0 && offset < UNIV_PAGE_SIZE);
	ulint space_id = mach_read_from_4(
		page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	const uint len = crypt_data->iv_length;
	const uint min_key_version = crypt_data->min_key_version;
	crypt_data->page0_offset = offset;
	ut_a(2 + len + 4 + MAGIC_SZ < maxsize);

	/*
	redo log this as bytewise updates to page 0
	followed by an MLOG_FILE_WRITE_CRYPT_DATA
	(that will during recovery update fil_space_t)
	*/
	mlog_write_string(page + offset, CRYPT_MAGIC, MAGIC_SZ, mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 0, type, MLOG_1BYTE, mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 1, len, MLOG_1BYTE, mtr);
	mlog_write_string(page + offset + MAGIC_SZ + 2, crypt_data->iv, len,
			  mtr);
	mlog_write_ulint(page + offset + MAGIC_SZ + 2 + len, min_key_version,
			 MLOG_4BYTES, mtr);

	DBUG_EXECUTE_IF("ib_file_crypt_redo_crash_1",
		ut_error;);

	byte* log_ptr = mlog_open(mtr, 11 + 12 + len);
	if (log_ptr != NULL) {
		log_ptr = mlog_write_initial_log_record_fast(
			page,
			MLOG_FILE_WRITE_CRYPT_DATA,
			log_ptr, mtr);
		mach_write_to_4(log_ptr, space_id);
		log_ptr += 4;
		mach_write_to_2(log_ptr, offset);
		log_ptr += 2;
		mach_write_to_1(log_ptr, type);
		log_ptr += 1;
		mach_write_to_1(log_ptr, len);
		log_ptr += 1;
		mach_write_to_4(log_ptr, min_key_version);
		log_ptr += 4;
		mlog_close(mtr, log_ptr);

		mlog_catenate_string(mtr, crypt_data->iv, len);
	}

	DBUG_EXECUTE_IF("ib_file_crypt_redo_crash_2",
		ut_error;);
}

/******************************************************************
Write crypt data to a page (0) */
UNIV_INTERN
void
fil_space_write_crypt_data(ulint space, byte* page, ulint offset,
			   ulint maxsize, mtr_t* mtr)
{
	fil_space_crypt_t* crypt_data = fil_space_get_crypt_data(space);
	ibool page_encrypted = fil_space_is_page_encrypted(space);

	/* If no crypt data is stored on memory cache for this space
	or space is not encrypted and encryption is not enabled, then
	do not continue writing crypt data to page 0. */
	if (crypt_data == NULL || (!page_encrypted && !srv_encrypt_tables)) {
		return;
	}

	fil_space_write_crypt_data_low(crypt_data, crypt_data->type,
				       page, offset, maxsize, mtr);
}

/******************************************************************
Parse a MLOG_FILE_WRITE_CRYPT_DATA log entry */
UNIV_INTERN
byte*
fil_parse_write_crypt_data(byte* ptr, byte* end_ptr,
			   buf_block_t* block)
{
	/* check that redo log entry is complete */
	uint entry_size =
		4 + // size of space_id
		2 + // size of offset
		1 + // size of type
		1 + // size of iv-len
		4;  // size of min_key_version
	if (end_ptr - ptr < entry_size)
		return NULL;

	ulint space_id = mach_read_from_4(ptr);
	ptr += 4;
	uint offset = mach_read_from_2(ptr);
	ptr += 2;
	uint type = mach_read_from_1(ptr);
	ptr += 1;
	uint len = mach_read_from_1(ptr);
	ptr += 1;

	ut_a(type == CRYPT_SCHEME_UNENCRYPTED ||
	     type == CRYPT_SCHEME_1); // only supported
	ut_a(len == CRYPT_SCHEME_1_IV_LEN); // only supported
	uint min_key_version = mach_read_from_4(ptr);
	ptr += 4;

	if (end_ptr - ptr < len)
		return NULL;

	fil_space_crypt_t* crypt_data = fil_space_create_crypt_data();
	crypt_data->page0_offset = offset;
	crypt_data->min_key_version = min_key_version;
	memcpy(crypt_data->iv, ptr, len);
	ptr += len;

	/* update fil_space memory cache with crypt_data */
	fil_space_set_crypt_data(space_id, crypt_data);

	return ptr;
}

/******************************************************************
Clear crypt data from a page (0) */
UNIV_INTERN
void
fil_space_clear_crypt_data(byte* page, ulint offset)
{
	//TODO(jonaso): pass crypt-data and read len from there
	ulint len = CRYPT_SCHEME_1_IV_LEN;
	ulint size =
		sizeof(CRYPT_MAGIC) +
		1 +   // type
		1 +   // len
		len + // iv
		4;    // min key version
	memset(page + offset, 0, size);
}

/*********************************************************************
Check if page shall be encrypted before write */
UNIV_INTERN
bool
fil_space_check_encryption_write(
/*==============================*/
	ulint space)          /*!< in: tablespace id */
{
	if (srv_encrypt_tables == FALSE)
		return false;

	fil_space_crypt_t* crypt_data = fil_space_get_crypt_data(space);
	if (crypt_data == NULL)
		return false;

	if (crypt_data->type == CRYPT_SCHEME_UNENCRYPTED)
		return false;

	return true;
}

/******************************************************************
Encrypt a page */
UNIV_INTERN
void
fil_space_encrypt(ulint space, ulint offset, lsn_t lsn,
	const byte* src_frame, ulint zip_size, byte* dst_frame, ulint encryption_key)
{
	fil_space_crypt_t* crypt_data;
	ulint page_size = (zip_size) ? zip_size : UNIV_PAGE_SIZE;

	// get key (L)
	uint key_version;
	byte key[MY_AES_MAX_KEY_LENGTH];
	uint key_length;

	ulint orig_page_type = mach_read_from_2(src_frame+FIL_PAGE_TYPE);
	if (orig_page_type==FIL_PAGE_TYPE_FSP_HDR
			|| orig_page_type==FIL_PAGE_TYPE_XDES
			|| orig_page_type== FIL_PAGE_PAGE_ENCRYPTED
			|| orig_page_type== FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
		//TODO: is this really needed ?
		memcpy(dst_frame, src_frame, page_size);
		return;
	}

	if (srv_encrypt_tables) {
		crypt_data = fil_space_get_crypt_data(space);

		if (crypt_data == NULL) {
			//TODO: Is this really needed ?
			memcpy(dst_frame, src_frame, page_size);
			return;
		}

		fil_crypt_get_latest_key(key, &key_length, crypt_data, &key_version);
	} else {
		key_version = encryption_key;
		fil_crypt_get_latest_key(key, &key_length, NULL, (uint*)&key_version);
	}


	/* Load the iv or counter (depending to the encryption algorithm used) */
	unsigned char iv[MY_AES_BLOCK_SIZE];

	if (current_aes_dynamic_method == MY_AES_ALGORITHM_CTR)
	{
	  // create counter block (C)
	  mach_write_to_4(iv + 0, space);
	  ulint space_offset = mach_read_from_4(
			  src_frame + FIL_PAGE_OFFSET);
	  mach_write_to_4(iv + 4, space_offset);
	  mach_write_to_8(iv + 8, lsn);
	} else {
	    // take the iv from the key provider

	    int load_iv_rc = get_encryption_iv(key_version, (uchar *) iv, sizeof(iv));

	    // if the iv can not be loaded the whole page can not be encrypted
	    if (load_iv_rc != CRYPT_KEY_OK)
	    {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Unable to decrypt data-block. "
			" Can not load iv for key %d"
			" return-code: %d. Can't continue!\n",
			key_version, load_iv_rc);

		ut_error;
	    }
	}

	ibool page_compressed = (mach_read_from_2(src_frame+FIL_PAGE_TYPE) == FIL_PAGE_PAGE_COMPRESSED);
	ibool page_encrypted  = fil_space_is_page_encrypted(space);
	ulint compression_alg = mach_read_from_8(src_frame+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);

	// copy page header
	memcpy(dst_frame, src_frame, FIL_PAGE_DATA);


	if (page_encrypted && !page_compressed) {
		// key id
		mach_write_to_2(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION,
			key_version);
		// original page type
		mach_write_to_2(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 2,
			orig_page_type);
		// new page type
		mach_write_to_2(dst_frame+FIL_PAGE_TYPE, FIL_PAGE_PAGE_ENCRYPTED);
	} else {
		// store key version
		mach_write_to_4(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION,
			        key_version);
	}

	// encrypt page data
	ulint unencrypted_bytes = FIL_PAGE_DATA + FIL_PAGE_DATA_END;
	ulint srclen = page_size - unencrypted_bytes;
	const byte* src = src_frame + FIL_PAGE_DATA;
	byte* dst = dst_frame + FIL_PAGE_DATA;
	uint32 dstlen;

	if (page_compressed) {
		srclen = page_size -  FIL_PAGE_DATA;
	}

	int rc = (* my_aes_encrypt_dynamic)(src, srclen,
	                                    dst, &dstlen,
	                                    (unsigned char*)key, key_length,
	                                    (unsigned char*)iv, sizeof(iv),
	                                    1);

	if (! ((rc == AES_OK) && ((ulint) dstlen == srclen))) {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Unable to encrypt data-block "
			" src: %p srclen: %ld buf: %p buflen: %d."
			" return-code: %d. Can't continue!\n",
			src, (long)srclen,
			dst, dstlen, rc);
		ut_error;
	}

	if (!page_compressed) {
		// copy page trailer
		memcpy(dst_frame + page_size - FIL_PAGE_DATA_END,
			src_frame + page_size - FIL_PAGE_DATA_END,
			FIL_PAGE_DATA_END);

		/* handle post encryption checksum */
		ib_uint32_t checksum = 0;
		srv_checksum_algorithm_t algorithm =
			static_cast<srv_checksum_algorithm_t>(srv_checksum_algorithm);

		if (zip_size == 0) {
			switch (algorithm) {
			case SRV_CHECKSUM_ALGORITHM_CRC32:
			case SRV_CHECKSUM_ALGORITHM_STRICT_CRC32:
				checksum = buf_calc_page_crc32(dst_frame);
				break;
			case SRV_CHECKSUM_ALGORITHM_INNODB:
			case SRV_CHECKSUM_ALGORITHM_STRICT_INNODB:
				checksum = (ib_uint32_t) buf_calc_page_new_checksum(
					dst_frame);
				break;
			case SRV_CHECKSUM_ALGORITHM_NONE:
			case SRV_CHECKSUM_ALGORITHM_STRICT_NONE:
				checksum = BUF_NO_CHECKSUM_MAGIC;
				break;
				/* no default so the compiler will emit a warning
				* if new enum is added and not handled here */
			}
		} else {
			checksum = page_zip_calc_checksum(dst_frame, zip_size,
				                          algorithm);
		}

		// store the post-encryption checksum after the key-version
		mach_write_to_4(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 4,
			        checksum);
	} else {
		/* Page compressed and encrypted tables have different
		FIL_HEADER */
		ulint page_len = log10((double)page_size)/log10((double)2);
		/* Set up the correct page type */
		mach_write_to_2(dst_frame+FIL_PAGE_TYPE, FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED);
		/* Set up the compression algorithm */
		mach_write_to_2(dst_frame+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION+4, orig_page_type);
		/* Set up the compressed size */
		mach_write_to_1(dst_frame+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION+6, page_len);
		/* Set up the compression method */
		mach_write_to_1(dst_frame+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION+7, compression_alg);
	}

}

/*********************************************************************
Check if extra buffer shall be allocated for decrypting after read */
UNIV_INTERN
bool
fil_space_check_encryption_read(
/*==============================*/
	ulint space)          /*!< in: tablespace id */
{
	fil_space_crypt_t* crypt_data = fil_space_get_crypt_data(space);

	if (crypt_data == NULL)
		return false;

	if (crypt_data->type == CRYPT_SCHEME_UNENCRYPTED)
		return false;

	return true;
}

/******************************************************************
Decrypt a page */
UNIV_INTERN
bool
fil_space_decrypt(fil_space_crypt_t* crypt_data,
		  const byte* src_frame, ulint page_size, byte* dst_frame)
{
	ulint page_type = mach_read_from_2(src_frame+FIL_PAGE_TYPE);
	// key version
	uint key_version;
	bool page_encrypted = (page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED
		               || page_type == FIL_PAGE_PAGE_ENCRYPTED);

	bool page_compressed = (page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED
            || page_type == FIL_PAGE_PAGE_COMPRESSED);

	ulint orig_page_type=0;

	if (page_type == FIL_PAGE_PAGE_ENCRYPTED) {
		key_version = mach_read_from_2(
			src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
		orig_page_type =  mach_read_from_2(
			src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 2);
	} else {
		key_version = mach_read_from_4(
			src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
	}

	if (key_version == 0 && !page_encrypted) {
		//TODO: is this really needed ?
		memcpy(dst_frame, src_frame, page_size);
		return false; /* page not decrypted */
	}

	// read space & offset & lsn
	ulint space = mach_read_from_4(
		src_frame + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID);
	ulint offset = mach_read_from_4(
		src_frame + FIL_PAGE_OFFSET);
	ib_uint64_t lsn = mach_read_from_8(src_frame + FIL_PAGE_LSN);

	// copy page header
	memcpy(dst_frame, src_frame, FIL_PAGE_DATA);

	if (page_type == FIL_PAGE_PAGE_ENCRYPTED) {
		// orig page type
		mach_write_to_2(dst_frame+FIL_PAGE_TYPE, orig_page_type);
	}

	// get key
	byte key[MY_AES_MAX_KEY_LENGTH];
	uint key_length;
	fil_crypt_get_key(key, &key_length, crypt_data, key_version, page_encrypted);

	// get the iv
	unsigned char iv[MY_AES_BLOCK_SIZE];

	if (current_aes_dynamic_method == MY_AES_ALGORITHM_CTR)
	{
	  // create counter block

	  mach_write_to_4(iv + 0, space);
	  mach_write_to_4(iv + 4, offset);
	  mach_write_to_8(iv + 8, lsn);
	} else {
	    // take the iv from the key provider

	    int load_iv_rc = get_encryption_iv(key_version, (uchar *) iv, sizeof(iv));

	    // if the iv can not be loaded the whole page can not be decrypted
	    if (load_iv_rc != CRYPT_KEY_OK)
	    {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Unable to decrypt data-block. "
			" Can not load iv for key %d"
			" return-code: %d. Can't continue!\n",
			key_version, load_iv_rc);

		return AES_KEY_CREATION_FAILED;
	    }
	}

	const byte* src = src_frame + FIL_PAGE_DATA;
	byte* dst = dst_frame + FIL_PAGE_DATA;
	uint32 dstlen;
	ulint srclen = page_size - (FIL_PAGE_DATA + FIL_PAGE_DATA_END);
	ulint compressed_len;
	ulint compression_method;

	if (page_compressed) {
		orig_page_type = mach_read_from_2(src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION+4);
		compressed_len = mach_read_from_1(src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION+6);
		compression_method = mach_read_from_1(src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION+7);
	}

	if (page_encrypted && !page_compressed) {
		orig_page_type = mach_read_from_2(src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION+2);
	}

	if (page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
		srclen = pow((double)2, (double)((int)compressed_len)) - FIL_PAGE_DATA;
	}

	int rc = (* my_aes_decrypt_dynamic)(src, srclen,
	                                    dst, &dstlen,
	                                    (unsigned char*)key, key_length,
	                                    (unsigned char*)iv, sizeof(iv),
	                                    1);

	if (! ((rc == AES_OK) && ((ulint) dstlen == srclen))) {
		ib_logf(IB_LOG_LEVEL_FATAL,
			"Unable to decrypt data-block "
			" src: %p srclen: %ld buf: %p buflen: %d."
			" return-code: %d. Can't continue!\n",
			src, (long)srclen,
			dst, dstlen, rc);
		ut_error;
	}

	if (page_type != FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
		// copy page trailer
		memcpy(dst_frame + page_size - FIL_PAGE_DATA_END,
		       src_frame + page_size - FIL_PAGE_DATA_END,
		       FIL_PAGE_DATA_END);

		// clear key-version & crypt-checksum from dst
		memset(dst_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, 0, 8);
	} else {
		/* For page compressed tables we set up the FIL_HEADER again */
		/* setting original page type */
		mach_write_to_2(dst_frame + FIL_PAGE_TYPE, orig_page_type);
		/* page_compression uses BUF_NO_CHECKSUM_MAGIC as checksum */
		mach_write_to_4(dst_frame + FIL_PAGE_SPACE_OR_CHKSUM, BUF_NO_CHECKSUM_MAGIC);
		/* Set up the flush lsn to be compression algorithm */
		mach_write_to_8(dst_frame+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION, compression_method);
	}

	return true; /* page was decrypted */
}

/******************************************************************
Decrypt a page */
UNIV_INTERN
void
fil_space_decrypt(ulint space,
		  const byte* src_frame, ulint page_size, byte* dst_frame)
{
	fil_space_decrypt(fil_space_get_crypt_data(space),
			  src_frame, page_size, dst_frame);
}

/*********************************************************************
Verify checksum for a page (iff it's encrypted)
NOTE: currently this function can only be run in single threaded mode
as it modifies srv_checksum_algorithm (temporarily)
@return true if page is encrypted AND OK, false otherwise */
bool
fil_space_verify_crypt_checksum(const byte* src_frame, ulint zip_size)
{
	// key version
	uint key_version = mach_read_from_4(
		src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);

	if (key_version == 0) {
		return false; // unencrypted page
	}

	/* "trick" the normal checksum routines by storing the post-encryption
	* checksum into the normal checksum field allowing for reuse of
	* the normal routines */

	// post encryption checksum
	ib_uint32_t stored_post_encryption = mach_read_from_4(
		src_frame + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 4);

	// save pre encryption checksum for restore in end of this function
	ib_uint32_t stored_pre_encryption = mach_read_from_4(
		src_frame + FIL_PAGE_SPACE_OR_CHKSUM);

	ib_uint32_t checksum_field2 = mach_read_from_4(
		src_frame + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM);

	/** prepare frame for usage of normal checksum routines */
	mach_write_to_4(const_cast<byte*>(src_frame) + FIL_PAGE_SPACE_OR_CHKSUM,
			stored_post_encryption);

	/* NOTE: this function is (currently) only run when restoring
	* dblwr-buffer, server is single threaded so it's safe to modify
	* srv_checksum_algorithm */
	srv_checksum_algorithm_t save_checksum_algorithm =
		(srv_checksum_algorithm_t)srv_checksum_algorithm;
	if (zip_size == 0 &&
	    (save_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_STRICT_INNODB ||
	     save_checksum_algorithm == SRV_CHECKSUM_ALGORITHM_INNODB)) {
		/* handle ALGORITHM_INNODB specially,
		* "downgrade" to ALGORITHM_INNODB and store BUF_NO_CHECKSUM_MAGIC
		* checksum_field2 is sort of pointless anyway...
		*/
		srv_checksum_algorithm = SRV_CHECKSUM_ALGORITHM_INNODB;
		mach_write_to_4(const_cast<byte*>(src_frame) +
				UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM,
				BUF_NO_CHECKSUM_MAGIC);
	}

	/* verify checksums */
	ibool corrupted = buf_page_is_corrupted(false, src_frame, zip_size);

	/** restore frame & algorithm */
	srv_checksum_algorithm = save_checksum_algorithm;

	mach_write_to_4(const_cast<byte*>(src_frame) +
			FIL_PAGE_SPACE_OR_CHKSUM,
			stored_pre_encryption);

	mach_write_to_4(const_cast<byte*>(src_frame) +
			UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM,
			checksum_field2);

	if (!corrupted) {
		return true;  // page was encrypted and checksum matched
	} else {
		return false; // page was encrypted but checksum didn't match
	}
}

/***********************************************************************/

/** A copy of global key state */
struct key_state_t {
	key_state_t() : key_version(0),
			rotate_key_age(srv_fil_crypt_rotate_key_age) {}
	bool operator==(const key_state_t& other) const {
		return key_version == other.key_version &&
			rotate_key_age == other.rotate_key_age;
	}
	uint key_version;
	uint rotate_key_age;
};

/***********************************************************************
Copy global key state */
static void
fil_crypt_get_key_state(
	key_state_t *new_state)
{
	if (srv_encrypt_tables == TRUE) {
		new_state->key_version = get_latest_encryption_key_version();
		new_state->rotate_key_age = srv_fil_crypt_rotate_key_age;
		ut_a(new_state->key_version > 0);
	} else {
		new_state->key_version = 0;
		new_state->rotate_key_age = 0;
	}
}

/***********************************************************************
Check if a key needs rotation given a key_state */
static bool
fil_crypt_needs_rotation(uint key_version, const key_state_t *key_state)
{
	// TODO(jonaso): Add support for rotating encrypted => unencrypted

	if (key_version == 0 && key_state->key_version != 0) {
		/* this is rotation unencrypted => encrypted
		* ignore rotate_key_age */
		return true;
	}

	if (key_state->key_version == 0 && key_version != 0) {
		/* this is rotation encrypted => unencrypted */
		return true;
	}

	/* this is rotation encrypted => encrypted,
	* only reencrypt if key is sufficiently old */
	if (key_version + key_state->rotate_key_age < key_state->key_version)
		return true;

	return false;
}

/***********************************************************************
Check if a space is closing (i.e just before drop) */
UNIV_INTERN bool
fil_crypt_is_closing(ulint space)
{
	bool closing;
	fil_space_crypt_t *crypt_data = fil_space_get_crypt_data(space);
	mutex_enter(&crypt_data->mutex);
	closing = crypt_data->closing;
	mutex_exit(&crypt_data->mutex);
	return closing;
}

/***********************************************************************
Start encrypting a space
@return true if a pending op (fil_inc_pending_ops/fil_decr_pending_ops) is held
*/
static bool
fil_crypt_start_encrypting_space(ulint space, bool *recheck) {

	/* we have a pending op when entering function */
	bool pending_op = true;

	mutex_enter(&fil_crypt_threads_mutex);

	fil_space_crypt_t *crypt_data = fil_space_get_crypt_data(space);
	ibool page_encrypted = fil_space_is_page_encrypted(space);

	/*If spage is not encrypted and encryption is not enabled, then
	do not continue encrypting the space. */
	if (!page_encrypted && !srv_encrypt_tables) {
		mutex_exit(&fil_crypt_threads_mutex);
		return pending_op;
	}

	if (crypt_data != NULL || fil_crypt_start_converting) {
		/* someone beat us to it */
		if (fil_crypt_start_converting) {
			*recheck = true;
		}

		mutex_exit(&fil_crypt_threads_mutex);
		return pending_op;
	}

	/* NOTE: we need to write and flush page 0 before publishing
	* the crypt data. This so that after restart there is no
	* risk of finding encrypted pages without having
	* crypt data in page 0 */

	/* 1 - create crypt data */
	crypt_data = fil_space_create_crypt_data();
	if (crypt_data == NULL) {
		mutex_exit(&fil_crypt_threads_mutex);
		return pending_op;
	}

	crypt_data->type = CRYPT_SCHEME_UNENCRYPTED;
	crypt_data->min_key_version = 0; // all pages are unencrypted
	crypt_data->rotate_state.start_time = time(0);
	crypt_data->rotate_state.starting = true;
	crypt_data->rotate_state.active_threads = 1;

	mutex_enter(&crypt_data->mutex);
	fil_space_set_crypt_data(space, crypt_data);
	mutex_exit(&crypt_data->mutex);

	fil_crypt_start_converting = true;
	mutex_exit(&fil_crypt_threads_mutex);

	do
	{
		if (fil_crypt_is_closing(space) ||
		    fil_tablespace_is_being_deleted(space))
			break;

		mtr_t mtr;
		mtr_start(&mtr);

		/* 2 - get page 0 */
		ulint offset = 0;
		ulint zip_size = fil_space_get_zip_size(space);
		buf_block_t* block = buf_page_get_gen(space, zip_size, offset,
						      RW_X_LATCH,
						      NULL,
						      BUF_GET,
						      __FILE__, __LINE__,
						      &mtr);

		if (fil_crypt_is_closing(space) ||
		    fil_tablespace_is_being_deleted(space)) {
			mtr_commit(&mtr);
			break;
		}

		/* 3 - compute location to store crypt data */
		byte* frame = buf_block_get_frame(block);
		ulint maxsize;
		crypt_data->page0_offset =
			fsp_header_get_crypt_offset(zip_size, &maxsize);

		/* 4 - write crypt data to page 0 */
		fil_space_write_crypt_data_low(crypt_data,
					       CRYPT_SCHEME_1,
					       frame,
					       crypt_data->page0_offset,
					       maxsize, &mtr);

		mtr_commit(&mtr);

		if (fil_crypt_is_closing(space) ||
		    fil_tablespace_is_being_deleted(space)) {
			break;
		}

		/* record lsn of update */
		lsn_t end_lsn = mtr.end_lsn;

		/* 4 - sync tablespace before publishing crypt data */

		/* release "lock" while syncing */
		fil_decr_pending_ops(space);
		pending_op = false;

		bool success = false;
		ulint n_pages = 0;
		ulint sum_pages = 0;
		do {
			success = buf_flush_list(ULINT_MAX, end_lsn, &n_pages);
			buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);
			sum_pages += n_pages;
		} while (!success &&
			 !fil_crypt_is_closing(space) &&
			 !fil_tablespace_is_being_deleted(space));

		/* try to reacquire pending op */
		if (fil_inc_pending_ops(space, true))
			break;

		/* pending op reacquired! */
		pending_op = true;

		if (fil_crypt_is_closing(space) ||
		    fil_tablespace_is_being_deleted(space)) {
			break;
		}

		/* 5 - publish crypt data */
		mutex_enter(&fil_crypt_threads_mutex);
		mutex_enter(&crypt_data->mutex);
		crypt_data->type = CRYPT_SCHEME_1;
		ut_a(crypt_data->rotate_state.active_threads == 1);
		crypt_data->rotate_state.active_threads = 0;
		crypt_data->rotate_state.starting = false;

		fil_crypt_start_converting = false;
		mutex_exit(&crypt_data->mutex);
		mutex_exit(&fil_crypt_threads_mutex);

		return pending_op;
	} while (0);

	mutex_enter(&crypt_data->mutex);
	ut_a(crypt_data->rotate_state.active_threads == 1);
	crypt_data->rotate_state.active_threads = 0;
	mutex_exit(&crypt_data->mutex);

	mutex_enter(&fil_crypt_threads_mutex);
	fil_crypt_start_converting = false;
	mutex_exit(&fil_crypt_threads_mutex);

	return pending_op;
}

/***********************************************************************
Check if space needs rotation given a key_state */
static bool
fil_crypt_space_needs_rotation(uint space, const key_state_t *key_state,
			       bool *recheck)
{
	if (fil_space_get_type(space) != FIL_TABLESPACE) {
		return false;
	}

	if (fil_inc_pending_ops(space, true)) {
		/* tablespace being dropped */
		return false;
	}

	/* keep track of if we have pending op */
	bool pending_op = true;

	fil_space_crypt_t *crypt_data = fil_space_get_crypt_data(space);

	if (crypt_data == NULL) {
		/**
		* space has no crypt data
		*   start encrypting it...
		*/
		pending_op = fil_crypt_start_encrypting_space(space, recheck);

		crypt_data = fil_space_get_crypt_data(space);

		if (crypt_data == NULL) {
			if (pending_op) {
				fil_decr_pending_ops(space);
			}
			return false;
		}
	}

	mutex_enter(&crypt_data->mutex);

	do {
		/* prevent threads from starting to rotate space */
		if (crypt_data->rotate_state.starting) {
			/* recheck this space later */
			*recheck = true;
			break;
		}

		/* prevent threads from starting to rotate space */
		if (crypt_data->closing)
			break;

		if (crypt_data->rotate_state.flushing)
			break;

		bool need_key_rotation = fil_crypt_needs_rotation(
			crypt_data->min_key_version, key_state);

		time_t diff = time(0) - crypt_data->rotate_state.scrubbing.
			last_scrub_completed;
		bool need_scrubbing =
			diff >= srv_background_scrub_data_interval;

		if (need_key_rotation == false && need_scrubbing == false)
			break;

		mutex_exit(&crypt_data->mutex);
		/* NOTE! fil_decr_pending_ops is performed outside */
		return true;
	} while (0);

	mutex_exit(&crypt_data->mutex);
	if (pending_op) {
		fil_decr_pending_ops(space);
	}
	return false;
}

/** State of a rotation thread */
struct rotate_thread_t {
	explicit rotate_thread_t(uint no) {
		memset(this, 0, sizeof(* this));
		thread_no = no;
		first = true;
		estimated_max_iops = 20;
	}

	uint thread_no;
	bool first;		    /*!< is position before first space */
	ulint space;		    /*!< current space */
	ulint offset;		    /*!< current offset */
	ulint batch;		    /*!< #pages to rotate */
	uint  min_key_version_found;/*!< min key version found but not rotated */
	lsn_t end_lsn;		    /*!< max lsn when rotating this space */

	uint estimated_max_iops;   /*!< estimation of max iops */
	uint allocated_iops;	   /*!< allocated iops */
	uint cnt_waited;	   /*!< #times waited during this slot */
	uint sum_waited_us;	   /*!< wait time during this slot */

	fil_crypt_stat_t crypt_stat; // statistics

	btr_scrub_t scrub_data;      /* thread local data used by btr_scrub-functions
				     * when iterating pages of tablespace */

	/* check if this thread should shutdown */
	bool should_shutdown() const {
		return ! (srv_shutdown_state == SRV_SHUTDOWN_NONE &&
			  thread_no < srv_n_fil_crypt_threads);
	}
};

/***********************************************************************
Update global statistics with thread statistics */
static void
fil_crypt_update_total_stat(rotate_thread_t *state)
{
	mutex_enter(&crypt_stat_mutex);
	crypt_stat.pages_read_from_cache +=
		state->crypt_stat.pages_read_from_cache;
	crypt_stat.pages_read_from_disk +=
		state->crypt_stat.pages_read_from_disk;
	crypt_stat.pages_modified += state->crypt_stat.pages_modified;
	crypt_stat.pages_flushed += state->crypt_stat.pages_flushed;
	// remote old estimate
	crypt_stat.estimated_iops -= state->crypt_stat.estimated_iops;
	// add new estimate
	crypt_stat.estimated_iops += state->estimated_max_iops;
	mutex_exit(&crypt_stat_mutex);

	// make new estimate "current" estimate
	memset(&state->crypt_stat, 0, sizeof(state->crypt_stat));
	// record our old (current) estimate
	state->crypt_stat.estimated_iops = state->estimated_max_iops;
}

/***********************************************************************
Allocate iops to thread from global setting,
used before starting to rotate a space */
static bool
fil_crypt_alloc_iops(rotate_thread_t *state)
{
	ut_ad(state->allocated_iops == 0);

	uint max_iops = state->estimated_max_iops;
	mutex_enter(&fil_crypt_threads_mutex);
	if (n_fil_crypt_iops_allocated >= srv_n_fil_crypt_iops) {
		/* this can happen when user decreases srv_fil_crypt_iops */
		mutex_exit(&fil_crypt_threads_mutex);
		return false;
	}

	uint alloc = srv_n_fil_crypt_iops - n_fil_crypt_iops_allocated;
	if (alloc > max_iops)
		alloc = max_iops;

	n_fil_crypt_iops_allocated += alloc;
	mutex_exit(&fil_crypt_threads_mutex);

	state->allocated_iops = alloc;

	return alloc > 0;
}

/***********************************************************************
Reallocate iops to thread,
used when inside a space */
static void
fil_crypt_realloc_iops(rotate_thread_t *state)
{
	ut_a(state->allocated_iops > 0);

	if (10 * state->cnt_waited > state->batch) {
		/* if we waited more than 10% re-estimate max_iops */
		uint avg_wait_time_us =
			state->sum_waited_us / state->cnt_waited;

#if DEBUG_KEYROTATION_THROTTLING
		ib_logf(IB_LOG_LEVEL_INFO,
			"thr_no: %u - update estimated_max_iops from %u to %u.",
			state->thread_no,
			state->estimated_max_iops,
			1000000 / avg_wait_time_us);
#endif
		if (avg_wait_time_us == 0)
			avg_wait_time_us = 1; // prevent division by zero

		state->estimated_max_iops = 1000000 / avg_wait_time_us;
		state->cnt_waited = 0;
		state->sum_waited_us = 0;
	} else {
#if DEBUG_KEYROTATION_THROTTLING
		ib_logf(IB_LOG_LEVEL_INFO,
			"thr_no: %u only waited %lu%% skip re-estimate.",
			state->thread_no,
			(100 * state->cnt_waited) / state->batch);
#endif
	}

	if (state->estimated_max_iops <= state->allocated_iops) {
		/* return extra iops */
		uint extra = state->allocated_iops - state->estimated_max_iops;

		if (extra > 0) {
			mutex_enter(&fil_crypt_threads_mutex);
			if (n_fil_crypt_iops_allocated < extra) {
				/* unknown bug!
				* crash in debug
				* keep n_fil_crypt_iops_allocated unchanged
				* in release */
				ut_ad(0);
				extra = 0;
			}
			n_fil_crypt_iops_allocated -= extra;
			state->allocated_iops -= extra;

			if (state->allocated_iops == 0) {
				/* no matter how slow io system seems to be
				* never decrease allocated_iops to 0... */
				state->allocated_iops ++;
				n_fil_crypt_iops_allocated ++;
			}
			mutex_exit(&fil_crypt_threads_mutex);
			os_event_set(fil_crypt_threads_event);
		}
	} else {
		/* see if there are more to get */
		mutex_enter(&fil_crypt_threads_mutex);
		if (n_fil_crypt_iops_allocated < srv_n_fil_crypt_iops) {
			/* there are extra iops free */
			uint extra = srv_n_fil_crypt_iops -
				n_fil_crypt_iops_allocated;
			if (state->allocated_iops + extra >
			    state->estimated_max_iops) {
				/* but don't alloc more than our max */
				extra = state->estimated_max_iops -
					state->allocated_iops;
			}
			n_fil_crypt_iops_allocated += extra;
			state->allocated_iops += extra;
#if DEBUG_KEYROTATION_THROTTLING
			ib_logf(IB_LOG_LEVEL_INFO,
				"thr_no: %u increased iops from %u to %u.",
				state->thread_no,
				state->allocated_iops - extra,
				state->allocated_iops);
#endif
		}
		mutex_exit(&fil_crypt_threads_mutex);
	}

	fil_crypt_update_total_stat(state);
}

/***********************************************************************
Return allocated iops to global */
static void
fil_crypt_return_iops(rotate_thread_t *state)
{
	if (state->allocated_iops > 0) {
		uint iops = state->allocated_iops;
		mutex_enter(&fil_crypt_threads_mutex);
		if (n_fil_crypt_iops_allocated < iops) {
			/* unknown bug!
			* crash in debug
			* keep n_fil_crypt_iops_allocated unchanged
			* in release */
			ut_ad(0);
			iops = 0;
		}
		n_fil_crypt_iops_allocated -= iops;
		mutex_exit(&fil_crypt_threads_mutex);
		state->allocated_iops = 0;
		os_event_set(fil_crypt_threads_event);
	}

	fil_crypt_update_total_stat(state);
}

/***********************************************************************
Search for a space needing rotation */
bool
fil_crypt_find_space_to_rotate(
	const key_state_t *key_state,
	rotate_thread_t *state,
	bool *recheck)
{
	/* we need iops to start rotating */
	while (!state->should_shutdown() && !fil_crypt_alloc_iops(state)) {
		os_event_reset(fil_crypt_threads_event);
		os_event_wait_time(fil_crypt_threads_event, 1000000);
	}

	if (state->should_shutdown())
		return false;

	if (state->first) {
		state->first = false;
		state->space = fil_get_first_space();
	} else {
		state->space = fil_get_next_space(state->space);
	}

	while (!state->should_shutdown() && state->space != ULINT_UNDEFINED) {

		ulint space = state->space;
		if (fil_crypt_space_needs_rotation(space, key_state, recheck)) {
			/* init state->min_key_version_found before
			* starting on a space */
			state->min_key_version_found = key_state->key_version;
			return true;
		}

		state->space = fil_get_next_space(space);
	}

	/* if we didn't find any space return iops */
	fil_crypt_return_iops(state);

	return false;

}

/***********************************************************************
Start rotating a space */
static
void
fil_crypt_start_rotate_space(
	const key_state_t *key_state,
	rotate_thread_t *state)
{
	ulint space = state->space;
	fil_space_crypt_t *crypt_data = fil_space_get_crypt_data(space);
	mutex_enter(&crypt_data->mutex);
	if (crypt_data->rotate_state.active_threads == 0) {
		/* only first thread needs to init */
		crypt_data->rotate_state.next_offset = 1; // skip page 0
		/* no need to rotate beyond current max
		* if space extends, it will be encrypted with newer version */
		crypt_data->rotate_state.max_offset = fil_space_get_size(space);

		crypt_data->rotate_state.end_lsn = 0;
		crypt_data->rotate_state.min_key_version_found =
			key_state->key_version;

		crypt_data->rotate_state.start_time = time(0);
	}

	/* count active threads in space */
	crypt_data->rotate_state.active_threads++;

	/* Initialize thread local state */
	state->end_lsn = crypt_data->rotate_state.end_lsn;
	state->min_key_version_found =
		crypt_data->rotate_state.min_key_version_found;

	/* inform scrubbing */
	crypt_data->rotate_state.scrubbing.is_active =
		btr_scrub_start_space(space, &state->scrub_data);

	mutex_exit(&crypt_data->mutex);
}

/***********************************************************************
Search for batch of pages needing rotation */
static
bool
fil_crypt_find_page_to_rotate(
	const key_state_t *key_state,
	rotate_thread_t *state)
{
	ulint batch = srv_alloc_time * state->allocated_iops;
	ulint space = state->space;
	fil_space_crypt_t *crypt_data = fil_space_get_crypt_data(space);
	mutex_enter(&crypt_data->mutex);
	if (crypt_data->closing == false &&
	    crypt_data->rotate_state.next_offset <
	    crypt_data->rotate_state.max_offset) {

		state->offset = crypt_data->rotate_state.next_offset;
		ulint remaining = crypt_data->rotate_state.max_offset -
			crypt_data->rotate_state.next_offset;

		if (batch <= remaining)
			state->batch = batch;
		else
			state->batch = remaining;

		crypt_data->rotate_state.next_offset += batch;
		mutex_exit(&crypt_data->mutex);
		return true;
	}

	mutex_exit(&crypt_data->mutex);
	return false;
}

/***********************************************************************
Check if a page is uninitialized (doesn't need to be rotated) */
static bool
fil_crypt_is_page_uninitialized(const byte* frame, uint zip_size)
{
	if (zip_size) {
		ulint stored_checksum = mach_read_from_4(
			frame + FIL_PAGE_SPACE_OR_CHKSUM);
		/* empty pages aren't encrypted */
		if (stored_checksum == 0) {
			return true;
		}
	} else {
		ulint size = UNIV_PAGE_SIZE;
		ulint checksum_field1 = mach_read_from_4(
			frame + FIL_PAGE_SPACE_OR_CHKSUM);
		ulint checksum_field2 = mach_read_from_4(
			frame + size - FIL_PAGE_END_LSN_OLD_CHKSUM);
		/* empty pages are not encrypted */
		if (checksum_field1 == 0 && checksum_field2 == 0
		    && mach_read_from_4(frame + FIL_PAGE_LSN) == 0) {
			return true;
		}
	}
	return false;
}

#define fil_crypt_get_page_throttle(state,space,zip_size,offset,mtr,sleeptime_ms) \
	fil_crypt_get_page_throttle_func(state, space, zip_size, offset, mtr, \
					 sleeptime_ms, __FILE__, __LINE__)

/***********************************************************************
Get a page and compute sleep time  */
static
buf_block_t*
fil_crypt_get_page_throttle_func(rotate_thread_t *state,
				 ulint space, uint zip_size, ulint offset,
				 mtr_t *mtr,
				 ulint *sleeptime_ms,
				 const char *file,
				 ulint line)
{
	buf_block_t* block = buf_page_try_get_func(space, offset, RW_X_LATCH,
						   true,
						   file, line, mtr);
	if (block != NULL) {
		/* page was in buffer pool */
		state->crypt_stat.pages_read_from_cache++;
		return block;
	}

	state->crypt_stat.pages_read_from_disk++;

	ullint start = ut_time_us(NULL);
	block = buf_page_get_gen(space, zip_size, offset,
				 RW_X_LATCH,
				 NULL, BUF_GET_POSSIBLY_FREED,
				 file, line, mtr);
	ullint end = ut_time_us(NULL);

	if (end < start) {
		end = start; // safety...
	}

	state->cnt_waited++;
	state->sum_waited_us += (end - start);

	/* average page load */
	ulint add_sleeptime_ms = 0;
	ulint avg_wait_time_us = state->sum_waited_us / state->cnt_waited;
	ulint alloc_wait_us = 1000000 / state->allocated_iops;
	if (avg_wait_time_us < alloc_wait_us) {
		/* we reading faster than we allocated */
		add_sleeptime_ms = (alloc_wait_us - avg_wait_time_us) / 1000;
	} else {
		/* if page load time is longer than we want, skip sleeping */
	}

	*sleeptime_ms += add_sleeptime_ms;
	return block;
}


/***********************************************************************
Get block and allocation status

note: innodb locks fil_space_latch and then block when allocating page
but locks block and then fil_space_latch when freeing page.
*/
static
buf_block_t*
btr_scrub_get_block_and_allocation_status(
	rotate_thread_t *state,
	ulint space,
	ulint zip_size,
	ulint offset,
	mtr_t *mtr,
	btr_scrub_page_allocation_status_t *allocation_status,
	ulint *sleeptime_ms)
{
	mtr_t local_mtr;
	buf_block_t *block = NULL;
	mtr_start(&local_mtr);
	*allocation_status = fsp_page_is_free(space, offset, &local_mtr) ?
		BTR_SCRUB_PAGE_FREE :
		BTR_SCRUB_PAGE_ALLOCATED;

	if (*allocation_status == BTR_SCRUB_PAGE_FREE) {
		/* this is easy case, we lock fil_space_latch first and
		then block */
		block = fil_crypt_get_page_throttle(state,
						    space, zip_size,
						    offset, mtr,
						    sleeptime_ms);
		mtr_commit(&local_mtr);
	} else {
		/* page is allocated according to xdes */

		/* release fil_space_latch *before* fetching block */
		mtr_commit(&local_mtr);

		/* NOTE: when we have locked dict_index_get_lock(),
		* it's safe to release fil_space_latch and then fetch block
		* as dict_index_get_lock() is needed to make tree modifications
		* such as free-ing a page
		*/

		block = fil_crypt_get_page_throttle(state,
						    space, zip_size,
						    offset, mtr,
						    sleeptime_ms);
	}

	return block;
}


/***********************************************************************
Rotate one page */
static
void
fil_crypt_rotate_page(
	const key_state_t *key_state,
	rotate_thread_t *state)
{
	ulint space = state->space;
	ulint offset = state->offset;
	const uint zip_size = fil_space_get_zip_size(space);
	ulint sleeptime_ms = 0;

	/* check if tablespace is closing before reading page */
	if (fil_crypt_is_closing(space)) {
		return;
	}

	if (space == TRX_SYS_SPACE && offset == TRX_SYS_PAGE_NO) {
		/* don't encrypt this as it contains address to dblwr buffer */
		return;
	}

	mtr_t mtr;
	mtr_start(&mtr);
	buf_block_t* block = fil_crypt_get_page_throttle(state,
							 space, zip_size,
							 offset, &mtr,
							 &sleeptime_ms);

	bool modified = false;
	int needs_scrubbing = BTR_SCRUB_SKIP_PAGE;
	lsn_t block_lsn = block->page.newest_modification;
	uint kv =  block->page.key_version;

	/* check if tablespace is closing after reading page */
	if (!fil_crypt_is_closing(space)) {
		byte* frame = buf_block_get_frame(block);
		fil_space_crypt_t *crypt_data = fil_space_get_crypt_data(space);

		if (kv == 0 &&
		    fil_crypt_is_page_uninitialized(frame, zip_size)) {
			;
		} else if (fil_crypt_needs_rotation(kv, key_state)) {

			/* page can be "fresh" i.e never written in case
			* kv == 0 or it should have a key version at least
			* as big as the space minimum key version*/
			ut_a(kv == 0 || kv >= crypt_data->min_key_version);

			modified = true;

			/* force rotation by dummy updating page */
			mlog_write_ulint(frame +
					 FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
					 space, MLOG_4BYTES, &mtr);

			/* update block */
			block->page.key_version = key_state->key_version;

			/* statistics */
			state->crypt_stat.pages_modified++;
		} else {
			ut_a(kv >= crypt_data->min_key_version ||
			     (kv == 0 && key_state->key_version == 0));

			if (kv < state->min_key_version_found) {
				state->min_key_version_found = kv;
			}
		}

		needs_scrubbing = btr_page_needs_scrubbing(
			&state->scrub_data, block,
			BTR_SCRUB_PAGE_ALLOCATION_UNKNOWN);
	}

	mtr_commit(&mtr);
	lsn_t end_lsn = mtr.end_lsn;

	if (needs_scrubbing == BTR_SCRUB_PAGE) {
		mtr_start(&mtr);
		/*
		* refetch page and allocation status
		*/
		btr_scrub_page_allocation_status_t allocated;
		block = btr_scrub_get_block_and_allocation_status(
			state, space, zip_size, offset, &mtr,
			&allocated,
			&sleeptime_ms);

		/* get required table/index and index-locks */
		needs_scrubbing = btr_scrub_recheck_page(
			&state->scrub_data, block, allocated, &mtr);

		if (needs_scrubbing == BTR_SCRUB_PAGE) {
			/* we need to refetch it once more now that we have
			* index locked */
			block = btr_scrub_get_block_and_allocation_status(
				state, space, zip_size, offset, &mtr,
				&allocated,
				&sleeptime_ms);

			needs_scrubbing = btr_scrub_page(&state->scrub_data,
							 block, allocated,
							 &mtr);
		}

		/* NOTE: mtr is committed inside btr_scrub_recheck_page()
		* and/or btr_scrub_page. This is to make sure that
		* locks & pages are latched in corrected order,
		* the mtr is in some circumstances restarted.
		* (mtr_commit() + mtr_start())
		*/
	}

	if (needs_scrubbing != BTR_SCRUB_PAGE) {
		/* if page didn't need scrubbing it might be that cleanups
		are needed. do those outside of any mtr to prevent deadlocks.

		the information what kinds of cleanups that are needed are
		encoded inside the needs_scrubbing, but this is opaque to
		this function (except the value BTR_SCRUB_PAGE) */
		btr_scrub_skip_page(&state->scrub_data, needs_scrubbing);
	}

	if (needs_scrubbing == BTR_SCRUB_TURNED_OFF) {
		/* if we just detected that scrubbing was turned off
		* update global state to reflect this */
		fil_space_crypt_t *crypt_data = fil_space_get_crypt_data(space);
		mutex_enter(&crypt_data->mutex);
		crypt_data->rotate_state.scrubbing.is_active = false;
		mutex_exit(&crypt_data->mutex);
	}

	if (modified) {
		/* if we modified page, we take lsn from mtr */
		ut_a(end_lsn > state->end_lsn);
		ut_a(end_lsn > block_lsn);
		state->end_lsn = end_lsn;
	} else {
		/* if we did not modify page, check for max lsn */
		if (block_lsn > state->end_lsn) {
			state->end_lsn = block_lsn;
		}
	}

	if (sleeptime_ms) {
		os_event_reset(fil_crypt_throttle_sleep_event);
		os_event_wait_time(fil_crypt_throttle_sleep_event,
				   1000 * sleeptime_ms);
	}
}

/***********************************************************************
Rotate a batch of pages */
static
void
fil_crypt_rotate_pages(
	const key_state_t *key_state,
	rotate_thread_t *state)
{
	ulint space = state->space;
	ulint end = state->offset + state->batch;
	for (; state->offset < end; state->offset++) {

		/* we can't rotate pages in dblwr buffer as
		* it's not possible to read those due to lots of asserts
		* in buffer pool.
		*
		* However since these are only (short-lived) copies of
		* real pages, they will be updated anyway when the
		* real page is updated
		*/
		if (space == TRX_SYS_SPACE &&
		    buf_dblwr_page_inside(state->offset)) {
			continue;
		}

		fil_crypt_rotate_page(key_state, state);
	}
}

/***********************************************************************
Flush rotated pages and then update page 0 */
static
void
fil_crypt_flush_space(rotate_thread_t *state, ulint space)
{
	fil_space_crypt_t *crypt_data = fil_space_get_crypt_data(space);

	/* flush tablespace pages so that there are no pages left with old key */
	lsn_t end_lsn = crypt_data->rotate_state.end_lsn;
	if (end_lsn > 0 && !fil_crypt_is_closing(space)) {
		bool success = false;
		ulint n_pages = 0;
		ulint sum_pages = 0;
		ullint start = ut_time_us(NULL);
		do {
			success = buf_flush_list(ULINT_MAX, end_lsn, &n_pages);
			buf_flush_wait_batch_end(NULL, BUF_FLUSH_LIST);
			sum_pages += n_pages;
		} while (!success && !fil_crypt_is_closing(space));
		ullint end = ut_time_us(NULL);
		if (sum_pages && end > start) {
			state->cnt_waited += sum_pages;
			state->sum_waited_us += (end - start);

			/* statistics */
			state->crypt_stat.pages_flushed += sum_pages;
		}
	}

	if (crypt_data->min_key_version == 0) {
		crypt_data->type = CRYPT_SCHEME_UNENCRYPTED;
	}

	/* update page 0 */
	if (!fil_crypt_is_closing(space)) {
		mtr_t mtr;
		mtr_start(&mtr);
		ulint offset = 0; // page 0
		const uint zip_size = fil_space_get_zip_size(space);
		buf_block_t* block = buf_page_get_gen(space, zip_size, offset,
						      RW_X_LATCH, NULL, BUF_GET,
						      __FILE__, __LINE__, &mtr);
		byte* frame = buf_block_get_frame(block);
		fil_space_write_crypt_data(space, frame,
					   crypt_data->page0_offset,
					   ULINT_MAX, &mtr);
		mtr_commit(&mtr);
	}
}

/***********************************************************************
Complete rotating a space */
static
void
fil_crypt_complete_rotate_space(
	const key_state_t *key_state,
	rotate_thread_t *state)
{
	ulint space = state->space;
	fil_space_crypt_t *crypt_data = fil_space_get_crypt_data(space);
	mutex_enter(&crypt_data->mutex);

	/**
	* Update crypt data state with state from thread
	*/
	if (state->min_key_version_found <
	    crypt_data->rotate_state.min_key_version_found) {
		crypt_data->rotate_state.min_key_version_found =
			state->min_key_version_found;
	}

	if (state->end_lsn > crypt_data->rotate_state.end_lsn) {
		crypt_data->rotate_state.end_lsn = state->end_lsn;
	}

	ut_a(crypt_data->rotate_state.active_threads > 0);
	crypt_data->rotate_state.active_threads--;
	bool last = crypt_data->rotate_state.active_threads == 0;

	/**
	* check if space is fully done
	* this as when threads shutdown, it could be that we "complete"
	* iterating before we have scanned the full space.
	*/
	bool done = crypt_data->rotate_state.next_offset >=
		crypt_data->rotate_state.max_offset;

	/**
	* we should flush space if we're last thread AND
	* the iteration is done
	*/
	bool should_flush = last && done;

	if (should_flush) {
		/* we're the last active thread */
		crypt_data->rotate_state.flushing = true;
		crypt_data->min_key_version =
			crypt_data->rotate_state.min_key_version_found;
	}

	/* inform scrubbing */
	crypt_data->rotate_state.scrubbing.is_active = false;
	mutex_exit(&crypt_data->mutex);

	/* all threads must call btr_scrub_complete_space wo/ mutex held */
	if (btr_scrub_complete_space(&state->scrub_data) == true) {
		if (should_flush) {
			/* only last thread updates last_scrub_completed */
			mutex_enter(&crypt_data->mutex);
			crypt_data->rotate_state.scrubbing.
				last_scrub_completed = time(0);
			mutex_exit(&crypt_data->mutex);
		}
	}

	if (should_flush) {
		fil_crypt_flush_space(state, space);

		mutex_enter(&crypt_data->mutex);
		crypt_data->rotate_state.flushing = false;
		mutex_exit(&crypt_data->mutex);
	}
}

/*********************************************************************//**
A thread which monitors global key state and rotates tablespaces accordingly
@return a dummy parameter */
extern "C" UNIV_INTERN
os_thread_ret_t
DECLARE_THREAD(fil_crypt_thread)(
/*===============================*/
	void*	arg __attribute__((unused))) /*!< in: a dummy parameter required
					     * by os_thread_create */
{
	UT_NOT_USED(arg);

	mutex_enter(&fil_crypt_threads_mutex);
	uint thread_no = srv_n_fil_crypt_threads_started;
	srv_n_fil_crypt_threads_started++;
	mutex_exit(&fil_crypt_threads_mutex);
	os_event_set(fil_crypt_event); /* signal that we started */

	/* state of this thread */
	rotate_thread_t thr(thread_no);

	/* if we find a space that is starting, skip over it and recheck it later */
	bool recheck = false;

	key_state_t key_state;
	fil_crypt_get_key_state(&key_state);

	/* make sure that thread always checks all tablespace when starting.
	*
	* by decreasing key_version, loop that waits for change in key-state
	* should exit immediately causing thread to check all spaces when starting */
	key_state.key_version--;

	while (!thr.should_shutdown()) {

		key_state_t new_state;
		fil_crypt_get_key_state(&new_state);

		time_t wait_start = time(0);
		while (!thr.should_shutdown() && key_state == new_state) {

			/* wait for key state changes
			* i.e either new key version of change or
			* new rotate_key_age */
			os_event_reset(fil_crypt_threads_event);
			os_event_wait_time(fil_crypt_threads_event, 1000000);
			fil_crypt_get_key_state(&new_state);

			if (recheck) {
				/* check recheck here, after sleep, so
				* that we don't busy loop while when one thread is starting
				* a space*/
				break;
			}

			time_t waited = time(0) - wait_start;
			if (waited >= srv_background_scrub_data_check_interval)
				break;
		}

		recheck = false;
		thr.first = true;      // restart from first tablespace
		key_state = new_state; // save for next loop

		/* iterate all spaces searching for those needing rotation */
		while (!thr.should_shutdown() &&
		       fil_crypt_find_space_to_rotate(&new_state, &thr, &recheck)) {

			/* we found a space to rotate */
			fil_crypt_start_rotate_space(&new_state, &thr);

			/* decrement pending ops that was incremented in
			* fil_crypt_space_needs_rotation
			* (called from fil_crypt_find_space_to_rotate),
			* this makes sure that tablespace won't be dropped
			* just after we decided to start processing it. */
			fil_decr_pending_ops(thr.space);

			/* iterate all pages (cooperativly with other threads) */
			while (!thr.should_shutdown() &&
			       fil_crypt_find_page_to_rotate(&new_state, &thr)) {

				/* rotate a (set) of pages */
				fil_crypt_rotate_pages(&new_state, &thr);

				/* realloc iops */
				fil_crypt_realloc_iops(&thr);
			}

			/* complete rotation */
			fil_crypt_complete_rotate_space(&new_state, &thr);

			/* refresh key state */
			fil_crypt_get_key_state(&new_state);

			/* return iops */
			fil_crypt_return_iops(&thr);
		}
	}

	/* return iops if shutting down */
	fil_crypt_return_iops(&thr);

	mutex_enter(&fil_crypt_threads_mutex);
	srv_n_fil_crypt_threads_started--;
	mutex_exit(&fil_crypt_threads_mutex);
	os_event_set(fil_crypt_event); /* signal that we stopped */

	/* We count the number of threads in os_thread_exit(). A created
	thread should always use that to exit and not use return() to exit. */

	os_thread_exit(NULL);

	OS_THREAD_DUMMY_RETURN;
}

/*********************************************************************
Adjust thread count for key rotation */
UNIV_INTERN
void
fil_crypt_set_thread_cnt(uint new_cnt) {
	if (new_cnt > srv_n_fil_crypt_threads) {
		uint add = new_cnt - srv_n_fil_crypt_threads;
		srv_n_fil_crypt_threads = new_cnt;
		for (uint i = 0; i < add; i++) {
			os_thread_create(fil_crypt_thread, NULL, NULL);
		}
	} else if (new_cnt < srv_n_fil_crypt_threads) {
		srv_n_fil_crypt_threads = new_cnt;
		os_event_set(fil_crypt_threads_event);
	}

	while(srv_n_fil_crypt_threads_started != srv_n_fil_crypt_threads) {
		os_event_reset(fil_crypt_event);
		os_event_wait_time(fil_crypt_event, 1000000);
	}
}

/*********************************************************************
Adjust max key age */
UNIV_INTERN
void
fil_crypt_set_rotate_key_age(uint val)
{
	srv_fil_crypt_rotate_key_age = val;
	os_event_set(fil_crypt_threads_event);
}

/*********************************************************************
Adjust rotation iops */
UNIV_INTERN
void
fil_crypt_set_rotation_iops(uint val)
{
	srv_n_fil_crypt_iops = val;
	os_event_set(fil_crypt_threads_event);
}

/*********************************************************************
Init threads for key rotation */
UNIV_INTERN
void
fil_crypt_threads_init()
{
	fil_crypt_event = os_event_create();
	fil_crypt_threads_event = os_event_create();
	mutex_create(fil_crypt_threads_mutex_key,
		     &fil_crypt_threads_mutex, SYNC_NO_ORDER_CHECK);

	uint cnt = srv_n_fil_crypt_threads;
	srv_n_fil_crypt_threads = 0;
	fil_crypt_set_thread_cnt(cnt);
}

/*********************************************************************
End threads for key rotation */
UNIV_INTERN
void
fil_crypt_threads_end()
{
	/* stop threads */
	fil_crypt_set_thread_cnt(0);
}

/*********************************************************************
Clean up key rotation threads resources */
UNIV_INTERN
void
fil_crypt_threads_cleanup() {
	os_event_free(fil_crypt_event);
	os_event_free(fil_crypt_threads_event);
}

/*********************************************************************
Mark a space as closing */
UNIV_INTERN
void
fil_space_crypt_mark_space_closing(
	ulint space)
{
	mutex_enter(&fil_crypt_threads_mutex);
	fil_space_crypt_t* crypt_data = fil_space_get_crypt_data(space);
	if (crypt_data == NULL) {
		mutex_exit(&fil_crypt_threads_mutex);
		return;
	}

	mutex_enter(&crypt_data->mutex);
	mutex_exit(&fil_crypt_threads_mutex);
	crypt_data->closing = true;
	mutex_exit(&crypt_data->mutex);
}

/*********************************************************************
Wait for crypt threads to stop accessing space */
UNIV_INTERN
void
fil_space_crypt_close_tablespace(
	ulint space)
{
	mutex_enter(&fil_crypt_threads_mutex);
	fil_space_crypt_t* crypt_data = fil_space_get_crypt_data(space);
	if (crypt_data == NULL) {
		mutex_exit(&fil_crypt_threads_mutex);
		return;
	}

	uint start = time(0);
	uint last = start;
	mutex_enter(&crypt_data->mutex);
	mutex_exit(&fil_crypt_threads_mutex);
	crypt_data->closing = true;
	uint cnt = crypt_data->rotate_state.active_threads;
	bool flushing = crypt_data->rotate_state.flushing;
	while (cnt > 0 || flushing) {
		mutex_exit(&crypt_data->mutex);
		/* release dict mutex so that scrub threads can release their
		* table references */
		dict_mutex_exit_for_mysql();
		/* wakeup throttle (all) sleepers */
		os_event_set(fil_crypt_throttle_sleep_event);
		os_thread_sleep(20000);
		dict_mutex_enter_for_mysql();
		mutex_enter(&crypt_data->mutex);
		cnt = crypt_data->rotate_state.active_threads;
		flushing = crypt_data->rotate_state.flushing;

		uint now = time(0);
		if (now >= last + 30) {
			ib_logf(IB_LOG_LEVEL_WARN,
				"Waited %u seconds to drop space: %lu.",
				now - start, space);
			last = now;
		}
	}
	mutex_exit(&crypt_data->mutex);
}

/*********************************************************************
Get crypt status for a space (used by information_schema)
return 0 if crypt data present */
int
fil_space_crypt_get_status(
/*==================*/
	ulint id,				  /*!< in: space id */
	struct fil_space_crypt_status_t* status) /*!< out: status  */
{
	fil_space_crypt_t* crypt_data = fil_space_get_crypt_data(id);

	if (crypt_data != NULL) {
		status->space = id;
		status->scheme = crypt_data->type;
		mutex_enter(&crypt_data->mutex);
		status->keyserver_requests = crypt_data->keyserver_requests;
		status->min_key_version = crypt_data->min_key_version;
		if (crypt_data->rotate_state.active_threads > 0 ||
		    crypt_data->rotate_state.flushing) {
			status->rotating = true;
			status->flushing =
				crypt_data->rotate_state.flushing;
			status->rotate_next_page_number =
				crypt_data->rotate_state.next_offset;
			status->rotate_max_page_number =
				crypt_data->rotate_state.max_offset;
		} else {
			status->rotating = false;
		}
		mutex_exit(&crypt_data->mutex);
	} else {
		memset(status, 0, sizeof(*status));
	}

	if (srv_encrypt_tables == TRUE) {
		status->current_key_version = get_latest_encryption_key_version();
	} else {
		status->current_key_version = 0;
	}
	return crypt_data == NULL ? 1 : 0;
}

/*********************************************************************
Return crypt statistics */
void
fil_crypt_total_stat(fil_crypt_stat_t *stat)
{
	mutex_enter(&crypt_stat_mutex);
	*stat = crypt_stat;
	mutex_exit(&crypt_stat_mutex);
}

/*********************************************************************
Get scrub status for a space (used by information_schema)
return 0 if data found */
int
fil_space_get_scrub_status(
/*==================*/
	ulint id,				  /*!< in: space id */
	struct fil_space_scrub_status_t* status) /*!< out: status  */
{
	fil_space_crypt_t* crypt_data = fil_space_get_crypt_data(id);
	memset(status, 0, sizeof(*status));
	if (crypt_data != NULL) {
		status->space = id;
		status->compressed = fil_space_get_zip_size(id) > 0;
		mutex_enter(&crypt_data->mutex);
		status->last_scrub_completed =
			crypt_data->rotate_state.scrubbing.last_scrub_completed;
		if (crypt_data->rotate_state.active_threads > 0 &&
		    crypt_data->rotate_state.scrubbing.is_active) {
			status->scrubbing = true;
			status->current_scrub_started =
				crypt_data->rotate_state.start_time;
			status->current_scrub_active_threads =
				crypt_data->rotate_state.active_threads;
			status->current_scrub_page_number =
				crypt_data->rotate_state.next_offset;
			status->current_scrub_max_page_number =
				crypt_data->rotate_state.max_offset;
		} else {
			status->scrubbing = false;
		}
		mutex_exit(&crypt_data->mutex);
	} else {
		memset(status, 0, sizeof(*status));
	}

	return crypt_data == NULL ? 1 : 0;
}
