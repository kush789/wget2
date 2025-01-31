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
 * HTTP routines
 *
 * Changelog
 * 25.04.2012  Tim Ruehsen  created
 * 26.10.2012               added Cookie support (RFC 6265)
 *
 * Resources:
 * RFC 2616
 * RFC 6265
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <c-ctype.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#if WITH_ZLIB
//#include <zlib.h>
#endif
#ifdef WITH_LIBNGHTTP2
	#include <nghttp2/nghttp2.h>
#endif

#include <libwget.h>
#include "private.h"

#define HTTP_CTYPE_SEPERATOR (1<<0)
#define _http_isseperator(c) (http_ctype[(unsigned char)(c)]&HTTP_CTYPE_SEPERATOR)

static const unsigned char
	http_ctype[256] = {
		['('] = HTTP_CTYPE_SEPERATOR,
		[')'] = HTTP_CTYPE_SEPERATOR,
		['<'] = HTTP_CTYPE_SEPERATOR,
		['>'] = HTTP_CTYPE_SEPERATOR,
		['@'] = HTTP_CTYPE_SEPERATOR,
		[','] = HTTP_CTYPE_SEPERATOR,
		[';'] = HTTP_CTYPE_SEPERATOR,
		[':'] = HTTP_CTYPE_SEPERATOR,
		['\\'] = HTTP_CTYPE_SEPERATOR,
		['\"'] = HTTP_CTYPE_SEPERATOR,
		['/'] = HTTP_CTYPE_SEPERATOR,
		['['] = HTTP_CTYPE_SEPERATOR,
		[']'] = HTTP_CTYPE_SEPERATOR,
		['?'] = HTTP_CTYPE_SEPERATOR,
		['='] = HTTP_CTYPE_SEPERATOR,
		['{'] = HTTP_CTYPE_SEPERATOR,
		['}'] = HTTP_CTYPE_SEPERATOR,
		[' '] = HTTP_CTYPE_SEPERATOR,
		['\t'] = HTTP_CTYPE_SEPERATOR
	};

static char
	_abort_indicator;

static wget_vector_t
	*http_proxies,
	*https_proxies;

int wget_http_isseperator(char c)
{
	// return strchr("()<>@,;:\\\"/[]?={} \t", c) != NULL;
	return _http_isseperator(c);
}

// TEXT           = <any OCTET except CTLs, but including LWS>
//int http_istext(char c)
//{
//	return (c>=32 && c<=126) || c=='\r' || c=='\n' || c=='\t';
//}

// token          = 1*<any CHAR except CTLs or separators>

int wget_http_istoken(char c)
{
	return c > 32 && c <= 126 && !_http_isseperator(c);
}

const char *wget_http_parse_token(const char *s, const char **token)
{
	const char *p;

	for (p = s; wget_http_istoken(*s); s++);

	*token = wget_strmemdup(p, s - p);

	return s;
}

// quoted-string  = ( <"> *(qdtext | quoted-pair ) <"> )
// qdtext         = <any TEXT except <">>
// quoted-pair    = "\" CHAR
// TEXT           = <any OCTET except CTLs, but including LWS>
// CTL            = <any US-ASCII control character (octets 0 - 31) and DEL (127)>
// LWS            = [CRLF] 1*( SP | HT )

const char *wget_http_parse_quoted_string(const char *s, const char **qstring)
{
	if (*s == '\"') {
		const char *p = ++s;

		// relaxed scanning
		while (*s) {
			if (*s == '\"') break;
			else if (*s == '\\' && s[1]) {
				s += 2;
			} else
				s++;
		}

		*qstring = wget_strmemdup(p, s - p);
		if (*s == '\"') s++;
	} else
		*qstring = NULL;

	return s;
}

// generic-param  =  token [ EQUAL gen-value ]
// gen-value      =  token / host / quoted-string

const char *wget_http_parse_param(const char *s, const char **param, const char **value)
{
	const char *p;

	*param = *value = NULL;

	while (c_isblank(*s)) s++;

	if (*s == ';') {
		s++;
		while (c_isblank(*s)) s++;
	}
	if (!*s) return s;

	for (p = s; wget_http_istoken(*s); s++);
	*param = wget_strmemdup(p, s - p);

	while (c_isblank(*s)) s++;

	if (*s && *s++ == '=') {
		while (c_isblank(*s)) s++;
		if (*s == '\"') {
			s = wget_http_parse_quoted_string(s, value);
		} else {
			s = wget_http_parse_token(s, value);
		}
	}

	return s;
}

// message-header = field-name ":" [ field-value ]
// field-name     = token
// field-value    = *( field-content | LWS )
// field-content  = <the OCTETs making up the field-value
//                  and consisting of either *TEXT or combinations
//                  of token, separators, and quoted-string>

const char *wget_http_parse_name(const char *s, const char **name)
{
	while (c_isblank(*s)) s++;

	s = wget_http_parse_token(s, name);

	while (*s && *s != ':') s++;

	return *s == ':' ? s + 1 : s;
}

const char *wget_parse_name_fixed(const char *s, const char **name, size_t *namelen)
{
	while (c_isblank(*s)) s++;

	*name = s;

	while (wget_http_istoken(*s))
		s++;

	*namelen = s - *name;

	while (*s && *s != ':') s++;

	return *s == ':' ? s + 1 : s;
}

static int G_GNUC_WGET_NONNULL_ALL compare_param(wget_http_header_param_t *p1, wget_http_header_param_t *p2)
{
	return wget_strcasecmp_ascii(p1->name, p2->name);
}

void wget_http_add_param(wget_vector_t **params, wget_http_header_param_t *param)
{
	if (!*params) *params = wget_vector_create(4, 4, (int(*)(const void *, const void *))compare_param);
	wget_vector_add(*params, param, sizeof(*param));
}

/*
  Link           = "Link" ":" #link-value
  link-value     = "<" URI-Reference ">" *( ";" link-param )
  link-param     = ( ( "rel" "=" relation-types )
					  | ( "anchor" "=" <"> URI-Reference <"> )
					  | ( "rev" "=" relation-types )
					  | ( "hreflang" "=" Language-Tag )
					  | ( "media" "=" ( MediaDesc | ( <"> MediaDesc <"> ) ) )
					  | ( "title" "=" quoted-string )
					  | ( "title*" "=" ext-value )
					  | ( "type" "=" ( media-type | quoted-mt ) )
					  | ( link-extension ) )
  link-extension = ( parmname [ "=" ( ptoken | quoted-string ) ] )
					  | ( ext-name-star "=" ext-value )
  ext-name-star  = parmname "*" ; reserved for RFC2231-profiled
										  ; extensions.  Whitespace NOT
										  ; allowed in between.
  ptoken         = 1*ptokenchar
  ptokenchar     = "!" | "#" | "$" | "%" | "&" | "'" | "("
					  | ")" | "*" | "+" | "-" | "." | "/" | DIGIT
					  | ":" | "<" | "=" | ">" | "?" | "@" | ALPHA
					  | "[" | "]" | "^" | "_" | "`" | "{" | "|"
					  | "}" | "~"
  media-type     = type-name "/" subtype-name
  quoted-mt      = <"> media-type <">
  relation-types = relation-type
					  | <"> relation-type *( 1*SP relation-type ) <">
  relation-type  = reg-rel-type | ext-rel-type
  reg-rel-type   = LOALPHA *( LOALPHA | DIGIT | "." | "-" )
  ext-rel-type   = URI
*/
const char *wget_http_parse_link(const char *s, wget_http_link_t *link)
{
	memset(link, 0, sizeof(*link));

	while (c_isblank(*s)) s++;

	if (*s == '<') {
		// URI reference as of RFC 3987 (if relative, resolve as of RFC 3986)
		const char *p = s + 1;
		if ((s = strchr(p, '>')) != NULL) {
			const char *name = NULL, *value = NULL;

			link->uri = wget_strmemdup(p, s - p);
			s++;

			while (c_isblank(*s)) s++;

			while (*s == ';') {
				s = wget_http_parse_param(s, &name, &value);
				if (name && value) {
					if (!wget_strcasecmp_ascii(name, "rel")) {
						if (!wget_strcasecmp_ascii(value, "describedby"))
							link->rel = link_rel_describedby;
						else if (!wget_strcasecmp_ascii(value, "duplicate"))
							link->rel = link_rel_duplicate;
					} else if (!wget_strcasecmp_ascii(name, "pri")) {
						link->pri = atoi(value);
					} else if (!wget_strcasecmp_ascii(name, "type")) {
						link->type = value;
						value = NULL;
					}
					//				http_add_param(&link->params,&param);
					while (c_isblank(*s)) s++;
				}

				xfree(name);
				xfree(value);
			}

			//			if (!msg->contacts) msg->contacts=vec_create(1,1,NULL);
			//			vec_add(msg->contacts,&contact,sizeof(contact));

			while (*s && !c_isblank(*s)) s++;
		}
	}

	return s;
}

// from RFC 3230:
// Digest = "Digest" ":" #(instance-digest)
// instance-digest = digest-algorithm "=" <encoded digest output>
// digest-algorithm = token

const char *wget_http_parse_digest(const char *s, wget_http_digest_t *digest)
{
	const char *p;

	memset(digest, 0, sizeof(*digest));

	while (c_isblank(*s)) s++;
	s = wget_http_parse_token(s, &digest->algorithm);

	while (c_isblank(*s)) s++;

	if (*s == '=') {
		s++;
		while (c_isblank(*s)) s++;
		if (*s == '\"') {
			s = wget_http_parse_quoted_string(s, &digest->encoded_digest);
		} else {
			for (p = s; *s && !c_isblank(*s) && *s != ',' && *s != ';'; s++);
			digest->encoded_digest = wget_strmemdup(p, s - p);
		}
	}

	while (*s && !c_isblank(*s)) s++;

	return s;
}

// RFC 2617:
// challenge   = auth-scheme 1*SP 1#auth-param
// auth-scheme = token
// auth-param  = token "=" ( token | quoted-string )

const char *wget_http_parse_challenge(const char *s, wget_http_challenge_t *challenge)
{
	const char *old;

	memset(challenge, 0, sizeof(*challenge));

	while (c_isblank(*s)) s++;
	s = wget_http_parse_token(s, &challenge->auth_scheme);

	if (*s == ' ')
		s++; // Auth scheme must have a space at the end of the token
	else {
		// parse/syntax error
		xfree(challenge->auth_scheme);
		return s;
	}

	wget_http_header_param_t param;
	do {
		old = s;
		s = wget_http_parse_param(s, &param.name, &param.value);
		if (param.name) {
			if (*param.name && !param.value) {
				xfree(param.name);
				return old; // a new scheme detected
			}

			if (!param.value) {
				xfree(param.name);
				continue;
			}

			if (!challenge->params)
				challenge->params = wget_stringmap_create_nocase(8);
			wget_stringmap_put_noalloc(challenge->params, param.name, param.value);
		}

		while (c_isblank(*s)) s++;

		if (*s != ',') break;
		else if (*s) s++;
	} while (*s);

	return s;
}

