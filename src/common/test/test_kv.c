#include "check.h"
#include <disc/kv.h>

#include <string.h>

int main(void)
{
	struct kvbuf b;
	char v[128];

	kv_init(&b);
	kv_raw(&b, "!np");
	kv_str(&b, "title", "Hello world 100%");
	kv_int(&b, "pos", -42);
	kv_str(&b, "empty", "");
	kv_str(&b, "nl", "a\nb");
	kv_end(&b);
	CHECK(!strcmp(b.s, "!np title=Hello%20world%20100%25 pos=-42 empty= nl=a%0Ab\n"));

	CHECK(kv_get(b.s, "title", v, sizeof(v)) && !strcmp(v, "Hello world 100%"));
	CHECK(kv_get_int(b.s, "pos", 0) == -42);
	CHECK(kv_get(b.s, "empty", v, sizeof(v)) && !strcmp(v, ""));
	CHECK(kv_get(b.s, "nl", v, sizeof(v)) && !strcmp(v, "a\nb"));
	CHECK(!kv_get(b.s, "tit", v, sizeof(v)));
	CHECK(kv_get_int(b.s, "missing", 7) == 7);
	CHECK(kv_word(b.s, 0, v, sizeof(v)) && !strcmp(v, "!np"));
	CHECK(kv_word("set ui.font large", 2, v, sizeof(v)) && !strcmp(v, "large"));
	CHECK(!kv_word("one", 1, v, sizeof(v)));
	kv_get("x=abcdefgh", "x", v, 4);
	CHECK(!strcmp(v, "abc"));
	kv_free(&b);
	CHECK_DONE();
}
