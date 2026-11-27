#include "check.h"
#include <disc/text.h>

#include <stdlib.h>
#include <string.h>

static int cmp(const void *a, const void *b)
{
	return nat_cmp(*(char *const *)a, *(char *const *)b);
}

int main(void)
{
	const char *s = "Яблоко";
	CHECK(utf8_next(&s) == 0x042f);
	CHECK(uc_lower(0x042f) == 0x044f);
	CHECK(uc_upper(0x044f) == 0x042f);
	CHECK(uc_upper(0x0451) == 0x0401);
	CHECK(uc_lower(0x0401) == 0x0451);
	CHECK(uc_upper(0xe9) == 0xc9);
	CHECK(uc_upper(0x0161) == 0x0160);
	CHECK(uc_lower(0x017d) == 0x017e);
	CHECK(uc_upper(0x017e) == 0x017d);
	CHECK(uc_upper(0x03c9) == 0x03a9);
	CHECK(uc_upper('z') == 'Z');

	CHECK(index_letter("abba") == 'A');
	CHECK(index_letter("(What's the story)") == 'W');
	CHECK(index_letter("1999") == '#');
	CHECK(index_letter("ёлка") == 0x0401);
	CHECK(index_letter("Ägypten") == 0xc4);
	CHECK(index_letter("") == '#');
	CHECK(index_letter("...") == '#');
	CHECK(index_letter("а") == 0x0410);

	CHECK(nat_cmp("2 - b", "10 - a") < 0);
	CHECK(nat_cmp("track02", "track2") != 0);
	CHECK(nat_cmp("abc", "ABC") != 0);
	CHECK(nat_cmp("abc", "abd") < 0);
	CHECK(nat_cmp("Abc", "abd") < 0);
	CHECK(nat_cmp("Яблоко", "яблоко2") < 0);
	CHECK(nat_cmp("a", "a") == 0);
	CHECK(nat_cmp("disc 1", "Disc 01") != 0);

	char *names[] = { "10 Ten.flac", "2 Two.flac", "1 One.flac", "b", "A", "Ä", "z" };
	qsort(names, 7, sizeof(char *), cmp);
	CHECK(!strcmp(names[0], "1 One.flac"));
	CHECK(!strcmp(names[1], "2 Two.flac"));
	CHECK(!strcmp(names[2], "10 Ten.flac"));
	CHECK(!strcmp(names[3], "A"));
	CHECK(!strcmp(names[4], "b"));

	char buf[8];
	int n = utf8_put(buf, 0x042f);
	buf[n] = 0;
	CHECK(!strcmp(buf, "Я"));
	CHECK(has_suffix_ci("Song.FLAC", ".flac"));
	CHECK(!has_suffix_ci("flac", ".flac"));

	char q[32];
	CHECK(filter_quote(q, sizeof(q), "AC/DC") && !strcmp(q, "\"AC/DC\""));
	CHECK(filter_quote(q, sizeof(q), "Say \"Hi\" it's \\") && !strcmp(q, "\"Say \\\"Hi\\\" it\\'s \\\\\""));
	CHECK(filter_quote(q, sizeof(q), "") && !strcmp(q, "\"\""));
	CHECK(!filter_quote(q, 8, "too long value"));
	CHECK(year_of("1999") == 1999);
	CHECK(year_of("2003-05-01") == 2003);
	CHECK(year_of("03/1999") == 0);
	CHECK(year_of("199") == 0);
	CHECK(year_of("") == 0);
	CHECK_DONE();
}