const char *wget_http_parse_challenges(const char *s, wget_vector_t *challenges)
{
	wget_http_challenge_t challenge;

	while (*s) {
		s = wget_http_parse_challenge(s, &challenge);
		if (challenge.auth_scheme) {
			wget_vector_add(challenges, &challenge, sizeof(challenge));
		}
	}

	return s;
}

const char *wget_http_parse_location(const char *s, const char **location)
{
	const char *p;

	while (c_isblank(*s)) s++;

	for (p = s; *s && !c_isblank(*s); s++);
	*location = wget_strmemdup(p, s - p);

	return s;
}

// Transfer-Encoding       = "Transfer-Encoding" ":" 1#transfer-coding
// transfer-coding         = "chunked" | transfer-extension
// transfer-extension      = token *( ";" parameter )
// parameter               = attribute "=" value
// attribute               = token
// value                   = token | quoted-string

const char *wget_http_parse_transfer_encoding(const char *s, char *transfer_encoding)
{
	while (c_isblank(*s)) s++;

	if (!wget_strcasecmp_ascii(s, "identity"))
		*transfer_encoding = transfer_encoding_identity;
	else
		*transfer_encoding = transfer_encoding_chunked;

	while (wget_http_istoken(*s)) s++;

	return s;
}

// Content-Type   = "Content-Type" ":" media-type
// media-type     = type "/" subtype *( ";" parameter )
// type           = token
// subtype        = token
// example: Content-Type: text/html; charset=ISO-8859-4

const char *wget_http_parse_content_type(const char *s, const char **content_type, const char **charset)
{
	wget_http_header_param_t param;
	const char *p;

	while (c_isblank(*s)) s++;

	for (p = s; *s && (wget_http_istoken(*s) || *s == '/'); s++);
	if (content_type)
		*content_type = wget_strmemdup(p, s - p);

	if (charset) {
		*charset = NULL;

		while (*s) {
			s=wget_http_parse_param(s, &param.name, &param.value);
			if (!wget_strcasecmp_ascii("charset", param.name)) {
				xfree(param.name);
				*charset = param.value;
				break;
			}
			xfree(param.name);
			xfree(param.value);
		}
	}

	return s;
}

// RFC 2183
//
// disposition := "Content-Disposition" ":" disposition-type *(";" disposition-parm)
// disposition-type := "inline" / "attachment" / extension-token   ; values are not case-sensitive
// disposition-parm := filename-parm / creation-date-parm / modification-date-parm
//                     / read-date-parm / size-parm / parameter
// filename-parm := "filename" "=" value
// creation-date-parm := "creation-date" "=" quoted-date-time
// modification-date-parm := "modification-date" "=" quoted-date-time
// read-date-parm := "read-date" "=" quoted-date-time
// size-parm := "size" "=" 1*DIGIT
// quoted-date-time := quoted-string
//                     ; contents MUST be an RFC 822 `date-time'
//                     ; numeric timezones (+HHMM or -HHMM) MUST be used

const char *wget_http_parse_content_disposition(const char *s, const char **filename)
{
	wget_http_header_param_t param;
	char *p;

	if (filename) {
		*filename = NULL;

		while (*s) {
			s = wget_http_parse_param(s, &param.name, &param.value);
			if (param.value && !wget_strcasecmp_ascii("filename", param.name)) {
				// just take the last path part as filename
				if (!*filename) {
					if ((p = strpbrk(param.value,"/\\"))) {
						p = strdup(p + 1);
					} else {
						p = (char *) param.value;
						param.value = NULL;
					}

					wget_percent_unescape(p);
					if (!wget_str_is_valid_utf8(p)) {
						// if it is not UTF-8, assume ISO-8859-1
						// see http://stackoverflow.com/questions/93551/how-to-encode-the-filename-parameter-of-content-disposition-header-in-http
						*filename = wget_str_to_utf8(p, "iso-8859-1");
						xfree(p);
					} else {
						*filename = p;
						p = NULL;
					}
				}
			} else if (param.value && !wget_strcasecmp_ascii("filename*", param.name)) {
				// RFC5987
				// ext-value     = charset  "'" [ language ] "'" value-chars
				// ; like RFC 2231's <extended-initial-value>
				// ; (see [RFC2231], Section 7)

				// charset       = "UTF-8" / "ISO-8859-1" / mime-charset

				// mime-charset  = 1*mime-charsetc
				// mime-charsetc = ALPHA / DIGIT
				//		/ "!" / "#" / "$" / "%" / "&"
				//		/ "+" / "-" / "^" / "_" / "`"
				//		/ "{" / "}" / "~"
				//		; as <mime-charset> in Section 2.3 of [RFC2978]
				//		; except that the single quote is not included
				//		; SHOULD be registered in the IANA charset registry

				// language      = <Language-Tag, defined in [RFC5646], Section 2.1>

				// value-chars   = *( pct-encoded / attr-char )

				// pct-encoded   = "%" HEXDIG HEXDIG
				//		; see [RFC3986], Section 2.1

				// attr-char     = ALPHA / DIGIT
				//		/ "!" / "#" / "$" / "&" / "+" / "-" / "."
				//		/ "^" / "_" / "`" / "|" / "~"
				//		; token except ( "*" / "'" / "%" )

				if ((p = strchr(param.value, '\''))) {
					const char *charset = param.value;
					const char *language = p + 1;
					*p = 0;
					if ((p = strchr(language, '\''))) {
						*p++ = 0;
						if (*p) {
							wget_percent_unescape(p);
							if (wget_str_needs_encoding(p))
								*filename = wget_str_to_utf8(p, charset);
							else
								*filename = strdup(p);

							// just take the last path part as filename
							if ((p = strpbrk(*filename, "/\\"))) {
								p = strdup(p + 1);
								xfree(*filename);
								*filename = p;
							}

							xfree(param.name);
							xfree(param.value);
							break; // stop looping, we found the final filename
						}
					}
				}
			}
			xfree(param.name);
			xfree(param.value);
		}
	}

	return s;
}

// RFC 6797
//
// Strict-Transport-Security = "Strict-Transport-Security" ":" [ directive ]  *( ";" [ directive ] )
// directive                 = directive-name [ "=" directive-value ]
// directive-name            = token
// directive-value           = token | quoted-string

const char *wget_http_parse_strict_transport_security(const char *s, time_t *maxage, char *include_subdomains)
{
	wget_http_header_param_t param;

	*maxage = 0;
	*include_subdomains = 0;

	while (*s) {
		s = wget_http_parse_param(s, &param.name, &param.value);

		if (param.value) {
			if (!wget_strcasecmp_ascii(param.name, "max-age")) {
				long offset = atol(param.value);

				if (offset > 0)
					*maxage = time(NULL) + offset;
				else
					*maxage = 0; // keep 0 as a special value: remove entry from HSTS database
			}
		} else {
			if (!wget_strcasecmp_ascii(param.name, "includeSubDomains")) {
				*include_subdomains = 1;
			}
		}

		xfree(param.name);
		xfree(param.value);
	}

	return s;
}

// Content-Encoding  = "Content-Encoding" ":" 1#content-coding

const char *wget_http_parse_content_encoding(const char *s, char *content_encoding)
{
	while (c_isblank(*s)) s++;

	if (!wget_strcasecmp_ascii(s, "gzip") || !wget_strcasecmp_ascii(s, "x-gzip"))
		*content_encoding = wget_content_encoding_gzip;
	else if (!wget_strcasecmp_ascii(s, "deflate"))
		*content_encoding = wget_content_encoding_deflate;
	else if (!wget_strcasecmp_ascii(s, "bzip2"))
		*content_encoding = wget_content_encoding_bzip2;
	else if (!wget_strcasecmp_ascii(s, "xz") || !wget_strcasecmp_ascii(s, "lzma") || !wget_strcasecmp_ascii(s, "x-lzma"))
		// 'xz' is the tag currently understood by Firefox (2.1.2014)
		// 'lzma' / 'x-lzma' are the tags currently understood by ELinks
		*content_encoding = wget_content_encoding_lzma;
	else
		*content_encoding = wget_content_encoding_identity;

	while (wget_http_istoken(*s)) s++;

	return s;
}

const char *wget_http_parse_connection(const char *s, char *keep_alive)
{
	while (c_isblank(*s)) s++;

	if (!wget_strcasecmp_ascii(s, "keep-alive"))
		*keep_alive = 1;
	else
		*keep_alive = 0;

	while (wget_http_istoken(*s)) s++;

	return s;
}

const char *wget_http_parse_etag(const char *s, const char **etag)
{
	const char *p;

	while (c_isblank(*s)) s++;

	for (p = s; *s && !c_isblank(*s); s++);
	*etag = wget_strmemdup(p, s - p);

	return s;
}

/*
// returns GMT/UTC time as an integer of format YYYYMMDDHHMMSS
// this makes us independant from size of time_t - work around possible year 2038 problems
static long long NONNULL_ALL parse_rfc1123_date(const char *s)
{
	// we simply can't use strptime() since it requires us to setlocale()
	// which is not thread-safe !!!
	static const char *mnames[12] = {
		"Jan", "Feb", "Mar","Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	static int days_per_month[12] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};
	int day, mon = 0, year, hour, min, sec, leap, it;
	char mname[4] = "";

	if (sscanf(s, " %*[a-zA-Z], %02d %3s %4d %2d:%2d:%2d", &day, mname, &year, &hour, &min, &sec) >= 6) {
		// RFC 822 / 1123: Wed, 09 Jun 2021 10:18:14 GMT
	}
	else if (sscanf(s, " %*[a-zA-Z], %2d-%3s-%4d %2d:%2d:%2d", &day, mname, &year, &hour, &min, &sec) >= 6) {
		// RFC 850 / 1036 or Netscape: Wednesday, 09-Jun-21 10:18:14 or Wed, 09-Jun-2021 10:18:14
	}
	else if (sscanf(s, " %*[a-zA-Z], %3s %2d %2d:%2d:%2d %4d", mname, &day, &hour, &min, &sec, &year) >= 6) {
		// ANSI C's asctime(): Wed Jun 09 10:18:14 2021
	} else {
		err_printf(_("Failed to parse date '%s'\n"), s);
		return 0; // return as session cookie
	}

	if (*mname) {
		for (it = 0; it < countof(mnames); it++) {
			if (!wget_strcasecmp_ascii(mname, mnames[it])) {
				mon = it + 1;
				break;
			}
		}
	}

	if (year < 70 && year >= 0) year += 2000;
	else if (year >= 70 && year <= 99) year += 1900;

	if (mon == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
		leap = 1;
	else
		leap = 0;

	// we don't handle leap seconds

	if (year < 1601 || mon < 1 || mon > 12 || day < 1 || (day > days_per_month[mon - 1] + leap) ||
		hour < 0 || hour > 23 || min < 0 || min > 60 || sec < 0 || sec > 60)
	{
		err_printf(_("Failed to parse date '%s'\n"), s);
		return 0; // return as session cookie
	}

	return(((((long long)year*100 + mon)*100 + day)*100 + hour)*100 + min)*100 + sec;
}
*/

