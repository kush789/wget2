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
 * testing performance of hashmap/stringmap routines
 *
 * Changelog
 * 06.07.2012  Tim Ruehsen  created
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <string.h>
#ifdef HAVE_MMAP
#	include <sys/mman.h>
#endif
#include <errno.h>

#include <libwget.h>

static int G_GNUC_WGET_NONNULL_ALL _print_word(G_GNUC_WGET_UNUSED void *ctx, const char *word)
{
	printf("%s\n", word);
	return 0;
}

int main(int argc, const char *const *argv)
{
	int fd, it, unique = 0, duple = 0;
	char *buf, *word, *end;
	size_t length;
	struct stat st;
	wget_stringmap_t *map = wget_stringmap_create(1024);

	for (it = 1; it < argc; it++) {
		if ((fd = open(argv[it], O_RDONLY)) == -1) {
			fprintf(stderr, "Failed to read open %s\n", argv[it]);
			continue;
		}

		if (fstat(fd, &st)) {
			fprintf(stderr, "Failed to stat %s\n", argv[it]);
			close(fd);
			continue;
		}

		length = st.st_size;

#ifdef HAVE_MMAP
		if (!(buf = mmap(NULL, length + 1, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0))) {
			fprintf(stderr, "Failed to mmap %s (%d)\n", argv[it], errno);
			close(fd);
			continue;
		}
#else
		if (!(buf = xmalloc(length + 1)) || read(fd, buf, length) != (signed)length) {
			fprintf(stderr, "Failed to read %s (%d)\n", argv[it], errno);
			close(fd);
			continue;
		}
#endif

		buf[length] = 0;

		for (word = buf; *word; word = end) {
			while (*word && !isalnum(*word)) word++;
			for (end = word; *end && isalnum(*end);) end++;
			if (word != end) {
				char c = *end;
				*end = 0;

/*				if (stringmap_get(map, word)) {
					duple++;
				} else {
					stringmap_put_ident_noalloc(map, wget_strmemdup(word, end - word));
					unique++;
				}
*/
				if (wget_stringmap_put(map, word, NULL, 0))
					duple++;
				else
					unique++;

				*end = c;
			}
		}

#ifdef HAVE_MMAP
		munmap(buf, length);
#else
		free(buf);
#endif
		close(fd);
	}

	printf("read %u words, %u uniques, %u doubles\n", unique + duple, unique, duple);

	// const void *keys = stringmap_get_keys(map);
	wget_stringmap_browse(map, (int(*)(void *, const char *, void *))_print_word, NULL);

	wget_stringmap_free(&map);
	
	return 0;
}
