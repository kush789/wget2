/*
 * Copyright(c) 2012 Tim Ruehsen
 * Copyright(c) 2015-2016 Free Software Foundation, Inc.
 *
 * This file is part of libwget.
 *
 * Libwget is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Libwget is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libwget.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * a collection of utility routines
 *
 * Changelog
 * 25.04.2012  Tim Ruehsen  created
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

#include "c-ctype.h"
#include "c-strcase.h"

#include <libwget.h>
#include "private.h"

/**
 * \file
 * \brief General utility functions
 * \defgroup libwget-utils General utility functions
 * @{
 *
 * This is a collections of short routines that are used with libwget and/or Wget code.
 * They may be useful to other developers that is why they are exported.
 */

/**
 * \param[in] s1 String
 * \param[in] s2 String
 * \return
 * 0 if both \p s1 and \p s2 are NULL<br>
 * -1 if \p s1 is NULL and \p s2 is not NULL<br>
 * 1 if \p s1 is not NULL and \p s2 is NULL
 * else it returns strcmp(\p s1, \p s2)
 *
 * This functions compares \p s1 and \p s2 in the same way as strcmp() does,
 * except that it also handles NULL values.
 */
int wget_strcmp(const char *s1, const char *s2)
{
	if (!s1) {
		if (!s2)
			return 0;
		else
			return -1;
	} else {
		if (!s2)
			return 1;
		else
			return strcmp(s1, s2);
	}
}

/**
 * \param[in] s1 String
 * \param[in] s2 String
 * \return
 * 0 if both \p s1 and \p s2 are NULL<br>
 * -1 if \p s1 is NULL and \p s2 is not NULL<br>
 * 1 if \p s1 is not NULL and \p s2 is NULL
 * else it returns strcasecmp(\p s1, \p s2)
 *
 * This functions compares \p s1 and \p s2 in the same way as strcasecmp() does,
 * except that it also handles NULL values.
 */
int wget_strcasecmp(const char *s1, const char *s2)
{
	if (!s1) {
		if (!s2)
			return 0;
		else
			return -1;
	} else {
		if (!s2)
			return 1;
		else
			return strcasecmp(s1, s2);
	}
}

/**
 * \param[in] s1 String
 * \param[in] s2 String
 * \return
 * 0 if both \p s1 and \p s2 are the same disregarding case for ASCII letters a-z<br>
 * 0 if both \p s1 and \p s2 are NULL<br>
 * <0 if \p s1 is NULL and \p s2 is not NULL or \p s1 is smaller than \p s2<br>
 * >0 if \p s2 is NULL and \p s1 is not NULL or \p s1 is greater than \p s2.
 *
 * This functions compares \p s1 and \p s2 as ASCII strings, case insensitive.
 * It also accepts NULL values.
 */
int wget_strcasecmp_ascii(const char *s1, const char *s2)
{
	if (!s1) {
		if (!s2)
			return 0;
		else
			return -1;
	} else {
		if (!s2)
			return 1;
		else
			return c_strcasecmp(s1, s2);
	}
}

/**
 * \param[in] s1 String
 * \param[in] s2 String
 * \param[in] n Max. number of chars to compare
 * \return
 * 0 if both \p s1 and \p s2 are the same disregarding case for ASCII letters a-z<br>
 * 0 if both \p s1 and \p s2 are NULL<br>
 * <0 if \p s1 is NULL and \p s2 is not NULL or \p s1 is smaller than \p s2<br>
 * >0 if \p s2 is NULL and \p s1 is not NULL or \p s1 is greater than \p s2.
 *
 * This functions compares \p s1 and \p s2 as ASCII strings, case insensitive, up to a max number of \p n chars.
 * It also accepts NULL values.
 */
int wget_strncasecmp_ascii(const char *s1, const char *s2, size_t n)
{
	if (!s1) {
		if (!s2)
			return 0;
		else
			return -1;
	} else {
		if (!s2)
			return 1;
		else
			return c_strncasecmp(s1, s2, n);
	}
}

/**
 * @param[in,out] s String to convert
 * \return Value of s
 *
 * Converts ASCII string \p s to lowercase in place.
 */
char *wget_strtolower(char *s)
{
	if (s) {
		for (char *d = s; *d; d++) {
			if (c_isupper(*d))
				*d = c_tolower(*d);
		}
	}

	return s;
}

/**
 * \param[in] s1 String
 * \param[in] s2 String
 * \param[in] n Max. number of chars to compare
 * \return
 * 0 if both \p s1 and \p s2 are the same or if both \p s1 and \p s2 are NULL<br>
 * <0 if \p s1 is NULL and \p s2 is not NULL or \p s1 is smaller than \p s2<br>
 * >0 if \p s2 is NULL and \p s1 is not NULL or \p s1 is greater than \p s2.
 *
 * This functions compares \p s1 and \p s2 in the same way as strncmp() does,
 * except that it also handles NULL values.
 */