// copied this routine from
// http://ftp.netbsd.org/pub/pkgsrc/current/pkgsrc/pkgtools/libnbcompat/files/timegm.c

static int leap_days(int y1, int y2)
{
	y1--;
	y2--;
	return (y2/4 - y1/4) - (y2/100 - y1/100) + (y2/400 - y1/400);
}

/*
RFC 2616, 3.3.1 Full Date
HTTP-date    = rfc1123-date | rfc850-date | asctime-date
rfc1123-date = wkday "," SP date1 SP time SP "GMT"
rfc850-date  = weekday "," SP date2 SP time SP "GMT"
asctime-date = wkday SP date3 SP time SP 4DIGIT
date1        = 2DIGIT SP month SP 4DIGIT
					; day month year (e.g., 02 Jun 1982)
date2        = 2DIGIT "-" month "-" 2DIGIT
					; day-month-year (e.g., 02-Jun-82)
date3        = month SP ( 2DIGIT | ( SP 1DIGIT ))
					; month day (e.g., Jun  2)
time         = 2DIGIT ":" 2DIGIT ":" 2DIGIT
					; 00:00:00 - 23:59:59
wkday        = "Mon" | "Tue" | "Wed"
				 | "Thu" | "Fri" | "Sat" | "Sun"
weekday      = "Monday" | "Tuesday" | "Wednesday"
				 | "Thursday" | "Friday" | "Saturday" | "Sunday"
month        = "Jan" | "Feb" | "Mar" | "Apr"
				 | "May" | "Jun" | "Jul" | "Aug"
				 | "Sep" | "Oct" | "Nov" | "Dec"
*/

