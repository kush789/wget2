/*
 * Copyright(c) 2012-2014 Tim Ruehsen
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
 * Main file
 *
 * Changelog
 * 07.04.2012  Tim Ruehsen  created
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <unistd.h>
#include <stddef.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <c-ctype.h>
#include <ctype.h>
#include <time.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <locale.h>
#include "timespec.h" // gnulib gettime()

#include <libwget.h>

#include "wget.h"
#include "log.h"
#include "job.h"
#include "options.h"
#include "blacklist.h"
#include "host.h"
#include "bar.h"

#define URL_FLG_REDIRECTION  (1<<0)
#define URL_FLG_SITEMAP      (1<<1)

typedef struct {
	wget_thread_t
		tid;
	JOB
		*job;
	PART
		*part;
	wget_http_connection_t
		*conn;
	char
		*buf;
	size_t
		bufsize;
	int
		id;
	wget_thread_cond_t
		cond;
	char
		final_error;
} DOWNLOADER;

#define _CONTENT_TYPE_HTML 1
typedef struct {
	const char *
		filename;
	const char *
		encoding;
	wget_iri_t *
		base_url;
	WGET_HTML_PARSED_RESULT *
		parsed;
	int
		content_type;
} _conversion_t;
static wget_vector_t *conversions;

typedef struct {
	int
		ndownloads; // file downloads with 200 response
	int
		nredirects; // 301, 302
	int
		nnotmodified; // 304
	int
		nerrors;
	int
		nchunks; // chunk downloads with 200 response
	long long
		bytes_body_uncompressed; // uncompressed bytes in body
} _statistics_t;
static _statistics_t stats;

static void
	save_file(wget_http_response_t *resp, const char *fname),
	append_file(wget_http_response_t *resp, const char *fname),
	sitemap_parse_xml(JOB *job, const char *data, const char *encoding, wget_iri_t *base),
	sitemap_parse_xml_gz(JOB *job, wget_buffer_t *data, const char *encoding, wget_iri_t *base),
	sitemap_parse_xml_localfile(JOB *job, const char *fname, const char *encoding, wget_iri_t *base),
	sitemap_parse_text(JOB *job, const char *data, const char *encoding, wget_iri_t *base),
	atom_parse(JOB *job, const char *data, const char *encoding, wget_iri_t *base),
	atom_parse_localfile(JOB *job, const char *fname, const char *encoding, wget_iri_t *base),
	rss_parse(JOB *job, const char *data, const char *encoding, wget_iri_t *base),
	rss_parse_localfile(JOB *job, const char *fname, const char *encoding, wget_iri_t *base),
	html_parse(JOB *job, int level, const char *data, const char *encoding, wget_iri_t *base),
	html_parse_localfile(JOB *job, int level, const char *fname, const char *encoding, wget_iri_t *base),
	css_parse(JOB *job, const char *data, const char *encoding, wget_iri_t *base),
	css_parse_localfile(JOB *job, const char *fname, const char *encoding, wget_iri_t *base);
static int
	download_part(DOWNLOADER *downloader);
static unsigned int G_GNUC_WGET_PURE
	hash_url(const char *url);
wget_http_response_t
	*http_get(wget_iri_t *iri, PART *part, DOWNLOADER *downloader, const char *method);

static wget_stringmap_t
	*etags;
static wget_hashmap_t
	*known_urls;
static DOWNLOADER
	*downloaders;
static void
	*downloader_thread(void *p);
static long long
	quota;
static int
	exit_status,
	hsts_changed;
static volatile int
	terminate;

void set_exit_status(int status)
{
	// use Wget exit status scheme:
	// - error code 0 is default
	// - error code 1 is used directly by exit() (fatal errors)
	// - error codes 2... : lower numbers preceed higher numbers
	if (exit_status) {
		if (status < exit_status)
			exit_status = status;
	} else
		exit_status = status;
}

/*
 * This functions exists to pass the Wget test suite.
 * All we really need (Wget is targeted for Unix/Linux), is UNIX restriction (\NUL and /)
 *  with escaping of control characters.
 * See http://en.wikipedia.org/wiki/Comparison_of_file_systems
 */
static char *restrict_file_name(char *fname, char *esc)
{
	char *s, *dst;
	int escaped, c;

	switch (config.restrict_file_names) {
	case RESTRICT_NAMES_WINDOWS:
		break;
	case RESTRICT_NAMES_NOCONTROL:
		break;
	case RESTRICT_NAMES_ASCII:
		for (escaped = 0, dst = esc, s = fname; *s; s++) {
			if (*s < 32) {
				*dst++ = '%';
				*dst++ = (c = ((unsigned char)*s >> 4)) >= 10 ? c + 'A' - 10 : c + '0';
				*dst++ = (c = (*s & 0xf)) >= 10 ? c + 'A' - 10 : c + '0';
				escaped = 1;
			} else
				*dst++ = *s;
		}
		*dst = 0;

		if (escaped)
			return esc;
		break;
	case RESTRICT_NAMES_UPPERCASE:
		for (s = fname; *s; s++)
			if (*s >= 'a' && *s <= 'z') // islower() also returns true for chars > 0x7f, the test is not EBCDIC compatible ;-)
				*s &= ~0x20;
		break;
	case RESTRICT_NAMES_LOWERCASE:
		for (s = fname; *s; s++)
			if (*s >= 'A' && *s <= 'Z') // isupper() also returns true for chars > 0x7f, the test is not EBCDIC compatible ;-)
				*s |= 0x20;
		break;
	case RESTRICT_NAMES_UNIX:
	default:
		for (escaped = 0, dst = esc, s = fname; *s; s++) {
			if (*s >= 1 && *s <= 31) {
				*dst++ = '%';
				*dst++ = (c = ((unsigned char)*s >> 4)) >= 10 ? c + 'A' - 10 : c + '0';
				*dst++ = (c = (*s & 0xf)) >= 10 ? c + 'A' - 10 : c + '0';
				escaped = 1;
			} else
				*dst++ = *s;
		}
		*dst = 0;

		if (escaped)
			return esc;
		break;
	}

	return fname;
}

