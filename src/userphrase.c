/**
 * userphrase.c
 *
 * Copyright (c) 1999, 2000, 2001
 *	Lu-chuan Kung and Kang-pen Chen.
 *	All rights reserved.
 *
 * Copyright (c) 2004, 2006
 *	libchewing Core Team. See ChangeLog for details.
 *
 * See the file "COPYING" for information on usage and redistribution
 * of this file.
 */

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "chewing-utf8-util.h"
#include "hash-private.h"
#include "dict-private.h"
#include "tree-private.h"
#include "userphrase-private.h"
#include "private.h"
#include "key2pho-private.h"

/* load the orginal frequency from the static dict */
static int LoadOriginalFreq( ChewingData *pgdata, const uint16_t phoneSeq[], const char wordSeq[], int len )
{
	const TreeType *tree_pos;
	int retval;
	Phrase *phrase = ALC( Phrase, 1 );

	tree_pos = TreeFindPhrase( pgdata, 0, len - 1, phoneSeq );
	if ( tree_pos ) {
		GetPhraseFirst( pgdata, phrase, tree_pos );
		do {
			/* find the same phrase */
			if ( ! strcmp(
				phrase->phrase,
				wordSeq ) ) {
				retval = phrase->freq;
				free( phrase );
				return retval;
			}
		} while ( GetVocabNext( pgdata, phrase ) );
	}

	free( phrase );
	return FREQ_INIT_VALUE;
}

/* find the maximum frequency of the same phrase */
static int LoadMaxFreq( ChewingData *pgdata, const uint16_t phoneSeq[], int len )
{
	const TreeType *tree_pos;
	Phrase *phrase = ALC( Phrase, 1 );
	int maxFreq = FREQ_INIT_VALUE;
	int ret;
	sqlite3_stmt *stmt = NULL;

	tree_pos = TreeFindPhrase( pgdata, 0, len - 1, phoneSeq );
	if ( tree_pos ) {
		GetPhraseFirst( pgdata, phrase, tree_pos );
		do {
			if ( phrase->freq > maxFreq )
				maxFreq = phrase->freq;
		} while( GetVocabNext( pgdata, phrase ) );
	}
	free( phrase );

	ret = sqlite3_prepare_v2( pgdata->static_data.db,
		"SELECT MAX(user_freq) FROM userphrase_v1 WHERE phone = ?1", -1,
		&stmt, NULL );
	if ( ret != SQLITE_OK ) goto end;

	ret = sqlite3_bind_blob( stmt, 1,
		phoneSeq, (len + 1) * sizeof( phoneSeq[0]), SQLITE_STATIC );
	if ( ret != SQLITE_OK ) goto end;

	ret = sqlite3_step( stmt );
	if ( ret !=  SQLITE_ROW ) goto end;

	maxFreq = sqlite3_column_int( stmt, 0 );

end:
	sqlite3_finalize( stmt );
	return maxFreq;
}

/* compute the new updated freqency */
static int UpdateFreq( int freq, int maxfreq, int origfreq, int deltatime )
{
	int delta;

	/* Short interval */
	if ( deltatime < 4000 ) {
		delta = ( freq >= maxfreq ) ?
			min(
				( maxfreq - origfreq ) / 5 + 1,
				SHORT_INCREASE_FREQ ) :
			max(
				( maxfreq - origfreq ) / 5 + 1,
				SHORT_INCREASE_FREQ );
		return min( freq + delta, MAX_ALLOW_FREQ );
	}
	/* Medium interval */
	else if ( deltatime < 50000 ) {
		delta = ( freq >= maxfreq ) ?
			min(
				( maxfreq - origfreq ) / 10 + 1,
				MEDIUM_INCREASE_FREQ ) :
			max(
				( maxfreq - origfreq ) / 10 + 1,
				MEDIUM_INCREASE_FREQ );
		return min( freq + delta, MAX_ALLOW_FREQ );
	}
	/* long interval */
	else {
		delta = max( ( freq - origfreq ) / 5, LONG_DECREASE_FREQ );
		return max( freq - delta, origfreq );
	}
}

static int GetCurrentLifeTime( ChewingData *pgdata )
{
	return pgdata->static_data.new_lifetime;
}