time_t wget_http_parse_full_date(const char *s)
{
	// we simply can't use strptime() since it requires us to setlocale()
	// which is not thread-safe !!!
	static const char *mnames[12] = {
		"Jan", "Feb", "Mar","Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	static int days_per_month[12] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};
	// cumulated number of days until beginning of month for non-leap years
	static const int sum_of_days[12] = {
		0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
	};

	int day, mon = 0, year, hour, min, sec, leap_month, leap_year, days;
	char mname[4] = "";

	if (sscanf(s, " %*[a-zA-Z], %02d %3s %4d %2d:%2d:%2d", &day, mname, &year, &hour, &min, &sec) >= 6) {
		// RFC 822 / 1123: Wed, 09 Jun 2021 10:18:14 GMT
	}
	else if (sscanf(s, " %*[a-zA-Z], %2d-%3s-%4d %2d:%2d:%2d", &day, mname, &year, &hour, &min, &sec) >= 6) {
		// RFC 850 / 1036 or Netscape: Wednesday, 09-Jun-21 10:18:14 or Wed, 09-Jun-2021 10:18:14
	}
	else if (sscanf(s, " %*[a-zA-Z] %3s %2d %2d:%2d:%2d %4d", mname, &day, &hour, &min, &sec, &year) >= 6) {
		// ANSI C's asctime(): Wed Jun 09 10:18:14 2021
	} else {
		error_printf(_("Failed to parse date '%s'\n"), s);
		return 0; // return as session cookie
	}

	if (*mname) {
		unsigned it;

		for (it = 0; it < countof(mnames); it++) {
			if (!wget_strcasecmp_ascii(mname, mnames[it])) {
				mon = it + 1;
				break;
			}
		}
	}

	if (year < 70 && year >= 0) year += 2000;
	else if (year >= 70 && year <= 99) year += 1900;
	if (year < 1970) year = 1970;

	// we don't handle leap seconds

	leap_year = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
	leap_month = (mon == 2 && leap_year);

	if (mon < 1 || mon > 12 || day < 1 || (day > days_per_month[mon - 1] + leap_month) ||
		hour < 0 || hour > 23 || min < 0 || min > 60 || sec < 0 || sec > 60)
	{
		error_printf(_("Failed to parse date '%s'\n"), s);
		return 0; // return as session cookie
	}

	// calculate time_t from GMT/UTC time values

	days = 365 * (year - 1970) + leap_days(1970, year);
	days += sum_of_days[mon - 1] + (mon > 2 && leap_year);
	days += day - 1;

	return (((time_t)days * 24 + hour) * 60 + min) * 60 + sec;
}

char *wget_http_print_date(time_t t, char *buf, size_t bufsize)
{
	static const char *dnames[7] = {
		"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
	};
	static const char *mnames[12] = {
		"Jan", "Feb", "Mar","Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	struct tm tm;

	if (!bufsize)
		return buf;

	if (gmtime_r(&t, &tm)) {
		snprintf(buf, bufsize, "%s, %02d %s %d %02d:%02d:%02d GMT",
			dnames[tm.tm_wday],tm.tm_mday,mnames[tm.tm_mon],tm.tm_year+1900,
			tm.tm_hour, tm.tm_min, tm.tm_sec);
	} else
		*buf = 0;

	return buf;
}

// adjust time (t) by number of seconds (n)
/*
static long long adjust_time(long long t, int n)
{
	static int days_per_month[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int day, mon, year, hour, min, sec, leap;

	sec = t % 100;
	min = (t /= 100) % 100;
	hour = (t /= 100) % 100;
	day = (t /= 100) % 100;
	mon = (t /= 100) % 100;
	year = t / 100;

	sec += n;

	if (n >= 0) {
		if (sec >= 60) {
			min += sec / 60;
			sec %= 60;
		}
		if (min >= 60) {
			hour += min / 60;
			min %= 60;
		}
		if (hour >= 24) {
			day += hour / 24;
			hour %= 24;
		}
		while (1) {
			if (mon == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
				leap = 1;
			else
				leap = 0;
			if (day > days_per_month[mon - 1] + leap) {
				day -= (days_per_month[mon - 1] + leap);
				mon++;
				if (mon > 12) {
					mon = 1;
					year++;
				}
			} else break;
		}
	} else { // n<0
		if (sec < 0) {
			min += (sec - 59) / 60;
			sec = 59 + (sec + 1) % 60;
		}
		if (min < 0) {
			hour += (min - 59) / 60;
			min = 59 + (min + 1) % 60;
		}
		if (hour < 0) {
			day += (hour - 23) / 24;
			hour = 23 + (hour + 1) % 24;
		}
		for (;;) {
			if (day <= 0) {
				if (--mon < 1) {
					mon = 12;
					year--;
				}
				if (mon == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
					leap = 1;
				else
					leap = 0;
				day += (days_per_month[mon - 1] + leap);
			} else break;
		}
	}

	return (((((long long)year*100 + mon)*100 + day)*100 + hour)*100 + min)*100 + sec;
}

// return current GMT/UTC

static long long get_current_time(void)
{
	time_t t = time(NULL);
	struct tm tm;

	gmtime_r(&t, &tm);

	return (((((long long)(tm.tm_year + 1900)*100 + tm.tm_mon + 1)*100 + tm.tm_mday)*100 + tm.tm_hour)*100 + tm.tm_min)*100 + tm.tm_sec;
}
*/

/*
 RFC 6265

 set-cookie-header = "Set-Cookie:" SP set-cookie-string
 set-cookie-string = cookie-pair *( ";" SP cookie-av )
 cookie-pair       = cookie-name "=" cookie-value
 cookie-name       = token
 cookie-value      = *cookie-octet / ( DQUOTE *cookie-octet DQUOTE )
 cookie-octet      = %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E
                       ; US-ASCII characters excluding CTLs,
                       ; whitespace DQUOTE, comma, semicolon,
                       ; and backslash
 token             = <token, defined in [RFC2616], Section 2.2>

 cookie-av         = expires-av / max-age-av / domain-av /
                     path-av / secure-av / httponly-av /
                     extension-av
 expires-av        = "Expires=" sane-cookie-date
 sane-cookie-date  = <rfc1123-date, defined in [RFC2616], Section 3.3.1>
 max-age-av        = "Max-Age=" non-zero-digit *DIGIT
                       ; In practice, both expires-av and max-age-av
                       ; are limited to dates representable by the
                       ; user agent.
 non-zero-digit    = %x31-39
                       ; digits 1 through 9
 domain-av         = "Domain=" domain-value
 domain-value      = <subdomain>
                       ; defined in [RFC1034], Section 3.5, as
                       ; enhanced by [RFC1123], Section 2.1
 path-av           = "Path=" path-value
 path-value        = <any CHAR except CTLs or ";">
 secure-av         = "Secure"
 httponly-av       = "HttpOnly"
 extension-av      = <any CHAR except CTLs or ";">
*/
const char *wget_http_parse_setcookie(const char *s, wget_cookie_t *cookie)
{
	const char *name, *p;
	int allocated_cookie = (cookie == NULL);

	cookie = wget_cookie_init(cookie);

	while (c_isspace(*s)) s++;
	s = wget_http_parse_token(s, &cookie->name);
	while (c_isspace(*s)) s++;

	if (cookie->name && *cookie->name && *s == '=') {
		// *cookie-octet / ( DQUOTE *cookie-octet DQUOTE )
		for (s++; c_isspace(*s);) s++;

		if (*s == '\"')
			s++;

		// cookie-octet      = %x21 / %x23-2B / %x2D-3A / %x3C-5B / %x5D-7E
		for (p = s; *s > 32 && *s <= 126 && *s != '\\' && *s != ',' && *s != ';' && *s != '\"'; s++);
		cookie->value = wget_strmemdup(p, s - p);

		do {
			while (*s && *s != ';') s++;
			if (!*s) break;

			for (s++; c_isspace(*s);) s++;
			s = wget_http_parse_token(s, &name);

			if (name) {
				while (*s && *s != '=' && *s != ';') s++;
				// if (!*s) break;

				if (*s == '=') {
					// find end of value
					for (p = ++s; *s > 32 && *s <= 126 && *s != ';'; s++);

					if (!wget_strcasecmp_ascii(name, "expires")) {
						cookie->expires = wget_http_parse_full_date(p);
					} else if (!wget_strcasecmp_ascii(name, "max-age")) {
						long offset = atol(p);

						if (offset > 0)
							// cookie->maxage = adjust_time(get_current_time(), offset);
							cookie->maxage = time(NULL) + offset;
						else
							cookie->maxage = 0;
					} else if (!wget_strcasecmp_ascii(name, "domain")) {
						if (p != s) {
							if (cookie->domain)
								xfree(cookie->domain);

							if (*p == '.') { // RFC 6265 5.2.3
								do { p++; } while (*p == '.');
								cookie->domain_dot = 1;
							} else
								cookie->domain_dot = 0;

							cookie->domain = wget_strmemdup(p, s - p);
						}
					} else if (!wget_strcasecmp_ascii(name, "path")) {
						if (cookie->path)
							xfree(cookie->path);
						cookie->path = wget_strmemdup(p, s - p);
					} else {
						debug_printf("Unsupported cookie-av '%s'\n", name);
					}
				} else if (!wget_strcasecmp_ascii(name, "secure")) {
					cookie->secure_only = 1;
				} else if (!wget_strcasecmp_ascii(name, "httponly")) {
					cookie->http_only = 1;
				} else {
					debug_printf("Unsupported cookie-av '%s'\n", name);
				}

				xfree(name);
			}
		} while (*s);

		if (allocated_cookie)
			wget_cookie_free(&cookie);

	} else {
		if (allocated_cookie)
			wget_cookie_free(&cookie);
		else
			wget_cookie_deinit(cookie);
		error_printf("Cookie without name or assignment ignored\n");
	}

	return s;
}

/* content of <buf> will be destroyed */

/* buf must be 0-terminated */
wget_http_response_t *wget_http_parse_response_header(char *buf)
{
	const char *s;
	char *line, *eol;
	const char *name;
	size_t namelen;
	wget_http_response_t *resp = NULL;

	resp = xcalloc(1, sizeof(wget_http_response_t));

	if (sscanf(buf, " HTTP/%3hd.%3hd %3hd %31[^\r\n] ",
		&resp->major, &resp->minor, &resp->code, resp->reason) >= 3)
	{
		if ((eol = strchr(buf + 10, '\n'))) {
			// eol[-1]=0;
			// debug_printf("# %s\n",buf);
		} else {
			// empty HTTP header
			return resp;
		}
	} else {
		error_printf(_("HTTP response header not found\n"));
		xfree(resp);
		return NULL;
	}

	for (line = eol + 1; eol && *line && *line != '\r'; line = eol + 1) {
		eol = strchr(line + 1, '\n');
		while (eol && c_isblank(eol[1])) { // handle split lines
			*eol = eol[-1] = ' ';
			eol = strchr(eol + 1, '\n');
		}

		if (eol)
			eol[-1] = 0;

		// debug_printf("# %p %s\n",eol,line);

		s = wget_parse_name_fixed(line, &name, &namelen);
		// s now points directly after :

		switch (*name | 0x20) {
		case 'c':
			if (!wget_strncasecmp_ascii(name, "Content-Encoding", namelen)) {
				wget_http_parse_content_encoding(s, &resp->content_encoding);
			} else if (!wget_strncasecmp_ascii(name, "Content-Type", namelen)) {
				wget_http_parse_content_type(s, &resp->content_type, &resp->content_type_encoding);
			} else if (!wget_strncasecmp_ascii(name, "Content-Length", namelen)) {
				resp->content_length = (size_t)atoll(s);
				resp->content_length_valid = 1;
			} else if (!wget_strncasecmp_ascii(name, "Content-Disposition", namelen)) {
				wget_http_parse_content_disposition(s, &resp->content_filename);
			} else if (!wget_strncasecmp_ascii(name, "Connection", namelen)) {
				wget_http_parse_connection(s, &resp->keep_alive);
			}
			break;
		case 'l':
			if (!wget_strncasecmp_ascii(name, "Last-Modified", namelen)) {
				// Last-Modified: Thu, 07 Feb 2008 15:03:24 GMT
				resp->last_modified = wget_http_parse_full_date(s);
			} else if (resp->code / 100 == 3 && !wget_strncasecmp_ascii(name, "Location", namelen)) {
				xfree(resp->location);
				wget_http_parse_location(s, &resp->location);
			} else if (resp->code / 100 == 3 && !wget_strncasecmp_ascii(name, "Link", namelen)) {
				// debug_printf("s=%.31s\n",s);
				wget_http_link_t link;
				wget_http_parse_link(s, &link);
				// debug_printf("link->uri=%s\n",link.uri);
				if (!resp->links) {
					resp->links = wget_vector_create(8, 8, NULL);
					wget_vector_set_destructor(resp->links, (void(*)(void *))wget_http_free_link);
				}
				wget_vector_add(resp->links, &link, sizeof(link));
			}
			break;
		case 't':
			if (!wget_strncasecmp_ascii(name, "Transfer-Encoding", namelen)) {
				wget_http_parse_transfer_encoding(s, &resp->transfer_encoding);
			}
			break;
		case 's':
			if (!wget_strncasecmp_ascii(name, "Set-Cookie", namelen)) {
				// this is a parser. content validation must be done by higher level functions.
				wget_cookie_t cookie;
				wget_http_parse_setcookie(s, &cookie);

				if (cookie.name) {
					if (!resp->cookies) {
						resp->cookies = wget_vector_create(4, 4, NULL);
						wget_vector_set_destructor(resp->cookies, (void(*)(void *))wget_cookie_deinit);
					}
					wget_vector_add(resp->cookies, &cookie, sizeof(cookie));
				}
			}
			else if (!wget_strncasecmp_ascii(name, "Strict-Transport-Security", namelen)) {
				resp->hsts = 1;
				wget_http_parse_strict_transport_security(s, &resp->hsts_maxage, &resp->hsts_include_subdomains);
			}
			break;
		case 'w':
			if (!wget_strncasecmp_ascii(name, "WWW-Authenticate", namelen)) {
				wget_http_challenge_t challenge;
				wget_http_parse_challenge(s, &challenge);

				if (!resp->challenges) {
					resp->challenges = wget_vector_create(2, 2, NULL);
					wget_vector_set_destructor(resp->challenges, (void(*)(void *))wget_http_free_challenge);
				}
				wget_vector_add(resp->challenges, &challenge, sizeof(challenge));
			}
			break;
		case 'd':
			if (!wget_strncasecmp_ascii(name, "Digest", namelen)) {
				// http://tools.ietf.org/html/rfc3230
				wget_http_digest_t digest;
				wget_http_parse_digest(s, &digest);
				// debug_printf("%s: %s\n",digest.algorithm,digest.encoded_digest);
				if (!resp->digests) {
					resp->digests = wget_vector_create(4, 4, NULL);
					wget_vector_set_destructor(resp->digests, (void(*)(void *))wget_http_free_digest);
				}
				wget_vector_add(resp->digests, &digest, sizeof(digest));
			}
			break;
		case 'i':
			if (!wget_strncasecmp_ascii(name, "ICY-Metaint", namelen)) {
				resp->icy_metaint = atoi(s);
			}
			break;
		case 'e':
			if (!wget_strncasecmp_ascii(name, "ETag", namelen)) {
				wget_http_parse_etag(s, &resp->etag);
			}
			break;
		default:
			break;
		}
	}

/*
 * A workaround for broken server configurations
 * see http://mail-archives.apache.org/mod_mbox/httpd-dev/200207.mbox/<3D2D4E76.4010502@talex.com.pl>
 * 24.10.15: It turns out that some servers (stupidly) double-gzip the data and correctly have
 *              Content-Encoding: gzip
 *              Content-Type: application/x-gzip
 *            in the response.
 *  We leave the following code commented, just to remember that we might have a CLI option
 *  to turn it on - in case someone stumbles over these kind of broken servers.
 *
	if (resp->content_encoding == wget_content_encoding_gzip &&
		!wget_strcasecmp_ascii(resp->content_type, "application/x-gzip"))
	{
		debug_printf("Broken server configuration gzip workaround triggered\n");
		resp->content_encoding =  wget_content_encoding_identity;
	}
*/
	return resp;
}

int wget_http_free_param(wget_http_header_param_t *param)
{
	xfree(param->name);
	xfree(param->value);
	return 0;
}

void wget_http_free_link(wget_http_link_t *link)
{
	xfree(link->uri);
	xfree(link->type);
}

void wget_http_free_links(wget_vector_t **links)
{
	wget_vector_free(links);
}

void wget_http_free_digest(wget_http_digest_t *digest)
{
	xfree(digest->algorithm);
	xfree(digest->encoded_digest);
}

void wget_http_free_digests(wget_vector_t **digests)
{
	wget_vector_free(digests);
}

void wget_http_free_challenge(wget_http_challenge_t *challenge)
{
	xfree(challenge->auth_scheme);
	wget_stringmap_free(&challenge->params);
}

void wget_http_free_challenges(wget_vector_t **challenges)
{
	wget_vector_free(challenges);
}

void wget_http_free_cookies(wget_vector_t **cookies)
{
	wget_vector_free(cookies);
}

void wget_http_free_response(wget_http_response_t **resp)
{
	if (resp && *resp) {
		wget_http_free_links(&(*resp)->links);
		wget_http_free_digests(&(*resp)->digests);
		wget_http_free_challenges(&(*resp)->challenges);
		wget_http_free_cookies(&(*resp)->cookies);
		xfree((*resp)->content_type);
		xfree((*resp)->content_type_encoding);
		xfree((*resp)->content_filename);
		xfree((*resp)->location);
		xfree((*resp)->etag);
		// xfree((*resp)->reason);
		wget_buffer_free(&(*resp)->header);
		wget_buffer_free(&(*resp)->body);
		xfree(*resp);
	}
}

/* for security reasons: set all freed pointers to NULL */
void wget_http_free_request(wget_http_request_t **req)
{
	if (req && *req) {
		wget_buffer_deinit(&(*req)->esc_resource);
		wget_buffer_deinit(&(*req)->esc_host);
		wget_vector_free(&(*req)->headers);
		xfree(*req);
	}
}

wget_http_request_t *wget_http_create_request(const wget_iri_t *iri, const char *method)
{
	wget_http_request_t *req = xcalloc(1, sizeof(wget_http_request_t));

	wget_buffer_init(&req->esc_resource, req->esc_resource_buf, sizeof(req->esc_resource_buf));
	wget_buffer_init(&req->esc_host, req->esc_host_buf, sizeof(req->esc_host_buf));

	req->scheme = iri->scheme;
	strlcpy(req->method, method, sizeof(req->method));
	wget_iri_get_escaped_resource(iri, &req->esc_resource);
	wget_iri_get_escaped_host(iri, &req->esc_host);
	req->headers = wget_vector_create(8, 8, NULL);
	wget_vector_set_destructor(req->headers, (void(*)(void *))wget_http_free_param);

	return req;
}

void wget_http_add_header_vprintf(wget_http_request_t *req, const char *name, const char *fmt, va_list args)
{
	wget_http_header_param_t param;

	param.value = wget_str_vasprintf(fmt, args);
	param.name = strdup(name);
	wget_vector_add(req->headers, &param, sizeof(param));
}

void wget_http_add_header_printf(wget_http_request_t *req, const char *name, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	wget_http_add_header_vprintf(req, name, fmt, args);
	va_end(args);
}

void wget_http_add_header(wget_http_request_t *req, const char *name, const char *value)
{
	wget_http_header_param_t param = {
		.name = strdup(name),
		.value = strdup(value)
	};

	wget_vector_add(req->headers, &param, sizeof(param));
}

void wget_http_add_header_param(wget_http_request_t *req, wget_http_header_param_t *param)
{
	wget_http_header_param_t _param = {
		.name = strdup(param->name),
		.value = strdup(param->value)
	};

	wget_vector_add(req->headers, &_param, sizeof(_param));
}

void wget_http_add_credentials(wget_http_request_t *req, wget_http_challenge_t *challenge, const char *username, const char *password)
{
	if (!challenge)
		return;

	if (!username)
		username = "";

	if (!password)
		password = "";

	if (!wget_strcasecmp_ascii(challenge->auth_scheme, "basic")) {
		const char *encoded = wget_base64_encode_printf_alloc("%s:%s", username, password);
		wget_http_add_header_printf(req, "Authorization", "Basic %s", encoded);
		xfree(encoded);
	}
	else if (!wget_strcasecmp_ascii(challenge->auth_scheme, "digest")) {
		int md5size = wget_hash_get_len(WGET_DIGTYPE_MD5);
		char a1buf[md5size * 2 + 1], a2buf[md5size * 2 + 1];
		char response_digest[md5size * 2 + 1], cnonce[16] = "";
		wget_buffer_t buf;
		const char
			*realm = wget_stringmap_get(challenge->params, "realm"),
			*opaque = wget_stringmap_get(challenge->params, "opaque"),
			*nonce = wget_stringmap_get(challenge->params, "nonce"),
			*qop = wget_stringmap_get(challenge->params, "qop"),
			*algorithm = wget_stringmap_get(challenge->params, "algorithm");

		if (wget_strcmp(qop, "auth")) {
			error_printf(_("Unsupported quality of protection '%s'.\n"), qop);
			return;
		}

		if (wget_strcmp(algorithm, "MD5") && wget_strcmp(algorithm, "MD5-sess")) {
			error_printf(_("Unsupported algorithm '%s'.\n"), algorithm);
			return;
		}

		if (!realm || !nonce)
			return;

		// A1BUF = H(user ":" realm ":" password)
		wget_md5_printf_hex(a1buf, "%s:%s:%s", username, realm, password);

		if (!wget_strcmp(algorithm, "MD5-sess")) {
			// A1BUF = H( H(user ":" realm ":" password) ":" nonce ":" cnonce )
			snprintf(cnonce, sizeof(cnonce), "%08lx", wget_random()); // create random hex string
			wget_md5_printf_hex(a1buf, "%s:%s:%s", a1buf, nonce, cnonce);
		}

		// A2BUF = H(method ":" path)
		wget_md5_printf_hex(a2buf, "%s:/%s", req->method, req->esc_resource.data);

		if (!wget_strcmp(qop, "auth") || !wget_strcmp(qop, "auth-int")) {
			// RFC 2617 Digest Access Authentication
			if (!*cnonce)
				snprintf(cnonce, sizeof(cnonce), "%08lx", wget_random()); // create random hex string

			// RESPONSE_DIGEST = H(A1BUF ":" nonce ":" nc ":" cnonce ":" qop ": " A2BUF)
			wget_md5_printf_hex(response_digest, "%s:%s:00000001:%s:%s:%s", a1buf, nonce, /* nc, */ cnonce, qop, a2buf);
		} else {
			// RFC 2069 Digest Access Authentication

			// RESPONSE_DIGEST = H(A1BUF ":" nonce ":" A2BUF)
			wget_md5_printf_hex(response_digest, "%s:%s:%s", a1buf, nonce, a2buf);
		}

		wget_buffer_init(&buf, NULL, 256);

		wget_buffer_printf(&buf,
			"Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"/%s\", response=\"%s\"",
			username, realm, nonce, req->esc_resource.data, response_digest);

		if (!wget_strcmp(qop,"auth"))
			wget_buffer_printf_append(&buf, ", qop=auth, nc=00000001, cnonce=\"%s\"", cnonce);

		if (opaque)
			wget_buffer_printf_append(&buf, ", opaque=\"%s\"", opaque);

		if (algorithm)
			wget_buffer_printf_append(&buf, ", algorithm=%s", algorithm);

		wget_http_add_header(req, "Authorization", buf.data);

		wget_buffer_deinit(&buf);
	}
}

/*
static struct _config {
	int
		read_timeout;
	unsigned int
		dns_caching : 1;
} _config = {
	.read_timeout = -1,
	.dns_caching = 1
};

void http_set_config_int(int key, int value)
{
	switch (key) {
	case HTTP_READ_TIMEOUT: _config.read_timeout = value;
	case HTTP_DNS: _config.read_timeout = value;
	default: error_printf(_("Unknown config key %d (or value must not be an integer)\n"), key);
	}
}
*/

#ifdef WITH_LIBNGHTTP2
static ssize_t _send_callback(nghttp2_session *session G_GNUC_WGET_UNUSED,
	const uint8_t *data, size_t length, int flags G_GNUC_WGET_UNUSED, void *user_data)
{
	wget_http_connection_t *conn = (wget_http_connection_t *)user_data;
	int rc;

	// debug_printf("writing... %zd\n", length);
	if ((rc = wget_tcp_write(conn->tcp, (const char *)data, length)) <= 0) {
		// An error will be written by the wget_tcp_write function.
		// debug_printf("write rc %d, errno=%d\n", rc, errno);
		return rc ? NGHTTP2_ERR_CALLBACK_FAILURE : NGHTTP2_ERR_WOULDBLOCK;
	}
	// debug_printf("write rc %d\n",rc);

	return rc;
}

static ssize_t _recv_callback(nghttp2_session *session G_GNUC_WGET_UNUSED,
	uint8_t *buf, size_t length, int flags G_GNUC_WGET_UNUSED, void *user_data)
{
	wget_http_connection_t *conn = (wget_http_connection_t *)user_data;
	int rc;

	// debug_printf("reading... %zd\n", length);
	if ((rc = wget_tcp_read(conn->tcp, (char *)buf, length)) <= 0) {
		//  0 = timeout resp. blocking
		// -1 = failure
		// debug_printf("read rc %d, errno=%d\n", rc, errno);
		return rc ? NGHTTP2_ERR_CALLBACK_FAILURE : NGHTTP2_ERR_WOULDBLOCK;
	}
	// debug_printf("read rc %d\n",rc);

	return rc;
}

static void _print_frame_type(int type, const char tag)
{
	static const char *name[] = {
		[NGHTTP2_DATA] = "DATA",
		[NGHTTP2_HEADERS] = "HEADERS",
		[NGHTTP2_PRIORITY] = "PRIORITY",
		[NGHTTP2_RST_STREAM] = "RST_STREAM",
		[NGHTTP2_SETTINGS] = "SETTINGS",
		[NGHTTP2_PUSH_PROMISE] = "PUSH_PROMISE",
		[NGHTTP2_PING] = "PING",
		[NGHTTP2_GOAWAY] = "GOAWAY",
		[NGHTTP2_WINDOW_UPDATE] = "WINDOW_UPDATE",
		[NGHTTP2_CONTINUATION] = "CONTINUATION"
	};

	if ((unsigned) type < countof(name))
		debug_printf("[FRAME] %c %s\n", tag, name[type]);
	else
		debug_printf("[FRAME] %c Unknown type %d\n", tag, type);
}

static int _on_frame_send_callback(nghttp2_session *session G_GNUC_WGET_UNUSED,
	const nghttp2_frame *frame, void *user_data G_GNUC_WGET_UNUSED)
{
	_print_frame_type(frame->hd.type, '>');

	if (frame->hd.type == NGHTTP2_HEADERS) {
		const nghttp2_nv *nva = frame->headers.nva;

		for (unsigned i = 0; i < frame->headers.nvlen; i++)
			debug_printf("[FRAME] > %.*s: %.*s\n", (int)nva[i].namelen, nva[i].name, (int)nva[i].valuelen, nva[i].value);
	}

	return 0;
}

static int _on_frame_recv_callback(nghttp2_session *session G_GNUC_WGET_UNUSED,
	const nghttp2_frame *frame, void *user_data G_GNUC_WGET_UNUSED)
{
	_print_frame_type(frame->hd.type, '<');

	return 0;
}


// the following is just needed for the progress bar
struct _body_callback_context {
	wget_http_response_t *resp;
	void *context;
	int (*body_callback)(void *, const char *, size_t);
	char done;
};

static int _on_header_callback(nghttp2_session *session G_GNUC_WGET_UNUSED,
	const nghttp2_frame *frame, const uint8_t *name, size_t namelen,
	const uint8_t *value, size_t valuelen,
	uint8_t flags G_GNUC_WGET_UNUSED, void *user_data G_GNUC_WGET_UNUSED)
{
	wget_http_request_t *req = nghttp2_session_get_stream_user_data(session, frame->hd.stream_id);

	if (req) {
		if (frame->hd.type == NGHTTP2_HEADERS) {
			if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
				struct _body_callback_context *ctx = req->nghttp2_context;
				wget_http_response_t *resp = ctx->resp;
				const char *s = wget_strmemdup((char *)value, valuelen);

				debug_printf("%.*s: %s\n", (int)namelen, name, s);

				switch (namelen) {
				case 4:
					if (!memcmp(name, "etag", namelen)) {
						wget_http_parse_etag(s, &resp->etag);
					}
					else if (!memcmp(name, "link", namelen) && resp->code / 100 == 3) {
						// debug_printf("s=%.31s\n",s);
						wget_http_link_t link;
						wget_http_parse_link(s, &link);
						// debug_printf("link->uri=%s\n",link.uri);
						if (!resp->links) {
							resp->links = wget_vector_create(8, 8, NULL);
							wget_vector_set_destructor(resp->links, (void(*)(void *))wget_http_free_link);
						}
						wget_vector_add(resp->links, &link, sizeof(link));
					}
					break;
				case 6:
					if (!memcmp(name, "digest", namelen)) {
						// http://tools.ietf.org/html/rfc3230
						wget_http_digest_t digest;
						wget_http_parse_digest(s, &digest);
						// debug_printf("%s: %s\n",digest.algorithm,digest.encoded_digest);
						if (!resp->digests) {
							resp->digests = wget_vector_create(4, 4, NULL);
							wget_vector_set_destructor(resp->digests, (void(*)(void *))wget_http_free_digest);
						}
						wget_vector_add(resp->digests, &digest, sizeof(digest));
					}
					break;
				case 7:
					if (!memcmp(name, ":status", namelen) && valuelen == 3) {
						ctx->resp->code = ((value[0] - '0') * 10 + (value[1] - '0')) * 10 + (value[2] - '0');
					}
					break;
				case 8:
					if (resp->code / 100 == 3 && !memcmp(name, "location", namelen)) {
						xfree(resp->location);
						wget_http_parse_location(s, &resp->location);
					}
					break;
				case 10:
					if (!memcmp(name, "set-cookie", namelen)) {
						// this is a parser. content validation must be done by higher level functions.
						wget_cookie_t cookie;
						wget_http_parse_setcookie(s, &cookie);

						if (cookie.name) {
							if (!resp->cookies) {
								resp->cookies = wget_vector_create(4, 4, NULL);
								wget_vector_set_destructor(resp->cookies, (void(*)(void *))wget_cookie_deinit);
							}
							wget_vector_add(resp->cookies, &cookie, sizeof(cookie));
						}
					}
					else if (!memcmp(name, "connection", namelen)) {
						wget_http_parse_connection(s, &resp->keep_alive);
					}
					break;
				case 11:
					if (!memcmp(name, "icy-metaint", namelen)) {
						resp->icy_metaint = atoi(s);
					}
					break;
				case 12:
					if (!memcmp(name, "content-type", namelen)) {
						wget_http_parse_content_type(s, &resp->content_type, &resp->content_type_encoding);
					}
					break;
				case 13:
					if (!memcmp(name, "last-modified", namelen)) {
						// Last-Modified: Thu, 07 Feb 2008 15:03:24 GMT
						resp->last_modified = wget_http_parse_full_date(s);
					}
					break;
				case 14:
					if (!memcmp(name, "content-length", namelen)) {
						resp->content_length = (size_t)atoll(s);
						resp->content_length_valid = 1;
					}
					break;
				case 16:
					if (!memcmp(name, "content-encoding", namelen)) {
						wget_http_parse_content_encoding(s, &resp->content_encoding);
					}
					else if (!memcmp(name, "www-authenticate", namelen)) {
						wget_http_challenge_t challenge;
						wget_http_parse_challenge(s, &challenge);

						if (!resp->challenges) {
							resp->challenges = wget_vector_create(2, 2, NULL);
							wget_vector_set_destructor(resp->challenges, (void(*)(void *))wget_http_free_challenge);
						}
						wget_vector_add(resp->challenges, &challenge, sizeof(challenge));
					}
					break;
				case 17:
					if (!memcmp(name, "transfer-encoding", namelen)) {
						wget_http_parse_transfer_encoding(s, &resp->transfer_encoding);
					}
					break;
				case 19:
					if (!memcmp(name, "content-disposition", namelen)) {
						wget_http_parse_content_disposition(s, &resp->content_filename);
					}
					break;
				case 25:
					if (!memcmp(name, "strict-transport-security", namelen)) {
						resp->hsts = 1;
						wget_http_parse_strict_transport_security(s, &resp->hsts_maxage, &resp->hsts_include_subdomains);
					}
					break;
				}

				xfree(s);
			}
		}
	}

	return 0;
}

/*
 * This function is called to indicate that a stream is closed.
 */
static int _on_stream_close_callback(nghttp2_session *session, int32_t stream_id,
	uint32_t error_code G_GNUC_WGET_UNUSED, void *user_data G_GNUC_WGET_UNUSED)
{
	wget_http_request_t *req = nghttp2_session_get_stream_user_data(session, stream_id);

	debug_printf("closing stream %d\n", stream_id);
	if (req) {
		struct _body_callback_context *ctx = req->nghttp2_context;

		if (ctx)
			ctx->done = 1;
	}

	return 0;
}
/*
 * The implementation of nghttp2_on_data_chunk_recv_callback type. We
 * use this function to print the received response body.
 */
static int _on_data_chunk_recv_callback(nghttp2_session *session,
	uint8_t flags G_GNUC_WGET_UNUSED, int32_t stream_id,
	const uint8_t *data, size_t len,	void *user_data G_GNUC_WGET_UNUSED)
{
	wget_http_request_t *req = nghttp2_session_get_stream_user_data(session, stream_id);

	if (req) {
		struct _body_callback_context *ctx = req->nghttp2_context;
//		debug_printf("[INFO] C <---------------------------- S%d (DATA chunk - %zu bytes)\n", stream_id, len);
		debug_printf("nbytes %zd\n", len);
		if (ctx && ctx->body_callback)
			ctx->body_callback(ctx->context, (char *)data, (ssize_t)len);
		// debug_write((char *)data, len);
		// debug_printf("\n");
	}
	return 0;
}

static void setup_nghttp2_callbacks(nghttp2_session_callbacks *callbacks)
{
	nghttp2_session_callbacks_set_send_callback(callbacks, _send_callback);
	nghttp2_session_callbacks_set_recv_callback(callbacks, _recv_callback);
	nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, _on_frame_send_callback);
	nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, _on_frame_recv_callback);
	nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, _on_stream_close_callback);
	nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, _on_data_chunk_recv_callback);
	nghttp2_session_callbacks_set_on_header_callback(callbacks, _on_header_callback);
}
#endif