// this function should be called protected by a mutex - else race conditions will happen
static void mkdir_path(char *fname)
{
	char *p1, *p2;
	int rc;

	for (p1 = fname + 1; *p1 && (p2 = strchr(p1, '/')); p1 = p2 + 1) {
		*p2 = 0; // replace path separator

		// relative paths should have been normalized earlier,
		// but for security reasons, don't trust myself...
		if (*p1 == '.' && p1[1] == '.')
			error_printf_exit(_("Internal error: Unexpected relative path: '%s'\n"), fname);

		rc = mkdir(fname, 0755);

		debug_printf("mkdir(%s)=%d errno=%d\n",fname,rc,errno);
		if (rc) {
			struct stat st;

			if (errno == EEXIST && stat(fname, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG) {
				// we have a file in the way... move it away and retry
				int renamed = 0;

				for (int fnum = 1; fnum <= 999 && !renamed; fnum++) {
					char dst[strlen(fname) + 1 + 32];

					snprintf(dst, sizeof(dst), "%s.%d", fname, fnum);
					if (access(dst, F_OK) != 0 && rename(fname, dst) == 0)
						renamed = 1;
				}

				if (renamed) {
					rc = mkdir(fname, 0755);

					if (rc) {
						error_printf(_("Failed to make directory '%s' (errno=%d)\n"), fname, errno);
						*p2 = '/'; // restore path separator
						break;
					}
				} else
					error_printf(_("Failed to rename '%s' (errno=%d)\n"), fname, errno);
			} else if (errno != EEXIST) {
				error_printf(_("Failed to make directory '%s' (errno=%d)\n"), fname, errno);
				*p2 = '/'; // restore path separator
				break;
			}
		} else debug_printf("created dir %s\n", fname);

		*p2 = '/'; // restore path separator
	}
}

// generate the local filename corresponding to an URI
// respect the following options:
// --restrict-file-names (unix,windows,nocontrol,ascii,lowercase,uppercase)
// -nd / --no-directories
// -x / --force-directories
// -nH / --no-host-directories
// --protocol-directories
// --cut-dirs=number
// -P / --directory-prefix=prefix

const char * G_GNUC_WGET_NONNULL_ALL get_local_filename(wget_iri_t *iri)
{
	wget_buffer_t buf;
	char *fname;
	int directories;

	if ((config.spider || config.output_document) && !config.continue_download)
		return NULL;

	directories = !!config.recursive;

	if (config.directories == 0)
		directories = 0;

	if (config.force_directories == 1)
		directories = 1;

	wget_buffer_init(&buf, NULL, 256);

	if (config.directory_prefix && *config.directory_prefix) {
		wget_buffer_strcat(&buf, config.directory_prefix);
		wget_buffer_memcat(&buf, "/", 1);
	}

	if (directories) {
		if (config.protocol_directories && iri->scheme && *iri->scheme) {
			wget_buffer_strcat(&buf, iri->scheme);
			wget_buffer_memcat(&buf, "/", 1);
		}
		if (config.host_directories && iri->host && *iri->host) {
			// wget_iri_get_host(iri, &buf);
			wget_buffer_strcat(&buf, iri->host);
			// buffer_memcat(&buf, "/", 1);
		}

		if (config.cut_directories) {
			// cut directories
			wget_buffer_t path_buf;
			const char *p;
			int n;
			char sbuf[256];

			wget_buffer_init(&path_buf, sbuf, sizeof(sbuf));
			wget_iri_get_path(iri, &path_buf, config.local_encoding);

			for (n = 0, p = path_buf.data; n < config.cut_directories && p; n++) {
				p = strchr(*p == '/' ? p + 1 : p, '/');
			}
			if (!p && path_buf.data) {
				// we can't strip this many path elements, just use the filename
				p = strrchr(path_buf.data, '/');
				if (!p) {
					p = path_buf.data;
					if (*p != '/')
						wget_buffer_memcat(&buf, "/", 1);
					wget_buffer_strcat(&buf, p);
				}
			}

			wget_buffer_deinit(&path_buf);
		} else {
			wget_iri_get_path(iri, &buf, config.local_encoding);
		}

		fname = wget_iri_get_query_as_filename(iri, &buf, config.local_encoding);
	} else {
		fname = wget_iri_get_filename(iri, &buf, config.local_encoding);
	}

	// do the filename escaping here
	if (config.restrict_file_names) {
		char fname_esc[buf.length * 3 + 1];

		if (restrict_file_name(fname, fname_esc) != fname) {
			// escaping was really done, replace fname
			wget_buffer_strcpy(&buf, fname_esc);
			fname = buf.data;
		}
	}

	// create the complete directory path
//	mkdir_path(fname);

	if (config.delete_after) {
		wget_buffer_deinit(&buf);
		fname = NULL;
	} else
		debug_printf("local filename = '%s'\n", fname);

	return fname;
}

static long long _fetch_and_add_longlong(long long *p, long long n)
{
#ifdef WITH_SYNC_FETCH_AND_ADD_LONGLONG
	return __sync_fetch_and_add(p, n);
#else
	static wget_thread_mutex_t
		mutex = WGET_THREAD_MUTEX_INITIALIZER;

	wget_thread_mutex_lock(&mutex);
	long long old_value = *p;
	*p += n;
	wget_thread_mutex_unlock(&mutex);

	return old_value;
#endif
}

static void _atomic_increment_int(int *p)
{
#ifdef WITH_SYNC_FETCH_AND_ADD
	__sync_fetch_and_add(p, 1);
#else
	static wget_thread_mutex_t
		mutex = WGET_THREAD_MUTEX_INITIALIZER;

	wget_thread_mutex_lock(&mutex);
	*p += 1;
	wget_thread_mutex_unlock(&mutex);
#endif
}

// Since quota may change at any time in a threaded environment,
// we have to modify and check the quota in one (protected) step.
static long long quota_modify_read(size_t nbytes)
{
	return _fetch_and_add_longlong(&quota, (long long )nbytes);
}

static wget_vector_t
	*parents;
static wget_thread_mutex_t
	downloader_mutex = WGET_THREAD_MUTEX_INITIALIZER;

static int in_pattern_list(const wget_vector_t *v, const char *url)
{
	for (int it = 0; it < wget_vector_size(v); it++) {
		const char *pattern = wget_vector_get(v, it);

		debug_printf("pattern[%d] '%s' - %s\n", it, pattern, url);

		if (strpbrk(pattern, "*?[]")) {
			if (!fnmatch(pattern, url, config.ignore_case ? FNM_CASEFOLD : 0))
				return 1;
		} else if (config.ignore_case) {
			if (wget_match_tail_nocase(url, pattern))
				return 1;
		} else if (wget_match_tail(url, pattern)) {
			return 1;
		}
	}

	return 0;
}

static int in_host_pattern_list(const wget_vector_t *v, const char *hostname)
{
	for (int it = 0; it < wget_vector_size(v); it++) {
		const char *pattern = wget_vector_get(v, it);

		debug_printf("host_pattern[%d] '%s' - %s\n", it, pattern, hostname);

		if (strpbrk(pattern, "*?[]")) {
			if (!fnmatch(pattern, hostname, 0))
				return 1;
		} else if (wget_match_tail(pattern, hostname)) {
			return 1;
		}
	}

	return 0;
}

// Add URLs given by user (command line or -i option).
// Needs to be thread-save.
static void add_url_to_queue(const char *url, wget_iri_t *base, const char *encoding)
{
	wget_iri_t *iri;
	JOB *new_job = NULL, job_buf;

	iri = wget_iri_parse_base(base, url, encoding);

	if (!iri) {
		error_printf(_("Failed to parse URI '%s'\n"), url);
		return;
	}

	if (iri->scheme != WGET_IRI_SCHEME_HTTP && iri->scheme != WGET_IRI_SCHEME_HTTPS) {
		error_printf(_("URI scheme not supported: '%s'\n"), url);
		wget_iri_free(&iri);
		return;
	}

	wget_thread_mutex_lock(&downloader_mutex);

	if (!blacklist_add(iri)) {
		wget_thread_mutex_unlock(&downloader_mutex);
		return;
	}

	if (config.recursive) {
		if (!config.span_hosts) {
			// only download content from hosts given on the command line or from input file
			if (!wget_vector_contains(config.exclude_domains, iri->host)) {
				wget_vector_add_str(config.domains, iri->host);
			}
		}

		if (config.robots) {
			HOST * host;

			if ((host = hosts_add(iri))) {
				// a new host entry has been created
				new_job = job_init(&job_buf, wget_iri_parse_base(iri, "/robots.txt", encoding));
				new_job->host = host;
				host->robot_job = new_job;
				new_job->deferred = wget_vector_create(2, -2, NULL);
				wget_vector_add_noalloc(new_job->deferred, iri);
			} else if ((host = hosts_get(iri)) && (new_job = host->robot_job)) {
				wget_vector_add_noalloc(new_job->deferred, iri);
			}
		}

		if (!config.parent) {
			char *p;

			if (!parents)
				parents = wget_vector_create(4, -2, NULL);

			// calc length of directory part in iri->path (including last /)
			if (!iri->path || !(p = strrchr(iri->path, '/')))
				iri->dirlen = 0;
			else
				iri->dirlen = p - iri->path + 1;

			wget_vector_add_noalloc(parents, iri);
		}
	}

	if (!new_job)
		new_job = job_init(&job_buf, iri);

	if (!new_job->deferred)
		new_job->local_filename = get_local_filename(iri);
	else
		new_job->local_filename = get_local_filename(new_job->iri);

	queue_add_job(new_job);

	wget_thread_mutex_unlock(&downloader_mutex);
}

static wget_thread_mutex_t
	main_mutex = WGET_THREAD_MUTEX_INITIALIZER,
	known_urls_mutex = WGET_THREAD_MUTEX_INITIALIZER;
static wget_thread_cond_t
	main_cond = WGET_THREAD_COND_INITIALIZER, // is signalled whenever a job is done
	worker_cond = WGET_THREAD_COND_INITIALIZER;  // is signalled whenever a job is added
static wget_thread_t
	input_tid;
static void
	*input_thread(void *p);

// Needs to be thread-save
static void add_url(JOB *job, const char *encoding, const char *url, int flags)
{
	JOB *new_job = NULL, job_buf;
	wget_iri_t *iri;

	if (flags & URL_FLG_REDIRECTION) { // redirect
		if (config.max_redirect && job && job->redirection_level >= config.max_redirect) {
			return;
		}
	} else {
//		if (config.recursive) {
//			if (config.level && job->level >= config.level + config.page_requisites) {
//				continue;
//			}
//		}
	}

	iri = wget_iri_parse(url, encoding);

	if (!iri) {
		error_printf(_("Cannot resolve URI '%s'\n"), url);
		return;
	}

	if (iri->scheme != WGET_IRI_SCHEME_HTTP && iri->scheme != WGET_IRI_SCHEME_HTTPS) {
		info_printf(_("URL '%s' not followed (unsupported scheme '%s')\n"), url, iri->scheme);
		wget_iri_free(&iri);
		return;
	}

	if (config.https_only && iri->scheme != WGET_IRI_SCHEME_HTTPS) {
		info_printf(_("URL '%s' not followed (https-only requested)\n"), url);
		wget_iri_free(&iri);
		return;
	}

	wget_thread_mutex_lock(&downloader_mutex);

	if (config.recursive && !config.parent) {
		// do not ascend above the parent directory
		int ok = 0;

		// see if at least one parent matches
		for (int it = 0; it < wget_vector_size(parents); it++) {
			wget_iri_t *parent = wget_vector_get(parents, it);

			if (!strcmp(parent->host, iri->host)) {
				if (!parent->dirlen || !strncmp(parent->path, iri->path, parent->dirlen)) {
					// info_printf("found\n");
					ok = 1;
					break;
				}
			}
		}

		if (!ok) {
			wget_thread_mutex_unlock(&downloader_mutex);
			wget_iri_free(&iri);
			info_printf(_("URL '%s' not followed (parent ascending not allowed)\n"), url);
			return;
		}
	}

	if (config.recursive) {
		// only download content from given hosts
		const char *reason = NULL;

		if (!iri->host) {
			reason = _("missing ip/host/domain");
		} else if (!config.span_hosts && config.domains && !in_host_pattern_list(config.domains, iri->host)) {
			reason = _("no host-spanning requested");
		} else if (config.span_hosts && config.exclude_domains && in_host_pattern_list(config.exclude_domains, iri->host)) {
			reason = _("domain explicitely excluded");
		}

		if (reason) {
			wget_thread_mutex_unlock(&downloader_mutex);
			info_printf(_("URL '%s' not followed (%s)\n"), iri->uri, reason);
			wget_iri_free(&iri);
			return;
		}
	}

	if (config.recursive && config.robots) {
		HOST * host;

		if ((host = hosts_add(iri))) {
			// a new host entry has been created
			new_job = job_init(&job_buf, wget_iri_parse_base(iri, "/robots.txt", encoding));
			new_job->host = host;
			host->robot_job = new_job;
			new_job->deferred = wget_vector_create(2, -2, NULL);
			wget_vector_add_noalloc(new_job->deferred, iri);
			blacklist_add(iri);
		} else if ((host = hosts_get(iri))) {
			if (host->robot_job) {
				wget_vector_add_noalloc(host->robot_job->deferred, iri);
				wget_thread_mutex_unlock(&downloader_mutex);
				return;
			}

			if (host->robots && iri->path) {
				for (int it = 0; it < wget_vector_size(host->robots->paths); it++) {
					ROBOTS_PATH *path = wget_vector_get(host->robots->paths, it);
					if (!strncmp(path->path, iri->path, path->len)) {
						wget_thread_mutex_unlock(&downloader_mutex);
						info_printf(_("URL '%s' not followed (disallowed by robots.txt)\n"), iri->uri);
						wget_iri_free(&iri);
						return;
					}
//					info_printf("checked robot path '%.*s'\n", path->path, path->len);
				}
			}
		}
	}

	if (!new_job)
		new_job = job_init(&job_buf, blacklist_add(iri));

	if (new_job) {
		if (!config.output_document) {
			if (!(flags & URL_FLG_REDIRECTION) || config.trust_server_names || !job)
				new_job->local_filename = get_local_filename(new_job->iri);
			else
				new_job->local_filename = wget_strdup(job->local_filename);
		}

		if (job) {
			if (flags & URL_FLG_REDIRECTION) {
				new_job->redirection_level = job->redirection_level + 1;
				new_job->referer = job->referer;
			} else {
				new_job->level = job->level + 1;
				new_job->referer = job->iri;
				job->iri = NULL;
			}
		}

		// mark this job as a Sitemap job, but not if it is a robot.txt job
		if (flags & URL_FLG_SITEMAP && !new_job->deferred)
			new_job->sitemap = 1;

		// now add the new job to the queue (thread-safe))
		queue_add_job(new_job);

		// and wake up all waiting threads
		wget_thread_cond_signal(&worker_cond);
	}

	wget_thread_mutex_unlock(&downloader_mutex);
}

static void _convert_links(void)
{
	FILE *fpout = NULL;
	wget_buffer_t buf;
	char sbuf[1024];

	wget_buffer_init(&buf, sbuf, sizeof(sbuf));

	// cycle through all documents where links have been found
	for (int it = 0; it < wget_vector_size(conversions); it++) {
		_conversion_t *conversion = wget_vector_get(conversions, it);
		const char *data, *data_ptr;
		size_t data_length;

		wget_info_printf("convert %s %s %s\n", conversion->filename, conversion->base_url->uri, conversion->encoding);

		if (!(data = data_ptr = wget_read_file(conversion->filename, &data_length))) {
			wget_error_printf(_("%s not found (%d)\n"), conversion->filename, errno);
			continue;
		}

		// cycle through all links found in the document
		for (int it2 = 0; it2 < wget_vector_size(conversion->parsed->uris); it2++) {
			WGET_HTML_PARSED_URL *html_url = wget_vector_get(conversion->parsed->uris, it2);
			wget_string_t *url = &html_url->url;

			url->p = (size_t) url->p + data; // convert offset to pointer

			if (url->len == 1 && *url->p == '#') // ignore e.g. href='#'
				continue;

			if (wget_iri_relative_to_abs(conversion->base_url, url->p, url->len, &buf)) {
				// buf.data now holds the absolute URL as a string
				wget_iri_t *iri = wget_iri_parse(buf.data, conversion->encoding);

				if (!iri) {
					wget_error_printf(_("Cannot resolve URI '%s'\n"), buf.data);
					continue;
				}

				const char *filename = get_local_filename(iri);

				if (access(filename, W_OK) == 0) {
					const char *linkpath = filename, *dir = NULL, *p1, *p2;
					const char *docpath = conversion->filename;

					// e.g.
					// docpath  'hostname/1level/2level/3level/xyz.html'
					// linkpath 'hostname/1level/2level.bak/3level/xyz.html'
					// expected result: '../../2level.bak/3level/xyz.html'

					// find first difference in path
					for (dir = p1 = linkpath, p2 = docpath; *p1 && *p1 == *p2; p1++, p2++)
						if (*p1 == '/') dir = p1+1;

					// generate relative path
					wget_buffer_reset(&buf); // reuse buffer
					while (*p2) {
						if (*p2++ == '/')
							wget_buffer_memcat(&buf, "../", 3);
					}
					wget_buffer_strcat(&buf, dir);

					wget_info_printf("  %.*s -> %s\n", (int) url->len,  url->p, linkpath);
					wget_info_printf("       -> %s\n", buf.data);
				} else {
					// insert absolute URL
					wget_info_printf("  %.*s -> %s\n", (int) url->len,  url->p, buf.data);
				}

				if (buf.length != url->len || strncmp(buf.data, url->p, url->len)) {
					// conversion takes place, write to disk
					if (!fpout) {
						if (config.backup_converted) {
							char dstfile[strlen(conversion->filename) + 5 + 1];

							snprintf(dstfile, sizeof(dstfile), "%s.orig", conversion->filename);

							if (rename(conversion->filename, dstfile) == -1) {
								wget_error_printf(_("Failed to rename %s to %s (%d)"), conversion->filename, dstfile, errno);
							}
						}
						if (!(fpout = fopen(conversion->filename, "w")))
							wget_error_printf(_("Failed to write open %s (%d)"), conversion->filename, errno);
					}
					if (fpout) {
						fwrite(data_ptr, 1, url->p - data_ptr, fpout);
						fwrite(buf.data, 1, buf.length, fpout);
						data_ptr = url->p + url->len;
					}
				}
				xfree(filename);
				wget_iri_free(&iri);
			}
		}

		if (fpout) {
			fwrite(data_ptr, 1, (data + data_length) - data_ptr, fpout);
			fclose(fpout);
			fpout = NULL;
		}

		xfree(data);
	}

	wget_buffer_deinit(&buf);
}

static void print_status(DOWNLOADER *downloader, const char *fmt, ...) G_GNUC_WGET_NONNULL_ALL G_GNUC_WGET_PRINTF_FORMAT(2,3);
static void print_status(DOWNLOADER *downloader G_GNUC_WGET_UNUSED, const char *fmt, ...)
{
	if (config.verbose) {
		va_list args;

		va_start(args, fmt);
		wget_info_vprintf(fmt, args);
		va_end(args);
	}
}

static void nop(int sig)
{
	if (sig == SIGTERM) {
		abort(); // hard stop if got a SIGTERM
	} else if (sig == SIGINT) {
		if (terminate)
			abort(); // hard stop if pressed CTRL-C a second time

		terminate = 1; // set global termination flag
		wget_http_abort_connection(NULL); // soft-abort all connections
	}
}

int main(int argc, const char *const *argv)
{
	int n, rc;
	size_t bufsize = 0;
	char *buf = NULL;
	bool async_urls = false;

	setlocale(LC_ALL, "");

#if ENABLE_NLS != 0
	bindtextdomain("wget", LOCALEDIR);
	textdomain("wget");
#endif

#if defined(_WIN32) || defined(_WIN64)
	// not sure if this is needed for Windows
	// signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, nop);
	signal(SIGINT, nop);
#else
	// need to set some signals
	struct sigaction sig_action;
	memset(&sig_action, 0, sizeof(sig_action));

	sig_action.sa_sigaction = (void (*)(int, siginfo_t *, void *))SIG_IGN;
	sigaction(SIGPIPE, &sig_action, NULL); // this forces socket error return
	sig_action.sa_handler = nop;
	sigaction(SIGTERM, &sig_action, NULL);
	sigaction(SIGINT, &sig_action, NULL);
#endif

	known_urls = wget_hashmap_create(128, -2, (unsigned int (*)(const void *))hash_url, (int (*)(const void *, const void *))strcmp);

	n = init(argc, argv);
	if (n < 0) {
		set_exit_status(1);
		goto out;
	}

	for (; n < argc; n++) {
		add_url_to_queue(argv[n], config.base, config.local_encoding);
	}

	if (config.input_file) {
		if (config.force_html) {
			// read URLs from HTML file
			html_parse_localfile(NULL, 0, config.input_file, config.input_encoding, config.base);
		}
		else if (config.force_css) {
			// read URLs from CSS file
			css_parse_localfile(NULL, config.input_file, config.input_encoding, config.base);
		}
		else if (config.force_sitemap) {
			// read URLs from Sitemap XML file (base is normally not needed, all URLs should be absolute)
			sitemap_parse_xml_localfile(NULL, config.input_file, "utf-8", config.base);
		}
		else if (config.force_atom) {
			// read URLs from Atom Feed XML file
			atom_parse_localfile(NULL, config.input_file, "utf-8", config.base);
		}
		else if (config.force_rss) {
			// read URLs from RSS Feed XML file
			rss_parse_localfile(NULL, config.input_file, "utf-8", config.base);
		}
//		else if (!wget_strcasecmp_ascii(config.input_file, "http://", 7)) {
//		}
		else if (!strcmp(config.input_file, "-")) {
			if (isatty(STDIN_FILENO)) {
				ssize_t len;
				char *url;

				// read URLs from STDIN
				while ((len = wget_fdgetline(&buf, &bufsize, STDIN_FILENO)) >= 0) {
					for (url = buf; len && isspace(*url); url++, len--); // skip leading spaces
					if (*url == '#' || len <= 0) continue; // skip empty lines and comments
					for (;len && isspace(url[len - 1]); len--);  // skip trailing spaces
					// debug_printf("len=%zd url=%s\n", len, buf);

					url[len] = 0;
					add_url_to_queue(buf, config.base, config.input_encoding);
				}
			} else {
				// read URLs asynchronously and process each URL immediately when it arrives
				if ((rc = wget_thread_start(&input_tid, input_thread, NULL, 0)) != 0) {
					error_printf(_("Failed to start downloader, error %d\n"), rc);
				} else {
					async_urls = true;
				}
			}
		} else {
			int fd;
			ssize_t len;
			char *url;

			// read URLs from input file
			if ((fd = open(config.input_file, O_RDONLY)) >= 0) {
				while ((len = wget_fdgetline(&buf, &bufsize, fd)) >= 0) {
					for (url = buf; len && isspace(*url); url++, len--); // skip leading spaces
					if (*url == '#' || len <= 0) continue; // skip empty lines and comments
					for (;len && isspace(url[len - 1]); len--);  // skip trailing spaces
					// debug_printf("len=%zd url=%s\n", len, buf);

					url[len] = 0;
					add_url_to_queue(url, config.base, config.input_encoding);
				}
				close(fd);
			} else
				error_printf(_("Failed to open input file %s\n"), config.input_file);
		}
	}

	if (queue_size() == 0 && !input_tid) {
		error_printf(_("Nothing to do - goodbye\n"));
		goto out;
	}

	// At this point, all values have been initialized and all URLs read.
	// Perform any sanity checking or extra initialization here.

	// Decide on the number of threads to spawn. In case we're reading
	// asynchronously from STDIN or have are downloading recursively, we don't
	// know the queue_size at startup, and hence spawn config.max_threads
	// threads.
	if (!wget_thread_support()) {
		config.num_threads = 1;
	}
	if (config.recursive || async_urls || config.max_threads < queue_size()) {
		config.num_threads = config.max_threads;
	} else {
		config.num_threads = queue_size();
	}

	if (config.progress) {
		wget_logger_set_stream(wget_get_logger(WGET_LOGGER_INFO), NULL);
		bar_init();
	}

	downloaders = xcalloc(config.num_threads, sizeof(DOWNLOADER));

	while (!queue_empty() || input_tid) {
		for (n = 0; n < config.num_threads; n++) {
			downloaders[n].id = n;

			// start worker threads (I call them 'downloaders')
			if ((rc = wget_thread_start(&downloaders[n].tid, downloader_thread, &downloaders[n], 0)) != 0) {
				error_printf(_("Failed to start downloader, error %d\n"), rc);
			}
		}

		wget_thread_mutex_lock(&main_mutex);
		while (!terminate) {
			// queue_print();
			if (queue_empty() && !input_tid) {
				break;
			}

			if (config.progress)
				bar_printf(config.num_threads, "Files: %d  Bytes: %llu  Redirects: %d  Todo: %d", stats.ndownloads, quota, stats.nredirects, queue_size());

			if (config.quota && quota >= config.quota) {
				info_printf(_("Quota of %llu bytes reached - stopping.\n"), config.quota);
				break;
			}

			// here we sit and wait for an event from our worker threads
			wget_thread_cond_wait(&main_cond, &main_mutex);
		}
	}

	// stop downloaders
	terminate = 1;
	wget_thread_cond_signal(&worker_cond);
	wget_thread_mutex_unlock(&main_mutex);

	for (n = 0; n < config.num_threads; n++) {
		//		struct timespec ts;
		//		gettime(&ts);
		//		ts.tv_sec += 1;
		// if the thread is not detached, we have to call pthread_join()/pthread_timedjoin_np()
		// else we will have a huge memory leak
		//		if ((rc=pthread_timedjoin_np(downloader[n].tid, NULL, &ts))!=0)
		if ((rc = wget_thread_join(downloaders[n].tid)) != 0)
			error_printf(_("Failed to wait for downloader #%d (%d %d)\n"), n, rc, errno);
	}

	if (config.progress)
		bar_printf(config.num_threads, "Files: %d  Bytes: %llu  Redirects: %d  Todo: %d", stats.ndownloads, quota, stats.nredirects, queue_size());
	else if ((config.recursive || config.page_requisites || (config.input_file && quota != 0)) && quota) {
		info_printf(_("Downloaded: %d files, %llu bytes, %d redirects, %d errors\n"), stats.ndownloads, quota, stats.nredirects, stats.nerrors);
	}

	if (config.save_cookies)
		wget_cookie_db_save(config.cookie_db, config.save_cookies);

	if (config.hsts && config.hsts_file && hsts_changed)
		wget_hsts_db_save(config.hsts_db, config.hsts_file);

	if (config.ocsp && config.ocsp_file)
		wget_ocsp_db_save(config.ocsp_db, config.ocsp_file);

	if (config.delete_after && config.output_document)
		unlink(config.output_document);

	if (config.debug)
		blacklist_print();

	if (config.convert_links && !config.delete_after) {
		_convert_links();
		wget_vector_free(&conversions);
	}

 out:
	// freeing to avoid disguising valgrind output
	xfree(buf);
	queue_free();
	blacklist_free();
	hosts_free();
	xfree(downloaders);
	bar_deinit();
	wget_vector_clear_nofree(parents);
	wget_vector_free(&parents);
	wget_hashmap_free(&known_urls);
	wget_stringmap_free(&etags);
	deinit();

	return exit_status;
}

void *input_thread(void *p G_GNUC_WGET_UNUSED)
{
	ssize_t len;
	size_t bufsize = 0;
	char *buf = NULL;

	while ((len = wget_fdgetline(&buf, &bufsize, STDIN_FILENO)) >= 0) {
		add_url_to_queue(buf, config.base, config.local_encoding);
		wget_thread_cond_signal(&worker_cond);
	}

	// input closed, don't read from it any more
	debug_printf("input closed\n");
	input_tid = 0;
	return NULL;
}

void *downloader_thread(void *p)
{
	static wget_thread_mutex_t
		etag_mutex = WGET_THREAD_MUTEX_INITIALIZER;

	DOWNLOADER *downloader = p;
	wget_http_response_t *resp = NULL;
	JOB *job;
	PART *part;
	int do_wait = 0;

	downloader->tid = wget_thread_self(); // to avoid race condition

	wget_thread_mutex_lock(&main_mutex);

	while (!terminate) {
		if (queue_get(&downloader->job, &downloader->part) == 0) {
			if (!wget_thread_support()) {
				// not needed, but do not leave the lock not matched.
				wget_thread_mutex_unlock(&main_mutex);
				return NULL;
			}
			// here we sit and wait for a job
			wget_thread_cond_wait(&worker_cond, &main_mutex);
			continue;
		}
		wget_thread_mutex_unlock(&main_mutex);

		if (config.wait) {
			if (do_wait) {
				if (config.random_wait)
					wget_millisleep(rand() % config.wait + config.wait / 2); // (0.5 - 1.5) * config.wait
				else
					wget_millisleep(config.wait);

				if (terminate)
					break;
			} else
				do_wait = 1;
		}

//		if (config.progress)
//			bar_print(downloader->id, "Send header...");
//			bar_update(downloader->id, 0, 0); // update to empty bar

		if ((part = downloader->part)) {
			// download metalink part
			if (download_part(downloader) == 0) {
				wget_thread_mutex_lock(&main_mutex);
				queue_del(downloader->job);
				wget_thread_cond_signal(&main_cond);
			} else {
				wget_thread_mutex_lock(&main_mutex);
				if (config.progress)
					wget_thread_cond_signal(&main_cond); // needed for progress bar updates
			}

			continue;
		}

		// hey, we got a job...
		job = downloader->job;

		if (config.accept_patterns && !in_pattern_list(config.accept_patterns, job->iri->uri)) {
			if (config.recursive)
				job->head_first = 1; // enable mime-type check to assure e.g. text/html to be downloaded and parsed
		}

		if (config.reject_patterns && in_pattern_list(config.reject_patterns, job->iri->uri)) {
			if (config.recursive)
				job->head_first = 1; // enable mime-type check to assure e.g. text/html to be downloaded and parsed
		}

//		info_printf("head_first=%d deferred=%d iri=%s\n", job->head_first, !!job->deferred, job->iri->uri);
		if ((config.spider || config.chunk_size || job->head_first) && !job->deferred) {
			// In spider mode, we first make a HEAD request.
			// If the Content-Type header gives us not a parsable type, we are done.
			print_status(downloader, "[%d] Checking '%s' ...\n", downloader->id, job->iri->uri);
			for (int tries = 0; !resp && tries < config.tries && !terminate; tries++) {
				wget_millisleep(tries * 1000 > config.waitretry ? config.waitretry : tries * 1000);

				if (terminate)
					break;

				resp = http_get(job->iri, NULL, downloader, "HEAD");
				if (resp)
					print_status(downloader, "HTTP response %d %s\n", resp->code, resp->reason);
				else if (downloader->final_error)
					goto ready;
			}

			if (!resp)
				goto ready;

			// Wget compatibility
			if (resp->code/100 == 4)
				set_exit_status(8);

			if (config.spider || job->head_first) {
				job->head_first = 0;

				if (resp->code != 200 || !resp->content_type)
					goto ready;

				if (wget_strcasecmp_ascii(resp->content_type, "text/html")
					&& wget_strcasecmp_ascii(resp->content_type, "text/css")
					&& wget_strcasecmp_ascii(resp->content_type, "application/xhtml+xml")
					&& wget_strcasecmp_ascii(resp->content_type, "application/atom+xml")
					&& wget_strcasecmp_ascii(resp->content_type, "application/rss+xml")
					&& (!job->sitemap || !wget_strcasecmp_ascii(resp->content_type, "application/xml"))
					&& (!job->sitemap || !wget_strcasecmp_ascii(resp->content_type, "application/x-gzip"))
					&& (!job->sitemap || !wget_strcasecmp_ascii(resp->content_type, "text/plain")))
					goto ready;

				if (resp->etag) {
					wget_thread_mutex_lock(&etag_mutex);
					if (!etags)
						etags = wget_stringmap_create(128);
					int rc = wget_stringmap_put_noalloc(etags, resp->etag, NULL);
					resp->etag = NULL;
					wget_thread_mutex_unlock(&etag_mutex);

					if (rc) {
						info_printf("Not scanning '%s' (known ETag)\n", job->iri->uri);
						goto ready;
					}
				}
			} else if (config.chunk_size && resp->content_length > config.chunk_size) {
				// create metalink structure without hashing
				wget_metalink_piece_t piece = { .length = config.chunk_size };
				wget_metalink_mirror_t mirror = { .location = "-", .iri = job->iri };
				wget_metalink_t *metalink = xcalloc(1, sizeof(wget_metalink_t));
				metalink->size = resp->content_length; // total file size
				metalink->name = wget_strdup(job->local_filename);

				ssize_t npieces = (resp->content_length + config.chunk_size - 1) / config.chunk_size;
				metalink->pieces = wget_vector_create((int) npieces, 1, NULL);
				for (int it = 0; it < npieces; it++) {
					piece.position = it * config.chunk_size;
					wget_vector_add(metalink->pieces, &piece, sizeof(wget_metalink_piece_t));
				}

				metalink->mirrors = wget_vector_create(1, 1, NULL);

				wget_vector_add(metalink->mirrors, &mirror, sizeof(wget_metalink_mirror_t));

				job->metalink = metalink;

				// start or resume downloading
				if (!job_validate_file(job)) {
					// wake up sleeping workers
					wget_thread_cond_signal(&worker_cond);
					job = NULL; // do not remove this job from queue yet
				} // else file already downloaded and checksum ok
				goto ready;
			}

			wget_http_free_response(&resp);
		}

//		if (job->local_filename)
//			print_status(downloader, "[%d] Downloading '%s' ...\n", downloader->id, job->local_filename);
//		else
		if (config.progress)
			bar_print(downloader->id, job->iri->uri);
		else
			print_status(downloader, "[%d] Downloading '%s' ...\n", downloader->id, job->iri->uri);

		for (int tries = 0; !resp && tries < config.tries && !terminate; tries++) {
			wget_millisleep(tries * 1000 > config.waitretry ? config.waitretry : tries * 1000);

			if (terminate)
				break;

			resp = http_get(job->iri, NULL, downloader, NULL);
			if (resp)
				print_status(downloader, "HTTP response %d %s\n", resp->code, resp->reason);
			else if (downloader->final_error)
				goto ready;
		}

		if (!resp) {
			print_status(downloader, "[%d] Failed to download\n", downloader->id);
			goto ready;
		}

		wget_cookie_normalize_cookies(job->iri, resp->cookies); // sanitize cookies
		wget_cookie_store_cookies(config.cookie_db, resp->cookies); // store cookies

		// care for HSTS feature
		if (config.hsts && job->iri->scheme == WGET_IRI_SCHEME_HTTPS && resp->hsts) {
			wget_hsts_db_add(config.hsts_db, wget_hsts_new(job->iri->host, atoi(job->iri->resolv_port), resp->hsts_maxage, resp->hsts_include_subdomains));
			hsts_changed = 1;
		}

		// check if we got a RFC 6249 Metalink response
		// HTTP/1.1 302 Found
		// Date: Fri, 20 Apr 2012 15:00:40 GMT
		// Server: Apache/2.2.22 (Linux/SUSE) mod_ssl/2.2.22 OpenSSL/1.0.0e DAV/2 SVN/1.7.4 mod_wsgi/3.3 Python/2.7.2 mod_asn/1.5 mod_mirrorbrain/2.17.0 mod_fastcgi/2.4.2
		// X-Prefix: 87.128.0.0/10
		// X-AS: 3320
		// X-MirrorBrain-Mirror: ftp.suse.com
		// X-MirrorBrain-Realm: country
		// Link: <http://go-oo.mirrorbrain.org/evolution/stable/Evolution-2.24.0.exe.meta4>; rel=describedby; type="application/metalink4+xml"
		// Link: <http://go-oo.mirrorbrain.org/evolution/stable/Evolution-2.24.0.exe.torrent>; rel=describedby; type="application/x-bittorrent"
		// Link: <http://ftp.suse.com/pub/projects/go-oo/evolution/stable/Evolution-2.24.0.exe>; rel=duplicate; pri=1; geo=de
		// Link: <http://ftp.hosteurope.de/mirror/ftp.suse.com/pub/projects/go-oo/evolution/stable/Evolution-2.24.0.exe>; rel=duplicate; pri=2; geo=de
		// Link: <http://ftp.isr.ist.utl.pt/pub/MIRRORS/ftp.suse.com/projects/go-oo/evolution/stable/Evolution-2.24.0.exe>; rel=duplicate; pri=3; geo=pt
		// Link: <http://suse.mirrors.tds.net/pub/projects/go-oo/evolution/stable/Evolution-2.24.0.exe>; rel=duplicate; pri=4; geo=us
		// Link: <http://ftp.kddilabs.jp/Linux/distributions/ftp.suse.com/projects/go-oo/evolution/stable/Evolution-2.24.0.exe>; rel=duplicate; pri=5; geo=jp
		// Digest: MD5=/sr/WFcZH1MKTyt3JHL2tA==
		// Digest: SHA=pvNwuuHWoXkNJMYSZQvr3xPzLZY=
		// Digest: SHA-256=5QgXpvMLXWCi1GpNZI9mtzdhFFdtz6tuNwCKIYbbZfU=
		// Location: http://ftp.suse.com/pub/projects/go-oo/evolution/stable/Evolution-2.24.0.exe
		// Content-Type: text/html; charset=iso-8859-1

		if (resp->links) {
			// Found a Metalink answer (RFC 6249 Metalink/HTTP: Mirrors and Hashes).
			// We try to find and download the .meta4 file (RFC 5854).
			// If we can't find the .meta4, download from the link with the highest priority.

			wget_http_link_t *top_link = NULL, *metalink = NULL;
			int it;

			for (it = 0; it < wget_vector_size(resp->links); it++) {
				wget_http_link_t *link = wget_vector_get(resp->links, it);
				if (link->rel == link_rel_describedby) {
					if (link->type && (!wget_strcasecmp_ascii(link->type, "application/metalink4+xml") ||
						 !wget_strcasecmp_ascii(link->type, "application/metalink+xml")))
					{
						// found a link to a metalink4 description
						metalink = link;
						break;
					}
				} else if (link->rel == link_rel_duplicate) {
					if (!top_link || top_link->pri > link->pri)
						// just save the top priority link
						top_link = link;
				}
			}

			if (metalink) {
				// found a link to a metalink3 or metalink4 description, create a new job
				add_url(job, "utf-8", metalink->uri, 0);
				goto ready;
			} else if (top_link) {
				// no metalink4 description found, create a new job
				add_url(job, "utf-8", top_link->uri, 0);
				goto ready;
			}
		}

		if (resp->content_type) {
			if (!wget_strcasecmp_ascii(resp->content_type, "application/metalink4+xml")) {
				// print_status(downloader, "get metalink4 info\n");
				// save_file(resp, job->local_filename, O_TRUNC);
				job->metalink = metalink4_parse(resp->body->data);
			}
			else if (!wget_strcasecmp_ascii(resp->content_type, "application/metalink+xml")) {
				// print_status(downloader, "get metalink3 info\n");
				// save_file(resp, job->local_filename, O_TRUNC);
				job->metalink = metalink3_parse(resp->body->data);
			}
			if (job->metalink) {
				if (job->metalink->size <= 0) {
					error_printf("File length %llu - remove job\n", (unsigned long long)job->metalink->size);
				} else if (!job->metalink->mirrors) {
					error_printf("No download mirrors found - remove job\n");
				} else {
					// just loaded a metalink description, create parts and sort mirrors

					// start or resume downloading
					if (!job_validate_file(job)) {
						// sort mirrors by priority to download from highest priority first
						wget_metalink_sort_mirrors(job->metalink);

						// wake up sleeping workers
						wget_thread_cond_signal(&worker_cond);

						job = NULL; // do not remove this job from queue yet
					} // else file already downloaded and checksum ok
				}
				goto ready;
			}
		}

		if (resp->code == 200) {
			if (config.content_disposition && resp->content_filename)
				save_file(resp, resp->content_filename);
			else
				save_file(resp, config.output_document ? config.output_document : job->local_filename);

			if (config.recursive && (!config.level || job->level < config.level + config.page_requisites)) {
				if (resp->content_type) {
					if (!wget_strcasecmp_ascii(resp->content_type, "text/html")) {
						html_parse(job, job->level, resp->body->data, resp->content_type_encoding ? resp->content_type_encoding : config.remote_encoding, job->iri);
					} else if (!wget_strcasecmp_ascii(resp->content_type, "application/xhtml+xml")) {
						html_parse(job, job->level, resp->body->data, resp->content_type_encoding ? resp->content_type_encoding : config.remote_encoding, job->iri);
						// xml_parse(sockfd, resp, job->iri);
					} else if (!wget_strcasecmp_ascii(resp->content_type, "text/css")) {
						css_parse(job, resp->body->data, resp->content_type_encoding ? resp->content_type_encoding : config.remote_encoding, job->iri);
					} else if (!wget_strcasecmp_ascii(resp->content_type, "application/atom+xml")) { // see RFC4287, http://de.wikipedia.org/wiki/Atom_%28Format%29
						atom_parse(job, resp->body->data, "utf-8", job->iri);
					} else if (!wget_strcasecmp_ascii(resp->content_type, "application/rss+xml")) { // see http://cyber.law.harvard.edu/rss/rss.html
						rss_parse(job, resp->body->data, "utf-8", job->iri);
					} else if (job->sitemap) {
						if (!wget_strcasecmp_ascii(resp->content_type, "application/xml"))
							sitemap_parse_xml(job, resp->body->data, "utf-8", job->iri);
						else if (!wget_strcasecmp_ascii(resp->content_type, "application/x-gzip"))
							sitemap_parse_xml_gz(job, resp->body, "utf-8", job->iri);
						else if (!wget_strcasecmp_ascii(resp->content_type, "text/plain"))
							sitemap_parse_text(job, resp->body->data, "utf-8", job->iri);
					} else if (job->deferred && !wget_strcasecmp_ascii(resp->content_type, "text/plain")) {
						debug_printf("Scanning robots.txt ...\n");
						if ((job->host->robots = wget_robots_parse(resp->body->data))) {
							// add sitemaps to be downloaded (format http://www.sitemaps.org/protocol.html)
							for (int it = 0; it < wget_vector_size(job->host->robots->sitemaps); it++) {
								const char *sitemap = wget_vector_get(job->host->robots->sitemaps, it);
								info_printf("adding sitemap '%s'\n", sitemap);
//	debug_printf("XXX adding %s\n", sitemap);
								add_url(job, "utf-8", sitemap, URL_FLG_SITEMAP); // see http://www.sitemaps.org/protocol.html#escaping
							}
//	debug_printf("XXX 4\n");
//							info_printf("host->robots %p\n", job->host->robots);
						}
					}
				}
			}
		}
		else if (resp->code == 206 && config.continue_download) { // partial content
			if (config.content_disposition && resp->content_filename)
				append_file(resp, resp->content_filename);
			else
				append_file(resp, config.output_document ? config.output_document : job->local_filename);
		}
		else if (resp->code == 304 && config.timestamping) { // local document is up-to-date
			if (config.recursive && (!config.level || job->level < config.level + config.page_requisites) && job->local_filename) {
				const char *ext;

				if (config.content_disposition && resp->content_filename)
					ext = strrchr(resp->content_filename, '.');
				else
					ext = strrchr(job->local_filename, '.');

				if (ext) {
					if (!wget_strcasecmp_ascii(ext, ".html") || !wget_strcasecmp_ascii(ext, ".htm")) {
						html_parse_localfile(job, job->level, job->local_filename, resp->content_type_encoding ? resp->content_type_encoding : config.remote_encoding, job->iri);
					} else if (!wget_strcasecmp_ascii(ext, ".css")) {
						css_parse_localfile(job, job->local_filename, resp->content_type_encoding ? resp->content_type_encoding : config.remote_encoding, job->iri);
					}
				}
			}
		} else if (resp->code == 404) {
			if (!job->deferred) // ignore errors on robots.txt
				set_exit_status(8);
		}

		// regular download
ready:
		wget_http_free_response(&resp);

		// download of single-part file complete, remove from job queue
		wget_thread_mutex_lock(&main_mutex);
		queue_del(job);
		wget_thread_cond_signal(&main_cond);
	}

	wget_thread_mutex_unlock(&main_mutex);
	wget_http_close(&downloader->conn);

	// if we terminate, tell the other downloaders
	wget_thread_cond_signal(&worker_cond);

	return NULL;
}

static void _free_conversion_entry(_conversion_t *conversion)
{
	xfree(conversion->filename);
	xfree(conversion->encoding);
	wget_iri_free(&conversion->base_url);
	wget_html_free_urls_inline(&conversion->parsed);
}

static void _remember_for_conversion(const char *filename, wget_iri_t *base_url, int content_type, const char *encoding, WGET_HTML_PARSED_RESULT *parsed)
{
	static wget_thread_mutex_t
		mutex = WGET_THREAD_MUTEX_INITIALIZER;
	_conversion_t conversion;

	conversion.filename = strdup(filename);
	conversion.encoding = strdup(encoding);
	conversion.base_url = wget_iri_clone(base_url);
	conversion.content_type = content_type;
	conversion.parsed = parsed;

	wget_thread_mutex_lock(&mutex);

	if (!conversions) {
		conversions = wget_vector_create(128, -2, NULL);
		wget_vector_set_destructor(conversions, (void(*)(void *))_free_conversion_entry);
	}

	wget_vector_add(conversions, &conversion, sizeof(conversion));

	wget_thread_mutex_unlock(&mutex);
}

static unsigned int G_GNUC_WGET_PURE hash_url(const char *url)
{
	unsigned int hash = 0; // use 0 as SALT if hash table attacks doesn't matter

	while (*url)
		hash = hash * 101 + (unsigned char)*url++;

	return hash;
}

void html_parse(JOB *job, int level, const char *html, const char *encoding, wget_iri_t *base)
{
	WGET_HTML_PARSED_RESULT *parsed  = wget_html_get_urls_inline(html, config.follow_tags, config.ignore_tags);
	wget_iri_t *allocated_base = NULL;
	const char *reason;
	wget_buffer_t buf;
	char sbuf[1024];

	if (config.robots && !parsed->follow)
		goto cleanup;

	// http://www.whatwg.org/specs/web-apps/current-work/, 12.2.2.2
	if (encoding && encoding == config.remote_encoding) {
		reason = _("set by user");
	} else {
		if ((unsigned char)html[0] == 0xFE && (unsigned char)html[1] == 0xFF) {
			// Big-endian UTF-16
			encoding = "UTF-16BE";
			reason = _("set by BOM");
		} else if ((unsigned char)html[0] == 0xFF && (unsigned char)html[1] == 0xFE) {
			// Little-endian UTF-16
			encoding = "UTF-16LE";
			reason = _("set by BOM");
		} else if ((unsigned char)html[0] == 0xEF && (unsigned char)html[1] == 0xBB && (unsigned char)html[2] == 0xBF) {
			// UTF-8
			encoding = "UTF-8";
			reason = _("set by BOM");
		} else {
			reason = _("set by server response");
		}

		if (!wget_strncasecmp(parsed->encoding, "UTF-16", 6) || !wget_strncasecmp(encoding, "UTF-16", 6)) {
			// http://www.whatwg.org/specs/web-apps/current-work/, 12.2.2.2
			// we found an encoding in the HTML, so it can't be UTF-16*.
			encoding = "UTF-8";
			reason = _("wrong stated UTF-16* changed to UTF-8");
		}

		if (!encoding) {
			if (parsed->encoding) {
				encoding = parsed->encoding;
				reason = _("set by document");
			} else {
				encoding = "CP1252"; // default encoding for HTML5 (pre-HTML5 is iso-8859-1)
				reason = _("default, encoding not specified");
			}
		}
	}

	info_printf(_("URI content encoding = '%s' (%s)\n"), encoding, reason);

	wget_buffer_init(&buf, sbuf, sizeof(sbuf));

	if (parsed->base.p) {
		if (parsed->base.len > 1 || (parsed->base.len == 1 && *parsed->base.p != '#')) { // ignore e.g. href='#'
			if (wget_iri_relative_to_abs(base, parsed->base.p, parsed->base.len, &buf)) {
				// info_printf("%.*s -> %s\n", (int)parsed->base.len, parsed->base.p, buf.data);
				if (!base && !buf.length)
					info_printf(_("BASE '%.*s' not usable (missing absolute base URI)\n"), (int)parsed->base.len, parsed->base.p);
				else
					base = allocated_base = wget_iri_parse(buf.data, encoding);
			} else {
				error_printf(_("Cannot resolve BASE URI %.*s\n"), (int)parsed->base.len, parsed->base.p);
			}
		}
	}

	int page_requisites = config.recursive && config.page_requisites && config.level && level < config.level;
//	info_printf(_("page_req %d: %d %d %d %d\n"), page_requisites, config.recursive, config.page_requisites, config.level, level);

	wget_thread_mutex_lock(&known_urls_mutex);
	for (int it = 0; it < wget_vector_size(parsed->uris); it++) {
		WGET_HTML_PARSED_URL *html_url = wget_vector_get(parsed->uris, it);
		wget_string_t *url = &html_url->url;

		// Blacklist for URLs before they are processed
		if (wget_hashmap_put_noalloc(known_urls, wget_strmemdup(url->p, url->len), NULL)) {
			// error_printf(_("URL '%.*s' already known\n"), (int)url->len, url->p);
			continue;
		} else {
			// error_printf(_("URL '%.*s' added\n"), (int)url->len, url->p);
		}

		// with --page-requisites: just load inline URLs from the deepest level documents
		if (page_requisites && !wget_strcasecmp_ascii(html_url->attr, "href")) {
			// don't load from dir 'A', 'AREA' and 'EMBED'
			if (c_tolower(*html_url->dir) == 'a'
				&& (html_url->dir[1] == 0 || !wget_strcasecmp_ascii(html_url->dir,"area") || !wget_strcasecmp_ascii(html_url->dir,"embed"))) {
				info_printf(_("URL '%.*s' not followed (page requisites + level)\n"), (int)url->len, url->p);
				continue;
			}
		}

		if (url->len > 1 || (url->len == 1 && *url->p != '#')) { // ignore e.g. href='#'
			if (wget_iri_relative_to_abs(base, url->p, url->len, &buf)) {
				// info_printf("%.*s -> %s\n", (int)url->len, url->p, buf.data);
				if (!base && !buf.length)
					info_printf(_("URL '%.*s' not followed (missing base URI)\n"), (int)url->len, url->p);
				else
					add_url(job, encoding, buf.data, 0);
			} else {
				error_printf(_("Cannot resolve relative URI %.*s\n"), (int)url->len, url->p);
			}
		}
	}
	wget_thread_mutex_unlock(&known_urls_mutex);

	wget_buffer_deinit(&buf);

	if (config.convert_links && !config.delete_after) {
		for (int it = 0; it < wget_vector_size(parsed->uris); it++) {
			WGET_HTML_PARSED_URL *html_url = wget_vector_get(parsed->uris, it);
			html_url->url.p = (const char *) (html_url->url.p - html); // convert pointer to offset
		}
		_remember_for_conversion(job->local_filename, base, _CONTENT_TYPE_HTML, encoding, parsed);
		parsed = NULL; // 'parsed' has been consumed
	}

	wget_iri_free(&allocated_base);

cleanup:
	wget_html_free_urls_inline(&parsed);
}

void html_parse_localfile(JOB *job, int level, const char *fname, const char *encoding, wget_iri_t *base)
{
	char *data;

	if ((data = wget_read_file(fname, NULL)))
		html_parse(job, level, data, encoding, base);

	xfree(data);
}

void sitemap_parse_xml(JOB *job, const char *data, const char *encoding, wget_iri_t *base)
{
	wget_vector_t *urls, *sitemap_urls;
	const char *p;
	size_t baselen = 0;

	wget_sitemap_get_urls_inline(data, &urls, &sitemap_urls);

	if (base) {
		if ((p = strrchr(base->uri, '/')))
			baselen = p - base->uri + 1; // + 1 to include /
		else
			baselen = strlen(base->uri);
	}

	// process the sitemap urls here
	info_printf(_("found %d url(s) (base=%s)\n"), wget_vector_size(urls), base ? base->uri : NULL);
	wget_thread_mutex_lock(&known_urls_mutex);
	for (int it = 0; it < wget_vector_size(urls); it++) {
		wget_string_t *url = wget_vector_get(urls, it);;

		// A Sitemap file located at http://example.com/catalog/sitemap.xml can include any URLs starting with http://example.com/catalog/
		// but not any other.
		if (baselen && (url->len <= baselen || wget_strncasecmp(url->p, base->uri, baselen))) {
			info_printf(_("URL '%.*s' not followed (not matching sitemap location)\n"), (int)url->len, url->p);
			continue;
		}

		// Blacklist for URLs before they are processed
		if (wget_hashmap_put_noalloc(known_urls, (p = wget_strmemdup(url->p, url->len)), NULL)) {
			// the dup'ed url has already been freed when we come here
			info_printf(_("URL '%.*s' not followed (already known)\n"), (int)url->len, url->p);
			continue;
		}

		add_url(job, encoding, p, 0);
	}

	// process the sitemap index urls here
	info_printf(_("found %d sitemap url(s) (base=%s)\n"), wget_vector_size(sitemap_urls), base ? base->uri : NULL);
	for (int it = 0; it < wget_vector_size(sitemap_urls); it++) {
		wget_string_t *url = wget_vector_get(sitemap_urls, it);;

		// TODO: url must have same scheme, port and host as base

		// Blacklist for URLs before they are processed
		if (wget_hashmap_put_noalloc(known_urls, (p = wget_strmemdup(url->p, url->len)), NULL)) {
			// the dup'ed url has already been freed when we come here
			info_printf(_("URL '%.*s' not followed (already known)\n"), (int)url->len, url->p);
			continue;
		}

		add_url(job, encoding, p, URL_FLG_SITEMAP);
	}
	wget_thread_mutex_unlock(&known_urls_mutex);

	wget_vector_free(&urls);
	wget_vector_free(&sitemap_urls);
	// wget_sitemap_free_urls_inline(&res);
}

static int _get_unzipped(void *userdata, const char *data, size_t length)
{
	wget_buffer_memcat((wget_buffer_t *)userdata, data, length);

	return 0;
}

void sitemap_parse_xml_gz(JOB *job, wget_buffer_t *gzipped_data, const char *encoding, wget_iri_t *base)
{
	wget_buffer_t *plain = wget_buffer_alloc(gzipped_data->length * 10);
	wget_decompressor_t *dc = NULL;

	if ((dc = wget_decompress_open(wget_content_encoding_gzip, _get_unzipped, plain))) {
		wget_decompress(dc, gzipped_data->data, gzipped_data->length);
		wget_decompress_close(dc);

		sitemap_parse_xml(job, plain->data, encoding, base);
	} else
		error_printf("Can't scan '%s' because no libz support enabled at compile time\n", job->iri->uri);

	wget_buffer_free(&plain);
}

void sitemap_parse_xml_localfile(JOB *job, const char *fname, const char *encoding, wget_iri_t *base)
{
	char *data;

	if ((data = wget_read_file(fname, NULL)))
		sitemap_parse_xml(job, data, encoding, base);

	xfree(data);
}

void sitemap_parse_text(JOB *job, const char *data, const char *encoding, wget_iri_t *base)
{
	size_t baselen = 0;
	const char *end, *line, *p;
	size_t len;

	if (base) {
		if ((p = strrchr(base->uri, '/')))
			baselen = p - base->uri + 1; // + 1 to include /
		else
			baselen = strlen(base->uri);
	}

	// also catch the case where the last line isn't terminated by '\n'
	for (line = end = data; *end && (end = (p = strchr(line, '\n')) ? p : line + strlen(line)); line = end + 1) {
		// trim
		len = end - line;
		for (;len && isspace(*line); line++, len--); // skip leading spaces
		for (;len && isspace(line[len - 1]); len--);  // skip trailing spaces

		if (len) {
			// A Sitemap file located at http://example.com/catalog/sitemap.txt can include any URLs starting with http://example.com/catalog/
			// but not any other.
			if (baselen && (len <= baselen || wget_strncasecmp(line, base->uri, baselen))) {
				info_printf(_("URL '%.*s' not followed (not matching sitemap location)\n"), (int)len, line);
			} else {
				char url[len + 1];

				memcpy(url, line, len);
				url[len] = 0;

				add_url(job, encoding, url, 0);
			}
		}
	}
}

static void _add_urls(JOB *job, wget_vector_t *urls, const char *encoding, wget_iri_t *base)
{
	const char *p;
	size_t baselen = 0;

	if (base) {
		if ((p = strrchr(base->uri, '/')))
			baselen = p - base->uri + 1; // + 1 to include /
		else
			baselen = strlen(base->uri);
	}

	info_printf(_("found %d url(s) (base=%s)\n"), wget_vector_size(urls), base ? base->uri : NULL);

	wget_thread_mutex_lock(&known_urls_mutex);
	for (int it = 0; it < wget_vector_size(urls); it++) {
		wget_string_t *url = wget_vector_get(urls, it);

		if (baselen && (url->len <= baselen || wget_strncasecmp(url->p, base->uri, baselen))) {
			info_printf(_("URL '%.*s' not followed (not matching sitemap location)\n"), (int)url->len, url->p);
			continue;
		}

		// Blacklist for URLs before they are processed
		if (wget_hashmap_put_noalloc(known_urls, (p = wget_strmemdup(url->p, url->len)), NULL)) {
			// the dup'ed url has already been freed when we come here
			info_printf(_("URL '%.*s' not followed (already known)\n"), (int)url->len, url->p);
			continue;
		}

		add_url(job, encoding, p, 0);
	}
	wget_thread_mutex_unlock(&known_urls_mutex);
}

void atom_parse(JOB *job, const char *data, const char *encoding, wget_iri_t *base)
{
	wget_vector_t *urls;

	wget_atom_get_urls_inline(data, &urls);
	_add_urls(job, urls, encoding, base);
	wget_vector_free(&urls);
	// wget_atom_free_urls_inline(&res);
}

void atom_parse_localfile(JOB *job, const char *fname, const char *encoding, wget_iri_t *base)
{
	char *data;

	if ((data = wget_read_file(fname, NULL)))
		atom_parse(job, data, encoding, base);

	xfree(data);
}

void rss_parse(JOB *job, const char *data, const char *encoding, wget_iri_t *base)
{
	wget_vector_t *urls;

	wget_rss_get_urls_inline(data, &urls);
	_add_urls(job, urls, encoding, base);
	wget_vector_free(&urls);
	// wget_rss_free_urls_inline(&res);
}

void rss_parse_localfile(JOB *job, const char *fname, const char *encoding, wget_iri_t *base)
{
	char *data;

	if ((data = wget_read_file(fname, NULL)))
		rss_parse(job, data, encoding, base);

	xfree(data);
}

struct css_context {
	JOB
		*job;
	wget_iri_t
		*base;
	const char
		*encoding;
	wget_buffer_t
		uri_buf;
	char
		encoding_allocated;
};

static void _css_parse_encoding(void *context, const char *encoding, size_t len)
{
	struct css_context *ctx = context;

	// take only the first @charset rule
	if (!ctx->encoding_allocated && wget_strncasecmp_ascii(ctx->encoding, encoding, len)) {
		ctx->encoding = wget_strmemdup(encoding, len);
		ctx->encoding_allocated = 1;
		info_printf(_("URI content encoding = '%s'\n"), ctx->encoding);
	}
}

static void _css_parse_uri(void *context, const char *url, size_t len, size_t pos G_GNUC_WGET_UNUSED)
{
	struct css_context *ctx = context;

	if (len > 1 || (len == 1 && *url != '#')) {
		// ignore e.g. href='#'
		if (wget_iri_relative_to_abs(ctx->base, url, len, &ctx->uri_buf)) {
			if (!ctx->base && !ctx->uri_buf.length)
				info_printf(_("URL '%.*s' not followed (missing base URI)\n"), (int)len, url);
			else
				add_url(ctx->job, ctx->encoding, ctx->uri_buf.data, 0);
		} else {
			error_printf(_("Cannot resolve relative URI %.*s\n"), (int)len, url);
		}
	}
}

void css_parse(JOB *job, const char *data, const char *encoding, wget_iri_t *base)
{
	// create scheme://authority that will be prepended to relative paths
	struct css_context context = { .base = base, .job = job, .encoding = encoding };
	char sbuf[1024];

	wget_buffer_init(&context.uri_buf, sbuf, sizeof(sbuf));

	if (encoding)
		info_printf(_("URI content encoding = '%s'\n"), encoding);

	wget_css_parse_buffer(data, _css_parse_uri, _css_parse_encoding, &context);

	if (context.encoding_allocated)
		xfree(context.encoding);

	wget_buffer_deinit(&context.uri_buf);
}

void css_parse_localfile(JOB *job, const char *fname, const char *encoding, wget_iri_t *base)
{
	// create scheme://authority that will be prepended to relative paths
	struct css_context context = { .base = base, .job = job, .encoding = encoding };
	char sbuf[1024];

	wget_buffer_init(&context.uri_buf, sbuf, sizeof(sbuf));

	if (encoding)
		info_printf(_("URI content encoding = '%s'\n"), encoding);

	wget_css_parse_file(fname, _css_parse_uri, _css_parse_encoding, &context);

	if (context.encoding_allocated)
		xfree(context.encoding);

	wget_buffer_deinit(&context.uri_buf);
}

static long long G_GNUC_WGET_NONNULL_ALL get_file_size(const char *fname)
{
	struct stat st;

	if (stat(fname, &st)==0) {
		return st.st_size;
	}

	return 0;
}

static time_t G_GNUC_WGET_NONNULL_ALL get_file_mtime(const char *fname)
{
	struct stat st;

	if (stat(fname, &st)==0) {
		return st.st_mtime;
	}

	return 0;
}

static void set_file_mtime(int fd, time_t modified)
{
	struct timespec timespecs[2]; // [0]=last access  [1]=last modified

	gettime(&timespecs[0]);

	timespecs[1].tv_sec = modified;
	timespecs[1].tv_nsec = 0;

	if (futimens(fd, timespecs) == -1)
		error_printf (_("Failed to set file date: %s\n"), strerror (errno));
}

static void G_GNUC_WGET_NONNULL((1)) _save_file(wget_http_response_t *resp, const char *fname, int flag)
{
	static wget_thread_mutex_t
		savefile_mutex = WGET_THREAD_MUTEX_INITIALIZER;
	char *alloced_fname = NULL;
	int fd, multiple = 0, fnum, oflag = flag, maxloop;
	size_t fname_length;

	if (!fname)
		return;

	if (config.spider) {
		debug_printf("not saved '%s' (spider mode enabled)\n", fname);
		return;
	}

	// do not save into directories
	fname_length = strlen(fname);
	if (fname[fname_length - 1] == '/') {
		debug_printf("not saved '%s' (file is a directory)\n", fname);
		return;
	}

	// - optimistic approach expects data being written without error
	// - to be Wget compatible: quota_modify_read() returns old quota value
	if (config.quota) {
		if (quota_modify_read(config.save_headers ? resp->header->length + resp->body->length : resp->body->length) >= config.quota) {
			debug_printf("not saved '%s' (quota of %lld reached)\n", fname, config.quota);
			return;
		}
	} else {
		// just update number bytes read (body only) for display purposes
		quota_modify_read(config.save_headers ? resp->header->length + resp->body->length : resp->body->length);
	}

	if (fname == config.output_document) {
		// <fname> can only be NULL if config.delete_after is set
		if (!strcmp(fname, "-")) {
			size_t rc;

			if (config.save_headers) {
				if ((rc = fwrite(resp->header->data, 1, resp->header->length, stdout)) != resp->header->length) {
					error_printf(_("Failed to write to STDOUT (%zu, errno=%d)\n"), rc, errno);
					set_exit_status(3);
				}
			}

			if ((rc = fwrite(resp->body->data, 1, resp->body->length, stdout)) != resp->body->length) {
				error_printf(_("Failed to write to STDOUT (%zu, errno=%d)\n"), rc, errno);
				set_exit_status(3);
			}

			return;
		}

		if (config.delete_after) {
			debug_printf("not saved '%s' (--delete-after)\n", fname);
			return;
		}

		flag = O_APPEND;
	}

	if (config.adjust_extension && resp->content_type) {
		const char *ext;

		if (!wget_strcasecmp_ascii(resp->content_type, "text/html") || !wget_strcasecmp_ascii(resp->content_type, "application/xhtml+xml")) {
			ext = ".html";
		} else if (!wget_strcasecmp_ascii(resp->content_type, "text/css")) {
			ext = ".css";
		} else if (!wget_strcasecmp_ascii(resp->content_type, "application/atom+xml")) {
			ext = ".atom";
		} else if (!wget_strcasecmp_ascii(resp->content_type, "application/rss+xml")) {
			ext = ".rss";
		} else
			ext = NULL;

		if (ext) {
			size_t ext_length = strlen(ext);

			if (fname_length >= ext_length && wget_strcasecmp_ascii(fname + fname_length - ext_length, ext)) {
				alloced_fname = xmalloc(fname_length + ext_length + 1);
				memcpy(alloced_fname, fname, fname_length);
				memcpy(alloced_fname + fname_length, ext, ext_length + 1);
				fname = alloced_fname;
			}
		}
	}

	if (config.accept_patterns && !in_pattern_list(config.accept_patterns, fname)) {
		debug_printf("not saved '%s' (doesn't match accept pattern)\n", fname);
		xfree(alloced_fname);
		return;
	}

	if (config.reject_patterns && in_pattern_list(config.reject_patterns, fname)) {
		debug_printf("not saved '%s' (matches reject pattern)\n", fname);
		xfree(alloced_fname);
		return;
	}

	wget_thread_mutex_lock(&savefile_mutex);

	fname_length += 16;

	if (config.timestamping) {
		if (oflag == O_TRUNC)
			flag = O_TRUNC;
	} else if (!config.clobber || (config.recursive && config.directories)) {
		if (oflag == O_TRUNC && !(config.recursive && config.directories))
			flag = O_EXCL;
	} else if (flag != O_APPEND) {
		// wget compatibility: "clobber" means generating of .x files
		multiple = 1;
		flag = O_EXCL;

		if (config.backups) {
			char src[fname_length + 1], dst[fname_length + 1];

			for (int it = config.backups; it > 0; it--) {
				if (it > 1)
					snprintf(src, sizeof(src), "%s.%d", fname, it - 1);
				else
					strlcpy(src, fname, sizeof(src));
				snprintf(dst, sizeof(dst), "%s.%d", fname, it);

				if (rename(src, dst) == -1 && errno != ENOENT)
					error_printf(_("Failed to rename %s to %s (errno=%d)\n"), src, dst, errno);
			}
		}
	}

	// create the complete directory path
	mkdir_path((char *) fname);
	fd = open(fname, O_WRONLY | flag | O_CREAT, 0644);
	// debug_printf("1 fd=%d flag=%02x (%02x %02x %02x) errno=%d %s\n",fd,flag,O_EXCL,O_TRUNC,O_APPEND,errno,fname);

	// find a non-existing filename
	char unique[fname_length + 1];
	*unique = 0;
	for (fnum = 0, maxloop = 999; fd < 0 && ((multiple && errno == EEXIST) || errno == EISDIR) && fnum < maxloop; fnum++) {
		snprintf(unique, sizeof(unique), "%s.%d", fname, fnum + 1);
		fd = open(unique, O_WRONLY | flag | O_CREAT, 0644);
	}

	if (fd >= 0) {
		ssize_t rc;

		if (config.save_headers) {
			if ((rc = write(fd, resp->header->data, resp->header->length)) != (ssize_t)resp->header->length) {
				error_printf(_("Failed to write file %s (%zd, errno=%d)\n"), fnum ? unique : fname, rc, errno);
				set_exit_status(3);
			}
		}

		if ((rc = write(fd, resp->body->data, resp->body->length)) != (ssize_t)resp->body->length) {
			error_printf(_("Failed to write file %s (%zd, errno=%d)\n"), fnum ? unique : fname, rc, errno);
			set_exit_status(3);
		}

		if ((flag & (O_TRUNC | O_EXCL)) && resp->last_modified)
			set_file_mtime(fd, resp->last_modified);

		if (flag == O_APPEND)
			info_printf("appended to '%s'\n", fnum ? unique : fname);
		else
			info_printf("saved '%s'\n", fnum ? unique : fname);

		close(fd);
	} else {
		if (fd == -1) {
			if (errno == EEXIST)
				error_printf(_("File '%s' already there; not retrieving.\n"), fname);
			else if (errno == EISDIR)
				info_printf(_("Directory / file name clash - not saving '%s'\n"), fname);
			else {
				error_printf(_("Failed to open '%s' (errno=%d): %s\n"), fname, errno, strerror(errno));
				set_exit_status(3);
			}
		}
	}

	wget_thread_mutex_unlock(&savefile_mutex);

	xfree(alloced_fname);
}

static void G_GNUC_WGET_NONNULL((1)) save_file(wget_http_response_t *resp, const char *fname)
{
	_save_file(resp, fname, O_TRUNC);
}

static void G_GNUC_WGET_NONNULL((1)) append_file(wget_http_response_t *resp, const char *fname)
{
	_save_file(resp, fname, O_APPEND);
}

int download_part(DOWNLOADER *downloader)
{
	JOB *job = downloader->job;
	wget_metalink_t *metalink = job->metalink;
	PART *part = downloader->part;
	int mirror_index = downloader->id % wget_vector_size(metalink->mirrors);
	int ret = -1;

	// we try every mirror max. 'config.tries' number of times
	for (int tries = 0; tries < config.tries && !part->done && !terminate; tries++) {
		wget_millisleep(tries * 1000 > config.waitretry ? config.waitretry : tries * 1000);

		if (terminate)
			break;

		for (int mirrors = 0; mirrors < wget_vector_size(metalink->mirrors) && !part->done; mirrors++) {
			wget_http_response_t *resp;
			wget_metalink_mirror_t *mirror = wget_vector_get(metalink->mirrors, mirror_index);

			print_status(downloader, "downloading part %d/%d (%lld-%lld) %s from %s (mirror %d)\n",
				part->id, wget_vector_size(job->parts),
				(long long)part->position, (long long)(part->position + part->length - 1),
				metalink->name, mirror->iri->host, mirror_index);

			mirror_index = (mirror_index + 1) % wget_vector_size(metalink->mirrors);

			resp = http_get(mirror->iri, part, downloader, "GET");
			if (resp) {
				wget_cookie_store_cookies(config.cookie_db, resp->cookies); // sanitize and store cookies

				// just update number bytes read (body only) for display purposes
				quota_modify_read(config.save_headers ? resp->header->length + resp->body->length : resp->body->length);

				if (resp->code != 200 && resp->code != 206) {
					print_status(downloader, "part %d download error %d\n", part->id, resp->code);
				} else if (!resp->body) {
					print_status(downloader, "part %d download error 'empty body'\n", part->id);
				} else if (resp->body->length != (size_t)part->length) {
					print_status(downloader, "part %d download error '%zd bytes of %lld expected'\n",
						part->id, resp->body->length, (long long)part->length);
				} else {
					int fd;

					print_status(downloader, "part %d downloaded\n", part->id);
					if ((fd = open(metalink->name, O_WRONLY | O_CREAT, 0644)) != -1) {
						ssize_t nbytes;
						if ((nbytes = pwrite(fd, resp->body->data, resp->body->length, part->position)) == (ssize_t)resp->body->length)
							part->done = 1; // set this when downloaded ok
						else
							error_printf(_("Failed to pwrite %zd bytes at pos %lld (%zd)\n"), resp->body->length, (long long)part->position, nbytes);

						close(fd);
					} else {
						error_printf(_("Failed to write open %s\n"), metalink->name);
						set_exit_status(3);
					}
				}

				wget_http_free_response(&resp);
			}
		}
	}

	if (part->done) {
		// check if all parts are done (downloaded + hash-checked)
		int all_done = 1, it;

		wget_thread_mutex_lock(&downloader_mutex);
		for (it = 0; it < wget_vector_size(job->parts); it++) {
			PART *p = wget_vector_get(job->parts, it);
			if (!p->done) {
				all_done = 0;
				break;
			}
		}
		wget_thread_mutex_unlock(&downloader_mutex);

		// debug_printf("all_done=%d\n",all_done);
		if (all_done) {
//		if (all_done && wget_vector_size(job->metalink->hashes) > 0) {
			// check integrity of complete file
			if (config.progress)
				bar_print(downloader->id, "Checksumming...");
			else
				print_status(downloader, "%s checking...\n", job->local_filename);
			if (job_validate_file(job)) {
				if (config.progress)
					bar_print(downloader->id, "Checksum OK");
				else
					debug_printf("checksum ok\n");
				ret = 0;
			} else {
				if (config.progress)
					bar_print(downloader->id, "Checksum FAILED");
				else
					debug_printf("checksum failed\n");
			}
		}
	} else {
		print_status(downloader, "part %d failed\n", part->id);
		part->inuse = 0; // something was wrong, reload again later
	}

	return ret;
}

// the following is just needed for the progress bar
struct _body_callback_context {
	DOWNLOADER *downloader;
	wget_buffer_t *body;
	size_t expected_length;
};
static int _get_header(void *context, wget_http_response_t *resp)
{
	struct _body_callback_context *ctx = (struct _body_callback_context *)context;

	// initialize the expected max. number of bytes for bar display
	bar_update(ctx->downloader->id, ctx->expected_length = resp->content_length, 0);

	return 0;
}
static int _get_body(void *context, const char *data, size_t length)
{
	struct _body_callback_context *ctx = (struct _body_callback_context *)context;

	wget_buffer_memcat(ctx->body, data, length); // append new data to body

	bar_update(ctx->downloader->id, ctx->expected_length, ctx->body->length);

	return 0;
}

wget_http_response_t *http_get(wget_iri_t *iri, PART *part, DOWNLOADER *downloader, const char *method)
{
	wget_iri_t *dont_free = iri;
	wget_http_connection_t *conn;
	wget_http_response_t *resp = NULL;
	wget_vector_t *challenges = NULL;
	const char *iri_scheme;
//	int max_redirect = 3;
	wget_buffer_t buf;
	char sbuf[256];
	int rc, tries = 0;

	downloader->final_error = 0;

	wget_buffer_init(&buf, sbuf, sizeof(sbuf));

	if (config.hsts && iri && iri->scheme == WGET_IRI_SCHEME_HTTP && wget_hsts_host_match(config.hsts_db, iri->host, atoi(iri->resolv_port))) {
		info_printf("HSTS in effect for %s:%s\n", iri->host, iri->resolv_port);
		iri_scheme = wget_iri_set_scheme(iri, WGET_IRI_SCHEME_HTTPS);
	} else
		iri_scheme = NULL;

	while (iri && ++tries <= config.tries) {
		conn = downloader->conn;

		if (conn && !wget_strcmp(conn->esc_host, iri->host) &&
			conn->scheme == iri->scheme &&
			!wget_strcmp(conn->port, iri->resolv_port))
		{
			debug_printf("reuse connection %s\n", conn->esc_host);
		} else {
			if (conn) {
				debug_printf("close connection %s\n", conn->esc_host);
				wget_http_close(&downloader->conn);
			}

			if ((rc = wget_http_open(&downloader->conn, iri)) == WGET_E_SUCCESS) {
				debug_printf("opened connection %s\n", downloader->conn->esc_host);
			} else {
				debug_printf("Failed to http_open (%d)\n", rc);
				if (rc == WGET_E_HANDSHAKE || rc == WGET_E_CERTIFICATE) {
					downloader->final_error = 1;
					set_exit_status(5);
				}
			}

			conn = downloader->conn;
		}

		if (conn) {
			wget_http_request_t *req;

			if (method)
				req = wget_http_create_request(iri, method);
			else if (config.post_data || config.post_file)
				req = wget_http_create_request(iri, "POST");
			else
				req = wget_http_create_request(iri, "GET");

			if (config.continue_download || config.timestamping) {
				const char *local_filename = downloader->job->local_filename;

				if (config.continue_download)
					wget_http_add_header_printf(req, "Range", "bytes=%llu-",
						get_file_size(local_filename));

				if (config.timestamping) {
					time_t mtime = get_file_mtime(local_filename);

					if (mtime) {
						char http_date[32];

						wget_http_print_date(mtime + 1, http_date, sizeof(http_date));
						wget_http_add_header(req, "If-Modified-Since", http_date);
					}
				}
			}

			// 20.06.2012: www.google.de only sends gzip responses with one of the
			// following header lines in the request.
			// User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.5) Gecko/20100101 Firefox/10.0.5 Iceweasel/10.0.5
			// User-Agent: Mozilla/5.0 (X11; Linux) KHTML/4.8.3 (like Gecko) Konqueror/4.8
			// User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/536.11 (KHTML, like Gecko) Chrome/20.0.1132.34 Safari/536.11
			// User-Agent: Opera/9.80 (X11; Linux x86_64; U; en) Presto/2.10.289 Version/12.00
			// User-Agent: Wget/1.13.4 (linux-gnu)
			//
			// Accept: prefer XML over HTML
			/*				"Accept-Encoding: gzip\r\n"\
			"User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.5) Gecko/20100101 Firefox/10.0.5 Iceweasel/10.0.5\r\n"\
			"Accept: text/html,application/xhtml+xml,application/xml;q=0.9,/;q=0.8\r\n"
			"Accept-Language: en-us,en;q=0.5\r\n");
			 */

			wget_buffer_reset(&buf);
#if WITH_ZLIB
			wget_buffer_strcat(&buf, buf.length ? ", gzip, deflate" : "gzip, deflate");
#endif
#if WITH_BZIP2
			wget_buffer_strcat(&buf, buf.length ? ", bzip2" : "bzip2");
#endif
#if WITH_LZMA
			wget_buffer_strcat(&buf, buf.length ? ", xz, lzma" : "xz, lzma");
#endif
			if (!buf.length)
				wget_buffer_strcat(&buf, "identity");

			wget_http_add_header(req, "Accept-Encoding", buf.data);

			wget_http_add_header(req, "Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");

//			if (config.spider && !config.recursive)
//				http_add_header_if_modified_since(time(NULL));
//				http_add_header(req, "If-Modified-Since", "Wed, 29 Aug 2012 00:00:00 GMT");

			if (config.user_agent)
				wget_http_add_header(req, "User-Agent", config.user_agent);

			if (config.keep_alive)
				wget_http_add_header(req, "Connection", "keep-alive");

			if (!config.cache)
				wget_http_add_header(req, "Pragma", "no-cache");

			if (config.referer)
				wget_http_add_header(req, "Referer", config.referer);
			else if (downloader->job->referer) {
				wget_iri_t *referer = downloader->job->referer;

				wget_buffer_strcpy(&buf, referer->scheme);
				wget_buffer_memcat(&buf, "://", 3);
				wget_buffer_strcat(&buf, referer->host);
				if (referer->resolv_port) {
					wget_buffer_memcat(&buf, ":", 1);
					wget_buffer_strcat(&buf, referer->resolv_port);
				}
				wget_buffer_memcat(&buf, "/", 1);
				wget_iri_get_escaped_resource(referer, &buf);

				wget_http_add_header(req, "Referer", buf.data);
			}

			if (challenges) {
				// There might be more than one challenge, we could select the most secure one.
				// Prefer 'Digest' over 'Basic'
				// the following adds an Authorization: HTTP header
				wget_http_challenge_t *challenge, *selected_challenge = NULL;

				for (int it = 0; it < wget_vector_size(challenges); it++) {
					challenge = wget_vector_get(challenges, it);

					if (wget_strcasecmp_ascii(challenge->auth_scheme, "digest")) {
						selected_challenge = challenge;
						break;
					}
					else if (wget_strcasecmp_ascii(challenge->auth_scheme, "basic")) {
						if (!selected_challenge)
							selected_challenge = challenge;
					}
				}

				if (selected_challenge) {
					if (config.http_username) {
						wget_http_add_credentials(req, selected_challenge, config.http_username, config.http_password);
					} else if (config.netrc_file) {
						static wget_thread_mutex_t
							mutex = WGET_THREAD_MUTEX_INITIALIZER;

						wget_thread_mutex_lock(&mutex);
						if (!config.netrc_db) {
							config.netrc_db = wget_netrc_db_init(NULL);
							wget_netrc_db_load(config.netrc_db, config.netrc_file);
						}
						wget_thread_mutex_unlock(&mutex);

						wget_netrc_t *netrc = wget_netrc_get(config.netrc_db, iri->host);
						if (!netrc)
							netrc = wget_netrc_get(config.netrc_db, "default");

						if (netrc) {
							wget_http_add_credentials(req, selected_challenge, netrc->login, netrc->password);
						} else {
							wget_http_add_credentials(req, selected_challenge, config.http_username, config.http_password);
						}
					} else {
						wget_http_add_credentials(req, selected_challenge, config.http_username, config.http_password);
					}
				}
			}

			if (part)
				wget_http_add_header_printf(req, "Range", "bytes=%llu-%llu",
					(unsigned long long) part->position, (unsigned long long) part->position + part->length - 1);

			// add cookies
			if (config.cookies) {
				const char *cookie_string;

				if ((cookie_string = wget_cookie_create_request_header(config.cookie_db, iri))) {
					wget_http_add_header(req, "Cookie", cookie_string);
					xfree(cookie_string);
				}
			}

			if (config.post_data) {
				size_t length = strlen(config.post_data);

				wget_http_add_header(req, "Content-Type", "application/x-www-form-urlencoded");
				wget_http_add_header_printf(req, "Content-Length", "%zu", length);
				rc = wget_http_send_request_with_body(conn, req, config.post_data, length);
			} else if (config.post_file) {
				size_t length;
				char *data;

				if ((data = wget_read_file(config.post_file, &length))) {
					wget_http_add_header(req, "Content-Type", "application/x-www-form-urlencoded");
					wget_http_add_header_printf(req, "Content-Length", "%zu", length);
					rc = wget_http_send_request_with_body(conn, req, data, length);
					xfree(data);
				} else
					break;
			} else {
				rc = wget_http_send_request(conn, req);
			}

			if (rc == WGET_E_SUCCESS) {
				if (config.progress) {
					wget_buffer_t *body = wget_buffer_alloc(102400);
					struct _body_callback_context context = { .downloader = downloader, .body = body };

					resp = wget_http_get_response_cb(conn, req, config.save_headers || config.server_response ? WGET_HTTP_RESPONSE_KEEPHEADER : 0, _get_header, _get_body, &context);

					if (resp) {
						resp->body = body;
						if (!wget_strcasecmp_ascii(req->method, "GET"))
							resp->content_length = body->length;
					} else {
						wget_buffer_free(&body);
					}

				} else
					resp = wget_http_get_response(conn, NULL, req, config.save_headers || config.server_response ? WGET_HTTP_RESPONSE_KEEPHEADER : 0);
			}

			wget_http_free_request(&req);
		} else break;

		if (!resp) {
			wget_http_close(&downloader->conn);
			break;
		}

		if (config.server_response)
			info_printf("# got header %zd bytes:\n%s\n\n", resp->header->length, resp->header->data);

		// server doesn't support keep-alive or want us to close the connection
		if (!resp->keep_alive)
			wget_http_close(&downloader->conn);

		// do some statistics
		if (resp->code == 200) {
			if (part)
				_atomic_increment_int(&stats.nchunks);
			else
				_atomic_increment_int(&stats.ndownloads);
		}
		else if (resp->code == 301 || resp->code == 302)
			_atomic_increment_int(&stats.nredirects);
		else if (resp->code == 304)
			_atomic_increment_int(&stats.nnotmodified);
		else
			_atomic_increment_int(&stats.nerrors);

		if (resp->code == 302 && resp->links && resp->digests)
			break; // 302 with Metalink information

		if (resp->code == 401 && !challenges) { // Unauthorized
			wget_http_free_challenges(&challenges);
			if ((challenges = resp->challenges)) {
				resp->challenges = NULL;
				wget_http_free_response(&resp);
				continue; // try again with credentials
			}
			break;
		}

		// 304 Not Modified
		if (resp->code / 100 == 2 || resp->code / 100 >= 4 || resp->code == 304)
			break; // final response

		if (resp->location) {
			wget_buffer_t uri_buf;
			char uri_sbuf[1024];

			wget_cookie_normalize_cookies(iri, resp->cookies);
			wget_cookie_store_cookies(config.cookie_db, resp->cookies);

			wget_buffer_init(&uri_buf, uri_sbuf, sizeof(uri_sbuf));

			wget_iri_relative_to_abs(iri, resp->location, strlen(resp->location), &uri_buf);

			if (!part) {
				add_url(downloader->job, "utf-8", uri_buf.data, URL_FLG_REDIRECTION);
				wget_buffer_deinit(&uri_buf);
				break;
			} else {
				// directly follow when using metalink
				if (iri != dont_free)
					wget_iri_free(&iri);
				iri = wget_iri_parse(uri_buf.data, NULL);
				wget_buffer_deinit(&uri_buf);

				// apply the HSTS check to the location URL
				if (config.hsts && iri && iri->scheme == WGET_IRI_SCHEME_HTTP && wget_hsts_host_match(config.hsts_db, iri->host, atoi(iri->resolv_port))) {
					info_printf("HSTS in effect for %s:%s\n", iri->host, iri->resolv_port);
					iri_scheme = wget_iri_set_scheme(iri, WGET_IRI_SCHEME_HTTPS);
				} else
					iri_scheme = NULL;
			}
		}

		wget_http_free_response(&resp);
	}

	if (iri) {
		if (iri != dont_free)
			wget_iri_free(&iri);
		else if (iri_scheme)
			wget_iri_set_scheme(iri, iri_scheme);	// may have been changed by HSTS
	}

	wget_http_free_challenges(&challenges);
	wget_buffer_deinit(&buf);

	return resp;
}