int wget_strncmp(const char *s1, const char *s2, size_t n)
{
	if (!s1) {
		if (!s2)
			return 0;
		else
			return -1;
	} else {
		if (!s2)
			return 1;
		else
			return strncmp(s1, s2, n);
	}
}

/**
 * \param[in] s1 String
 * \param[in] s2 String
 * \param[in] n Max. number of chars to compare
 * \return
 * 0 if both \p s1 and \p s2 are the same disregarding case or if both \p s1 and \p s2 are NULL<br>
 * <0 if \p s1 is NULL and \p s2 is not NULL or \p s1 is smaller than \p s2<br>
 * >0 if \p s2 is NULL and \p s1 is not NULL or \p s1 is greater than \p s2.
 *
 * This functions compares \p s1 and \p s2 in the same way as strncasecmp() does,
 * except that it also handles NULL values.
 */
int wget_strncasecmp(const char *s1, const char *s2, size_t n)
{
	if (!s1) {
		if (!s2)
			return 0;
		else
			return -1;
	} else {
		if (!s2)
			return 1;
		else
			return strncasecmp(s1, s2, n);
	}
}

/**
 * \param[in] src Pointer to input buffer
 * \param[in] src_len Number of bytes to encode
 * \param[out] dst Buffer to hold the encoded string
 * \param[in] dst_size Size of \p dst in bytes
 *
 * Encodes a number of bytes into a lowercase hexadecimal C string.
 */
void wget_memtohex(const unsigned char *src, size_t src_len, char *dst, size_t dst_size)
{
	size_t it;
	int adjust = 0, c;

	if (dst_size == 0)
		return;

	if (src_len * 2 >= dst_size) {
		src_len = (dst_size - 1) / 2;
		adjust = 1;
	}

	for (it = 0; it < src_len; it++, src++) {
		*dst++ = (c = (*src >> 4)) >= 10 ? c + 'a' - 10 : c + '0';
		*dst++ = (c = (*src & 0xf)) >= 10 ? c + 'a' - 10 : c + '0';
	}
	if (adjust && (dst_size & 1) == 0)
		*dst++ = (c = (*src >> 4)) >= 10 ? c + 'a' - 10 : c + '0';

	*dst = 0;
}

/**
 * \param[in] ms Number of milliseconds to sleep
 *
 * Pause for \p ms milliseconds.
 */
void wget_millisleep(int ms)
{
	if (ms <= 0)
		return;

	nanosleep(&(struct timespec){ .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000 }, NULL);
}

static _GL_INLINE unsigned char G_GNUC_WGET_CONST _unhex(unsigned char c)
{
	return c <= '9' ? c - '0' : (c <= 'F' ? c - 'A' + 10 : c - 'a' + 10);
}

/**
 * \param[in,out] src String to unescape
 * \return
 * 0 if the string did not change<br>
 * 1 if unescaping took place
 *
 * Does an inline percent unescape.
 * Each occurrence of %xx (x = hex digit) will converted into it's byte representation.
 */
int wget_percent_unescape(char *src)
{
	int ret = 0;
	unsigned char *s = (unsigned char *)src; // just a helper to avoid casting a lot
	unsigned char *d = s;

	while (*s) {
		if (*s == '%') {
			if (c_isxdigit(s[1]) && c_isxdigit(s[2])) {
				*d++ = (_unhex(s[1]) << 4) | _unhex(s[2]);
				s += 3;
				ret = 1;
				continue;
			}
		}

		*d++ = *s++;
	}
	*d = 0;

	return ret;
}

/**
 * \param[in] s String
 * \param[in] tail String
 * \return 1 if \p tail matches the end of \p s, 0 if not
 *
 * Checks if \p tail matches the end of the string \p s.
 */
int wget_match_tail(const char *s, const char *tail)
{
	const char *p = s + strlen(s) - strlen(tail);

	return p >= s && !strcmp(p, tail);
}

/**
 * \param[in] s String
 * \param[in] tail String
 * \return 1 if \p tail matches the end of \p s, 0 if not
 *
 * Checks if \p tail matches the end of the string \p s, disregarding the case, ASCII only.
 *
 */
int wget_match_tail_nocase(const char *s, const char *tail)
{
	const char *p = s + strlen(s) - strlen(tail);

	return p >= s && !wget_strcasecmp_ascii(p, tail);
}

/**@}*/