int wget_http_open(wget_http_connection_t **_conn, const wget_iri_t *iri)
{
	static int next_http_proxy = -1;
	static int next_https_proxy = -1;
	static wget_thread_mutex_t
		mutex = WGET_THREAD_MUTEX_INITIALIZER;

	wget_iri_t
		*proxy;
	wget_http_connection_t
		*conn;
	const char
		*port,
		*host;
	int
		rc,
		ssl = iri->scheme == WGET_IRI_SCHEME_HTTPS;

	if (!_conn)
		return WGET_E_INVALID;

	conn = *_conn = xcalloc(1, sizeof(wget_http_connection_t)); // convenience assignment

	if (iri->scheme == WGET_IRI_SCHEME_HTTP && http_proxies) {
		wget_thread_mutex_lock(&mutex);
		proxy = wget_vector_get(http_proxies, (++next_http_proxy) % wget_vector_size(http_proxies));
		wget_thread_mutex_unlock(&mutex);

		host = proxy->host;
		port = proxy->resolv_port;
	} else if (iri->scheme == WGET_IRI_SCHEME_HTTPS && https_proxies) {
		wget_thread_mutex_lock(&mutex);
		proxy = wget_vector_get(https_proxies, (++next_https_proxy) % wget_vector_size(https_proxies));
		wget_thread_mutex_unlock(&mutex);

		host = proxy->host;
		port = proxy->resolv_port;
	} else {
		host = iri->host;
		port = iri->resolv_port;
	}

	conn->tcp = wget_tcp_init();
	if (ssl) {
		wget_tcp_set_ssl(conn->tcp, 1); // switch SSL on
		wget_tcp_set_ssl_hostname(conn->tcp, host); // enable host name checking
	}

	if ((rc = wget_tcp_connect(conn->tcp, host, port)) == WGET_E_SUCCESS) {
		conn->esc_host = iri->host ? strdup(iri->host) : NULL;
		conn->port = iri->resolv_port;
		conn->scheme = iri->scheme;
		conn->buf = wget_buffer_alloc(102400); // reusable buffer, large enough for most requests and responses
#ifdef WITH_LIBNGHTTP2
		if ((conn->protocol = wget_tcp_get_protocol(conn->tcp)) == WGET_PROTOCOL_HTTP_2_0) {
			nghttp2_session_callbacks *callbacks;

			if (nghttp2_session_callbacks_new(&callbacks)) {
				error_printf(_("Failed to create HTTP2 callbacks\n"));
				wget_http_close(_conn);
				return WGET_E_INVALID;
			}

			setup_nghttp2_callbacks(callbacks);
			rc = nghttp2_session_client_new(&conn->http2_session, callbacks, conn);
			nghttp2_session_callbacks_del(callbacks);

			if (rc) {
				error_printf(_("Failed to create HTTP2 client session (%d)\n"), rc);
				wget_http_close(_conn);
				return WGET_E_INVALID;
			}

			if ((rc = nghttp2_submit_settings(conn->http2_session, NGHTTP2_FLAG_NONE, NULL, 0))) {
				error_printf(_("Failed to submit HTTP2 client settings (%d)\n"), rc);
				wget_http_close(_conn);
				return WGET_E_INVALID;
			}
		}
#endif
	} else {
		wget_http_close(_conn);
	}

	return rc;
}