static void LogUserPhrase(
	ChewingData *pgdata,
	const uint16_t phoneSeq[],
	const char wordSeq[],
	int orig_freq,
	int max_freq,
	int user_freq,
	int recent_time)
{
	/* Size of each phone is len("0x1234 ") = 7 */
	char buf[7 * MAX_PHRASE_LEN + 1] = { 0 };
	int i;

	for ( i = 0; i < MAX_PHRASE_LEN; ++i ) {
		if ( phoneSeq[i] == 0 )
			break;
		snprintf( buf + 7 * i, 7 + 1, "%#06x ", phoneSeq[i] );
	}

	LOG_INFO( "userphrase %s, phone = %s, orig_freq = %d, max_freq = %d, user_freq = %d, recent_time = %d\n",
		wordSeq, buf, orig_freq, max_freq, user_freq, recent_time );
}

static int UserBindPhone( sqlite3_stmt *stmt, const uint16_t phoneSeq[] )
{
	int i;
	int len;
	int ret;

	assert( stmt );
	assert( phoneSeq );

	len = GetPhoneLen( phoneSeq );

	ret = sqlite3_bind_int( stmt, DB_INDEX_LENGTH, len );
	if ( ret != SQLITE_OK ) return ret;

	for ( i = 0; i < len; ++i ) {
		ret = sqlite3_bind_int( stmt, i + DB_INDEX_PHONE_0, phoneSeq[i] );
		if ( ret != SQLITE_OK ) return ret;
	}

	for ( i = len; i < MAX_PHRASE_LEN; ++i ) {
		ret = sqlite3_bind_int( stmt, i + DB_INDEX_PHONE_0, 0 );
		if ( ret != SQLITE_OK ) return ret;
	}

	return SQLITE_OK;
}

void UserUpdatePhraseBegin( ChewingData *pgdata )
{
	sqlite3_exec( pgdata->static_data.db, "BEGIN", 0, 0, 0 );
}

int UserUpdatePhrase( ChewingData *pgdata, const uint16_t phoneSeq[], const char wordSeq[] )
{
	int ret;
	int action;
	sqlite3_stmt *stmt = NULL;
	int len;

	int orig_freq;
	int max_freq;
	int user_freq;
	int recent_time;

	len = GetPhoneLen( phoneSeq );

	ret = sqlite3_prepare_v2( pgdata->static_data.db,
		DB_SELECT_USERPHRASE_BY_PHONE_PHRASE, -1, &stmt, NULL );
	if ( ret != SQLITE_OK ) goto error;

	ret = UserBindPhone( stmt, phoneSeq );
	if ( ret != SQLITE_OK ) goto error;

	ret = sqlite3_bind_text( stmt, DB_INDEX_PHRASE,
		wordSeq, -1, SQLITE_STATIC );
	if ( ret != SQLITE_OK ) goto error;

	recent_time = GetCurrentLifeTime( pgdata );
	ret = sqlite3_step( stmt );
	if ( ret == SQLITE_ROW ) {
		action = USER_UPDATE_MODIFY;

		orig_freq = sqlite3_column_int( pgdata->static_data.userphrase_stmt, DB_SELECT_INDEX_ORIG_FREQ );
		max_freq = LoadMaxFreq( pgdata, phoneSeq, len );
		user_freq = UpdateFreq(
			sqlite3_column_int( pgdata->static_data.userphrase_stmt, DB_SELECT_INDEX_USER_FREQ ),
			sqlite3_column_int( pgdata->static_data.userphrase_stmt, DB_SELECT_INDEX_MAX_FREQ ),
			orig_freq,
			recent_time - sqlite3_column_int( pgdata->static_data.userphrase_stmt, DB_SELECT_INDEX_TIME ) );
	} else {
		action = USER_UPDATE_INSERT;

		orig_freq = LoadOriginalFreq( pgdata, phoneSeq, wordSeq, len );
		max_freq = LoadMaxFreq( pgdata, phoneSeq, len );
		user_freq = orig_freq;
	}
	sqlite3_finalize( stmt );
	stmt = NULL;

	ret = sqlite3_prepare_v2( pgdata->static_data.db,
		DB_UPSERT_USERPHRASE, -1, &stmt, NULL );
	if ( ret != SQLITE_OK ) goto error;

	ret = sqlite3_bind_int( stmt, DB_INDEX_ORIG_FREQ, orig_freq );
	if ( ret != SQLITE_OK ) goto error;

	ret = sqlite3_bind_int( stmt, DB_INDEX_MAX_FREQ, max_freq );
	if ( ret != SQLITE_OK ) goto error;

	ret = sqlite3_bind_int( stmt, DB_INDEX_USER_FREQ, user_freq );
	if ( ret != SQLITE_OK ) goto error;

	ret = sqlite3_bind_int( stmt, DB_INDEX_TIME, recent_time );
	if ( ret != SQLITE_OK ) goto error;

	ret = UserBindPhone( stmt, phoneSeq );
	if ( ret != SQLITE_OK ) goto error;

	ret = sqlite3_bind_text( stmt, DB_INDEX_PHRASE,
		wordSeq, -1, SQLITE_STATIC );
	if ( ret != SQLITE_OK ) goto error;

	ret = sqlite3_step( stmt );
	if ( ret != SQLITE_DONE ) goto error;

	LogUserPhrase( pgdata, phoneSeq, wordSeq, orig_freq, max_freq, user_freq, recent_time);

	sqlite3_finalize( stmt );

	return action;

error:
	sqlite3_finalize( stmt );
	return USER_UPDATE_FAIL;
}

