/*
 * Copyright(c) 2012 Tim Ruehsen
 * Copyright(c) 2015-2016 Free Software Foundation, Inc.
 *
 * This file is part of Wget.
 *
 * Wget is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Wget is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Wget.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * IRI blacklist routines
 *
 * Changelog
 * 08.11.2012  Tim Ruehsen  created
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <libwget.h>

#include "log.h"
#include "blacklist.h"

static wget_hashmap_t
	*blacklist;

static wget_thread_mutex_t
	mutex = WGET_THREAD_MUTEX_INITIALIZER;

// Paul Larson's hash function from Microsoft Research
// ~ O(1) insertion, search and removal
static unsigned int G_GNUC_WGET_NONNULL_ALL hash_iri(const wget_iri_t *iri)
{
	unsigned int h = 0; // use 0 as SALT if hash table attacks doesn't matter
	const unsigned char *p;

	for (p = (unsigned char *)iri->scheme; p && *p; p++)
		h = h * 101 + *p;

	for (p = (unsigned char *)iri->port; p && *p; p++)
		h = h * 101 + *p;

	for (p = (unsigned char *)iri->host; p && *p; p++)
		h = h * 101 + *p;

	for (p = (unsigned char *)iri->path; p && *p; p++)
		h = h * 101 + *p;

	for (p = (unsigned char *)iri->query; p && *p; p++)
		h = h * 101 + *p;

	return h;
}

static int G_GNUC_WGET_NONNULL_ALL _blacklist_print(G_GNUC_WGET_UNUSED void *ctx, const wget_iri_t *iri)
{
	info_printf("blacklist %s\n", iri->uri);
	return 0;
}

void blacklist_print(void)
{
	wget_thread_mutex_lock(&mutex);
	wget_hashmap_browse(blacklist, (int(*)(void *, const void *, void *))_blacklist_print, NULL);
	wget_thread_mutex_unlock(&mutex);
}

int blacklist_size(void)
{
	return wget_hashmap_size(blacklist);
}

static void _free_entry(wget_iri_t *iri)
{
	wget_iri_free(&iri);
}

wget_iri_t *blacklist_add(wget_iri_t *iri)
{
	if (!iri)
		return NULL;

	if (wget_iri_supported(iri)) {
		wget_thread_mutex_lock(&mutex);

		if (!blacklist) {
			blacklist = wget_hashmap_create(128, -2, (unsigned int(*)(const void *))hash_iri, (int(*)(const void *, const void *))wget_iri_compare);
			wget_hashmap_set_key_destructor(blacklist, (void(*)(void *))_free_entry);
		}

		if (!wget_hashmap_contains(blacklist, iri)) {
			// info_printf("Add to blacklist: %s\n",iri->uri);
			wget_hashmap_put_noalloc(blacklist, iri, NULL); // use hashmap as a hashset (without value)
			wget_thread_mutex_unlock(&mutex);
			return iri;
		}

		wget_thread_mutex_unlock(&mutex);
	}

	wget_iri_free(&iri);

	return NULL;
}

void blacklist_free(void)
{
	wget_thread_mutex_lock(&mutex);
	wget_hashmap_free(&blacklist);
	wget_thread_mutex_unlock(&mutex);
}