void wget_http_close(wget_http_connection_t **conn)
{
	if (*conn) {
		debug_printf("closing connection\n");
#ifdef WITH_LIBNGHTTP2
		if ((*conn)->http2_session) {
			int rc = nghttp2_session_terminate_session((*conn)->http2_session, NGHTTP2_NO_ERROR);
			if (rc)
				error_printf(_("Failed to terminate HTTP2 session (%d)\n"), rc);
			nghttp2_session_del((*conn)->http2_session);
		}
#endif
		wget_tcp_deinit(&(*conn)->tcp);
//		if (!wget_tcp_get_dns_caching())
//			freeaddrinfo((*conn)->addrinfo);
		xfree((*conn)->esc_host);
		// xfree((*conn)->port);
		// xfree((*conn)->scheme);
		wget_buffer_free(&(*conn)->buf);
		xfree(*conn);
	}
}

#define INIT_NV(nv, NAME, VALUE) \
{ \
	(nv)->name = (uint8_t *) NAME; \
	(nv)->value = (uint8_t *) VALUE; \
	(nv)->namelen = sizeof(NAME) - 1; \
	(nv)->valuelen = sizeof(VALUE) - 1; \
	(nv)->flags = NGHTTP2_NV_FLAG_NONE; \
}

#define INIT_NV_CS(nv, NAME, VALUE) \
{ \
	(nv)->name = (uint8_t *) NAME; \
	(nv)->value = (uint8_t *) VALUE; \
	(nv)->namelen = strlen((char *)(nv)->name); \
	(nv)->valuelen = strlen(VALUE); \
	(nv)->flags = NGHTTP2_NV_FLAG_NONE; \
}

static int  G_GNUC_WGET_NONNULL((1,2)) _http_send_request(wget_http_connection_t *conn, wget_http_request_t *req, const void *body, size_t length)
{
	ssize_t nbytes;

#ifdef WITH_LIBNGHTTP2
	if (wget_tcp_get_protocol(conn->tcp) == WGET_PROTOCOL_HTTP_2_0) {
		int n = 4 + wget_vector_size(req->headers);
		nghttp2_nv nvs[n], *nvp;
		char resource[req->esc_resource.length + 2];

		resource[0] = '/';
		memcpy(resource + 1, req->esc_resource.data, req->esc_resource.length + 1);
		INIT_NV(&nvs[0], ":method", "GET")
		INIT_NV_CS(&nvs[1], ":path", resource)
		INIT_NV(&nvs[2], ":scheme", "https")
		INIT_NV_CS(&nvs[3], ":authority", req->esc_host.data)
		nvp = &nvs[4];

		for (int it = 0; it < wget_vector_size(req->headers); it++) {
			wget_http_header_param_t *param = wget_vector_get(req->headers, it);
			if (!wget_strcasecmp_ascii(param->name, "Connection"))
				continue;
			if (!wget_strcasecmp_ascii(param->name, "Accept-Encoding"))
				continue;

			INIT_NV_CS(nvp, param->name, param->value)
			nvp++;
		}

		// nghttp2 does strdup of name+value and lowercase conversion of 'name'
		req->stream_id = nghttp2_submit_request(conn->http2_session, NULL, nvs, nvp - nvs, NULL, req);

		if (req->stream_id < 0) {
			error_printf(_("Failed to submit HTTP2 request\n"));
			return -1;
		}

		debug_printf("HTTP2 stream id %d\n", req->stream_id);

		return 0;
	}
#endif

	if ((nbytes = wget_http_request_to_buffer(req, conn->buf)) < 0) {
		error_printf(_("Failed to create request buffer\n"));
		return -1;
	}

	if (body && length) {
		nbytes = wget_buffer_memcat(conn->buf, body, length);
	}

	if (wget_tcp_write(conn->tcp, conn->buf->data, nbytes) != nbytes) {
		// An error will be written by the wget_tcp_write function.
		// error_printf(_("Failed to send %zd bytes (%d)\n"), nbytes, errno);
		return -1;
	}

	debug_printf("# sent %zd bytes:\n%s", nbytes, conn->buf->data);

	return 0;
}