void UserUpdatePhraseEnd( ChewingData *pgdata )
{
	sqlite3_exec( pgdata->static_data.db, "END", 0, 0, 0 );
}

void UserRemovePhrase( ChewingData *pgdata, const uint16_t phoneSeq[], const char wordSeq[] )
{
	int ret;
	sqlite3_stmt *stmt = NULL;

	assert( pgdata );
	assert( phoneSeq );
	assert( wordSeq );

	ret = sqlite3_prepare_v2(
		pgdata->static_data.db,
		DB_DELETE_USERPHRASE, -1,
		&stmt, NULL );
	if ( ret != SQLITE_OK ) goto end;

	ret = UserBindPhone( stmt, phoneSeq );
	if ( ret != SQLITE_OK ) goto end;

	ret = sqlite3_bind_text( stmt, DB_INDEX_PHRASE, wordSeq, -1 , SQLITE_STATIC );
	if ( ret != SQLITE_OK ) goto end;

	ret = sqlite3_step( stmt );
	if ( ret != SQLITE_DONE ) goto end;

end:
	sqlite3_finalize( stmt );
}


UserPhraseData *UserGetPhraseFirst( ChewingData *pgdata, const uint16_t phoneSeq[] )
{
	int ret;

	assert( pgdata->static_data.userphrase_stmt == NULL );

	ret = sqlite3_prepare_v2(
		pgdata->static_data.db,
		DB_SELECT_USERPHRASE_BY_PHONE, -1,
		&pgdata->static_data.userphrase_stmt, NULL );
	if ( ret != SQLITE_OK ) goto error;

	ret = UserBindPhone( pgdata->static_data.userphrase_stmt, phoneSeq );
	if ( ret != SQLITE_OK ) goto error;

	return UserGetPhraseNext( pgdata, phoneSeq );

error:
	sqlite3_finalize( pgdata->static_data.userphrase_stmt );
	return NULL;
}

UserPhraseData *UserGetPhraseNext( ChewingData *pgdata, const uint16_t phoneSeq[] )
{
	int ret;

	assert( pgdata->static_data.userphrase_stmt );

	ret = sqlite3_step( pgdata->static_data.userphrase_stmt );
	if ( ret !=  SQLITE_ROW ) return NULL;

	/* FIXME: shall not remove const here. */
	pgdata->userphrase_data.wordSeq =
		(char *) sqlite3_column_text( pgdata->static_data.userphrase_stmt, DB_SELECT_INDEX_PHRASE );
	pgdata->userphrase_data.phoneSeq = (uint16_t *) phoneSeq;

	pgdata->userphrase_data.recentTime =
		sqlite3_column_int( pgdata->static_data.userphrase_stmt, DB_SELECT_INDEX_TIME );
	pgdata->userphrase_data.userfreq =
		sqlite3_column_int( pgdata->static_data.userphrase_stmt, DB_SELECT_INDEX_USER_FREQ );
	pgdata->userphrase_data.maxfreq =
		sqlite3_column_int( pgdata->static_data.userphrase_stmt, DB_SELECT_INDEX_MAX_FREQ );
	pgdata->userphrase_data.origfreq =
		sqlite3_column_int( pgdata->static_data.userphrase_stmt, DB_SELECT_INDEX_ORIG_FREQ );


	return &pgdata->userphrase_data;
}

void UserGetPhraseEnd( ChewingData *pgdata, const uint16_t phoneSeq[] )
{
	sqlite3_finalize( pgdata->static_data.userphrase_stmt );
	pgdata->static_data.userphrase_stmt = NULL;
}

void IncreaseLifeTime( ChewingData *pgdata )
{
	++pgdata->static_data.new_lifetime;
}