int wget_http_send_request(wget_http_connection_t *conn, wget_http_request_t *req)
{
	return _http_send_request(conn, req, NULL, 0);
}

int wget_http_send_request_with_body(wget_http_connection_t *conn, wget_http_request_t *req, const void *body, size_t length)
{
	return _http_send_request(conn, req, body, length);
}

ssize_t wget_http_request_to_buffer(wget_http_request_t *req, wget_buffer_t *buf)
{
	int use_proxy = 0;

//	buffer_sprintf(buf, "%s /%s HTTP/1.1\r\nHost: %s", req->method, req->esc_resource.data ? req->esc_resource.data : "",);

	wget_buffer_strcpy(buf, req->method);
	wget_buffer_memcat(buf, " ", 1);
	if (req->scheme == WGET_IRI_SCHEME_HTTP && wget_vector_size(http_proxies) > 0) {
		use_proxy = 1;
		wget_buffer_strcat(buf, req->scheme);
		wget_buffer_memcat(buf, "://", 3);
		wget_buffer_bufcat(buf, &req->esc_host);
	} else if (req->scheme == WGET_IRI_SCHEME_HTTPS && wget_vector_size(https_proxies) > 0) {
		use_proxy = 1;
		wget_buffer_strcat(buf, req->scheme);
		wget_buffer_memcat(buf, "://", 3);
		wget_buffer_bufcat(buf, &req->esc_host);
	}
	wget_buffer_memcat(buf, "/", 1);
	wget_buffer_bufcat(buf, &req->esc_resource);
	wget_buffer_memcat(buf, " HTTP/1.1\r\n", 11);
	wget_buffer_memcat(buf, "Host: ", 6);
	wget_buffer_bufcat(buf, &req->esc_host);
	wget_buffer_memcat(buf, "\r\n", 2);

	for (int it = 0; it < wget_vector_size(req->headers); it++) {
		wget_http_header_param_t *param = wget_vector_get(req->headers, it);

		wget_buffer_strcat(buf, param->name);
		wget_buffer_memcat(buf, ": ", 2);
		wget_buffer_strcat(buf, param->value);

		if (buf->data[buf->length - 1] != '\n') {
			wget_buffer_memcat(buf, "\r\n", 2);
		}
	}

	if (use_proxy)
		wget_buffer_strcat(buf, "Proxy-Connection: keep-alive\r\n");

	wget_buffer_memcat(buf, "\r\n", 2); // end-of-header

	return buf->length;
}

wget_http_response_t *wget_http_get_response_cb(
	wget_http_connection_t *conn,
	wget_http_request_t *req,
	unsigned int flags,
	int (*header_callback)(void *context, wget_http_response_t *resp),
	int (*body_callback)(void *context, const char *data, size_t length),
	void *context) // given to body_callback and header_callback
{
	size_t bufsize, body_len = 0, body_size = 0;
	ssize_t nbytes, nread = 0;
	char *buf, *p = NULL;
	wget_http_response_t *resp = NULL;
	wget_decompressor_t *dc = NULL;

#ifdef WITH_LIBNGHTTP2
	int ioflags;

	if (conn->protocol == WGET_PROTOCOL_HTTP_2_0) {
		resp = xcalloc(1, sizeof(wget_http_response_t));
		resp->major = 2;
		// we do not get a Keep-Alive header in HTTP2 - let's assume the connection stays open
		resp->keep_alive = 1;

		struct _body_callback_context ctx = { .resp = resp, .context = context, .body_callback = body_callback };
		req->nghttp2_context = &ctx;

		int timeout = wget_tcp_get_timeout(conn->tcp);

		for (int rc = 0; rc == 0 && !ctx.done && !conn->abort_indicator && !_abort_indicator;) {
			ioflags = 0;
			if (nghttp2_session_want_write(conn->http2_session))
				ioflags |= WGET_IO_WRITABLE;
			if (nghttp2_session_want_read(conn->http2_session))
				ioflags |= WGET_IO_READABLE;

			if (ioflags)
				ioflags = wget_tcp_ready_2_transfer(conn->tcp, ioflags);
			// debug_printf("ioflags=%d timeout=%d\n",ioflags,wget_tcp_get_timeout(conn->tcp));
			if (ioflags <= 0) break; // error or timeout

			wget_tcp_set_timeout(conn->tcp, 0); // 0 = immediate
			rc = 0;
			if (ioflags & WGET_IO_WRITABLE) {
				rc = nghttp2_session_send(conn->http2_session);
			}
			if (!rc && (ioflags & WGET_IO_READABLE))
				rc = nghttp2_session_recv(conn->http2_session);
			wget_tcp_set_timeout(conn->tcp, timeout); // restore old timeout

/*
			while (nghttp2_session_want_write(conn->http2_session)) {
				rc = nghttp2_session_send(conn->http2_session);
			}
			debug_printf("1 response status %d done %d\n", resp->code, ctx.done);
			if (nghttp2_session_want_read(conn->http2_session)) {
				rc = nghttp2_session_recv(conn->http2_session);
			}
*/
		}

		debug_printf("response status %d\n", resp->code);

		// a workaround for broken server configurations
		// see http://mail-archives.apache.org/mod_mbox/httpd-dev/200207.mbox/<3D2D4E76.4010502@talex.com.pl>
		if (resp->content_encoding == wget_content_encoding_gzip &&
			!wget_strcasecmp_ascii(resp->content_type, "application/x-gzip"))
		{
			debug_printf("Broken server configuration gzip workaround triggered\n");
			resp->content_encoding =  wget_content_encoding_identity;
		}

		return resp;
	}
#endif

	// reuse generic connection buffer
	buf = conn->buf->data;
	bufsize = conn->buf->size;

	while ((nbytes = wget_tcp_read(conn->tcp, buf + nread, bufsize - nread)) > 0) {
		debug_printf("nbytes %zd nread %zd %zd\n", nbytes, nread, bufsize);
		nread += nbytes;
		buf[nread] = 0; // 0-terminate to allow string functions

		if (nread < 4) continue;

		if (nread == nbytes)
			p = buf;
		else
			p = buf + nread - nbytes - 3;

		if ((p = strstr(p, "\r\n\r\n"))) {
			// found end-of-header
			*p = 0;

			debug_printf("# got header %zd bytes:\n%s\n\n", p - buf, buf);

			if (flags&WGET_HTTP_RESPONSE_KEEPHEADER) {
				wget_buffer_t *header = wget_buffer_alloc(p - buf + 4);
				wget_buffer_memcpy(header, buf, p - buf);
				wget_buffer_memcat(header, "\r\n\r\n", 4);

				if (!(resp = wget_http_parse_response_header(buf))) {
					wget_buffer_free(&header);
					goto cleanup; // something is wrong with the header
				}

				resp->header = header;

			} else {
				if (!(resp = wget_http_parse_response_header(buf)))
					goto cleanup; // something is wrong with the header
			}

			if (header_callback) {
				if (header_callback(context, resp))
					goto cleanup; // stop requested by callback function
			}

			if (req && !wget_strcasecmp_ascii(req->method, "HEAD"))
				goto cleanup; // a HEAD response won't have a body

			p += 4; // skip \r\n\r\n to point to body
			break;
		}

		if ((size_t)nread + 1024 > bufsize) {
			wget_buffer_ensure_capacity(conn->buf, bufsize + 1024);
			buf = conn->buf->data;
			bufsize = conn->buf->size;
		}
	}
	if (!nread) goto cleanup;

	if (!resp || resp->code / 100 == 1 || resp->code == 204 || resp->code == 304 ||
		(resp->transfer_encoding == transfer_encoding_identity && resp->content_length == 0 && resp->content_length_valid)) {
		// - body not included, see RFC 2616 4.3
		// - body empty, see RFC 2616 4.4
		goto cleanup;
	}

	dc = wget_decompress_open(resp->content_encoding, body_callback, context);

	// calculate number of body bytes so far read
	body_len = nread - (p - buf);
	// move already read body data to buf
	memmove(buf, p, body_len);
	buf[body_len] = 0;

	if (resp->transfer_encoding != transfer_encoding_identity) {
		size_t chunk_size = 0;
		char *end;

		debug_printf("method 1 %zd %zd:\n", body_len, body_size);
		// RFC 2616 3.6.1
		// Chunked-Body   = *chunk last-chunk trailer CRLF
		// chunk          = chunk-size [ chunk-extension ] CRLF chunk-data CRLF
		// chunk-size     = 1*HEX
		// last-chunk     = 1*("0") [ chunk-extension ] CRLF
		// chunk-extension= *( ";" chunk-ext-name [ "=" chunk-ext-val ] )
		// chunk-ext-name = token
		// chunk-ext-val  = token | quoted-string
		// chunk-data     = chunk-size(OCTET)
		// trailer        = *(entity-header CRLF)
		// entity-header  = extension-header = message-header
		// message-header = field-name ":" [ field-value ]
		// field-name     = token
		// field-value    = *( field-content | LWS )
		// field-content  = <the OCTETs making up the field-value
		//                  and consisting of either *TEXT or combinations
		//                  of token, separators, and quoted-string>

/*
			length := 0
			read chunk-size, chunk-extension (if any) and CRLF
			while (chunk-size > 0) {
				read chunk-data and CRLF
				append chunk-data to entity-body
				length := length + chunk-size
				read chunk-size and CRLF
			}
			read entity-header
			while (entity-header not empty) {
				append entity-header to existing header fields
				read entity-header
			}
			Content-Length := length
			Remove "chunked" from Transfer-Encoding
*/

		// read each chunk, stripping the chunk info
		p = buf;
		for (;;) {
			// debug_printf("#1 p='%.16s'\n",p);
			// read: chunk-size [ chunk-extension ] CRLF
			while ((!(end = strchr(p, '\r')) || end[1] != '\n')) {
				if (conn->abort_indicator || _abort_indicator)
					goto cleanup;

				if ((nbytes = wget_tcp_read(conn->tcp, buf + body_len, bufsize - body_len)) <= 0)
					goto cleanup;

				body_len += nbytes;
				buf[body_len] = 0;
				debug_printf("a nbytes %zd body_len %zd\n", nbytes, body_len);
			}
			end += 2;

			// now p points to chunk-size (hex)
			chunk_size = strtoll(p, NULL, 16);
			debug_printf("chunk size is %zd\n", chunk_size);
			if (chunk_size == 0) {
				// now read 'trailer CRLF' which is '*(entity-header CRLF) CRLF'
				if (*end == '\r' && end[1] == '\n') // shortcut for the most likely case (empty trailer)
					goto cleanup;

				debug_printf("reading trailer\n");
				while (!strstr(end, "\r\n\r\n")) {
					if (body_len > 3) {
						// just need to keep the last 3 bytes to avoid buffer resizing
						memmove(buf, buf + body_len - 3, 4); // plus 0 terminator, just in case
						body_len = 3;
					}

					if (conn->abort_indicator || _abort_indicator)
						goto cleanup;

					if ((nbytes = wget_tcp_read(conn->tcp, buf + body_len, bufsize - body_len)) <= 0)
						goto cleanup;

					body_len += nbytes;
					buf[body_len] = 0;
					end = buf;
					debug_printf("a nbytes %zd\n", nbytes);
				}
				debug_printf("end of trailer \n");
				goto cleanup;
			}

			p = end + chunk_size + 2;
			if (p <= buf + body_len) {
				debug_printf("1 skip chunk_size %zd\n", chunk_size);
				wget_decompress(dc, end, chunk_size);
				continue;
			}

			wget_decompress(dc, end, (buf + body_len) - end);

			chunk_size = p - (buf + body_len); // in fact needed bytes to have chunk_size+2 in buf

			debug_printf("need at least %zd more bytes\n", chunk_size);

			while (chunk_size > 0) {
				if (conn->abort_indicator || _abort_indicator)
					goto cleanup;

				if ((nbytes = wget_tcp_read(conn->tcp, buf, bufsize)) <= 0)
					goto cleanup;
				debug_printf("a nbytes=%zd chunk_size=%zd\n", nread, chunk_size);

				if (chunk_size <= (size_t)nbytes) {
					if (chunk_size == 1 || !strncmp(buf + chunk_size - 2, "\r\n", 2)) {
						debug_printf("chunk completed\n");
						// p=end+chunk_size+2;
					} else {
						error_printf(_("Expected end-of-chunk not found\n"));
						goto cleanup;
					}
					if (chunk_size > 2)
						wget_decompress(dc, buf, chunk_size - 2);
					body_len = nbytes - chunk_size;
					if (body_len)
						memmove(buf, buf + chunk_size, body_len);
					buf[body_len] = 0;
					p = buf;
					break;
				} else {
					chunk_size -= nbytes;
					if (chunk_size >= 2)
						wget_decompress(dc, buf, nbytes);
					else
						wget_decompress(dc, buf, nbytes - 1); // special case: we got a partial end-of-chunk
				}
			}
		}
	} else if (resp->content_length_valid) {
		// read content_length bytes
		debug_printf("method 2\n");

		if (body_len)
			wget_decompress(dc, buf, body_len);

		while (body_len < resp->content_length) {
			if (conn->abort_indicator || _abort_indicator)
				break;

			if (((nbytes = wget_tcp_read(conn->tcp, buf, bufsize)) <= 0))
				break;

			body_len += nbytes;
			debug_printf("nbytes %zd total %zd/%zd\n", nbytes, body_len, resp->content_length);
			wget_decompress(dc, buf, nbytes);
		}
		if (nbytes < 0)
			error_printf(_("Failed to read %zd bytes (%d)\n"), nbytes, errno);
		if (body_len < resp->content_length)
			error_printf(_("Just got %zu of %zu bytes\n"), body_len, body_size);
		else if (body_len > resp->content_length)
			error_printf(_("Body too large: %zu instead of %zu bytes\n"), body_len, resp->content_length);
		resp->content_length = body_len;
	} else {
		// read as long as we can
		debug_printf("method 3\n");

		if (body_len)
			wget_decompress(dc, buf, body_len);

		while (!conn->abort_indicator && !_abort_indicator && (nbytes = wget_tcp_read(conn->tcp, buf, bufsize)) > 0) {
			body_len += nbytes;
			debug_printf("nbytes %zd total %zd\n", nbytes, body_len);
			wget_decompress(dc, buf, nbytes);
		}
		resp->content_length = body_len;
	}

cleanup:
	wget_decompress_close(dc);

	return resp;
}

static int _get_body(void *userdata, const char *data, size_t length)
{
	wget_buffer_memcat((wget_buffer_t *)userdata, data, length);

	return 0;
}

// get response, resp->body points to body in memory

wget_http_response_t *wget_http_get_response(
	wget_http_connection_t *conn,
	int (*header_callback)(void *context, wget_http_response_t *resp),
	wget_http_request_t *req,
	unsigned int flags)
{
	wget_http_response_t *resp;
	wget_buffer_t *body = wget_buffer_alloc(102400);

	resp = wget_http_get_response_cb(conn, req, flags, header_callback, _get_body, body);

	if (resp) {
		resp->body = body;
		if (!wget_strcasecmp_ascii(req->method, "GET"))
			resp->content_length = body->length;
	} else {
		wget_buffer_free(&body);
	}

	return resp;
}

static int _get_fd(void *context, const char *data, size_t length)
{
	int fd = *(int *)context;
	ssize_t nbytes = write(fd, data, length);

	if (nbytes == -1 || (size_t)nbytes != length)
		error_printf(_("Failed to write %zu bytes of data (%d)\n"), length, errno);

	return 0;
}

wget_http_response_t *wget_http_get_response_fd(
	wget_http_connection_t *conn,
	int (*header_callback)(void *, wget_http_response_t *),
	int fd,
	unsigned int flags)
{
	 wget_http_response_t *resp = wget_http_get_response_cb(conn, NULL, flags, header_callback, _get_fd, &fd);

	return resp;
}

static int _get_stream(void *context, const char *data, size_t length)
{
	FILE *stream = (FILE *)context;
	size_t nbytes = fwrite(data, 1, length, stream);

	if (nbytes != length) {
		error_printf(_("Failed to write %zu bytes of data (%d)\n"), length, errno);

		if (feof(stream))
			return -1;
	}

	return 0;
}

wget_http_response_t *wget_http_get_response_stream(
	wget_http_connection_t *conn,
	int (*header_callback)(void *, wget_http_response_t *),
	FILE *stream, unsigned int flags)
{
	wget_http_response_t *resp = wget_http_get_response_cb(conn, NULL, flags, header_callback, _get_stream, stream);

	return resp;
}

wget_http_response_t *wget_http_get_response_func(
	wget_http_connection_t *conn,
	int (*header_callback)(void *, wget_http_response_t *),
	int (*body_callback)(void *, const char *, size_t),
	void *context, unsigned int flags)
{
	wget_http_response_t *resp = wget_http_get_response_cb(conn, NULL, flags, header_callback, body_callback, context);

	return resp;
}

/*
// get response, resp->body points to body in memory (nested func/trampoline version)
HTTP_RESPONSE *http_get_response(HTTP_CONNECTION *conn, HTTP_REQUEST *req)
{
	size_t bodylen=0, bodysize=102400;
	char *body=xmalloc(bodysize+1);

	int get_body(char *data, size_t length)
	{
		while (bodysize<bodylen+length)
			body=xrealloc(body,(bodysize*=2)+1);

		memcpy(body+bodylen,data,length);
		bodylen+=length;
		body[bodylen]=0;
		return 0;
	}

	HTTP_RESPONSE *resp=http_get_response_cb(conn,req,get_body);

	if (resp) {
		resp->body=body;
		resp->content_length=bodylen;
	} else {
		xfree(body);
	}

	return resp;
}

HTTP_RESPONSE *http_get_response_fd(HTTP_CONNECTION *conn, int fd)
{
	int get_file(char *data, size_t length) {
		if (write(fd,data,length)!=length)
			err_printf(_("Failed to write %zu bytes of data (%d)\n"),length,errno);
		return 0;
	}

	HTTP_RESPONSE *resp=http_get_response_cb(conn,NULL,get_file);

	return resp;
}
 */

/*
HTTP_RESPONSE *http_get_response_file(HTTP_CONNECTION *conn, const char *fname)
{
	size_t bodylen=0, bodysize=102400;
	char *body=xmalloc(bodysize+1);

	int get_file(char *data, size_t length) {
		if (write(fd,data,length)!=length)
			err_printf(_("Failed to write %zu bytes of data (%d)\n"),length,errno);
		return 0;
	}

	HTTP_RESPONSE *resp=http_get_response_cb(conn,NULL,get_file);

	return resp;
}
 */

static wget_vector_t *_parse_proxies(const char *proxy, const char *encoding)
{
	if (proxy) {
		wget_vector_t *proxies;
		const char *s, *p;

		proxies = wget_vector_create(8, -2, NULL);
		wget_vector_set_destructor(proxies, (void(*)(void *))wget_iri_free_content);

		for (s = proxy; (p = strchr(s, ',')); s = p + 1) {
			while (c_isspace(*s) && s < p) s++;

			if (p != s) {
				wget_iri_t *iri;
				char host[p - s + 1];

				memcpy(host, s, p -s);
				host[p - s] = 0;
				iri = wget_iri_parse (host, encoding);
				if (!iri) {
					wget_vector_free(&proxies);
					return NULL;
				}
				wget_vector_add_noalloc(proxies, iri);
			}
		}
		if (*s) {
			wget_iri_t *iri = wget_iri_parse(s, encoding);
			if (!iri) {
				wget_vector_free(&proxies);
				return NULL;
			}
			wget_vector_add_noalloc(proxies, iri);
		}

		return proxies;
	}

	return NULL;
}

int wget_http_set_http_proxy(const char *proxy, const char *encoding)
{
	if (http_proxies)
		wget_vector_free(&http_proxies);

	http_proxies = _parse_proxies(proxy, encoding);
	if (!http_proxies)
		return -1;

	return 0;
}

int wget_http_set_https_proxy(const char *proxy, const char *encoding)
{
	if (https_proxies)
		wget_vector_free(&https_proxies);

	https_proxies = _parse_proxies(proxy, encoding);
	if (!https_proxies)
		return -1;

	return 0;
}

void wget_http_abort_connection(wget_http_connection_t *conn)
{
	if (conn)
		conn->abort_indicator = 1; // stop single connection
	else
		_abort_indicator = 1; // stop all connections
}
