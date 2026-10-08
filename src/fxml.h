/* fxml.h -- a small, bounded, tolerant XML reader (pull tokens over a buffer).
**
** Made for podcast RSS feeds: walk start tags, end tags and text in order,
** read attributes and decoded text on demand. Nothing is allocated; tokens
** point into the caller's buffer, decoding writes into caller buffers.
**
**   FmXml x;
**   xml_init(&x, text, len);
**   while (xml_next(&x) != XML_EOF)
**     if (x.type == XML_START && xml_is(&x, "enclosure"))
**       xml_attr(&x, "url", url, sizeof url);
**
** Safe by design: DOCTYPE declarations (and their internal subset) are
** skipped, so there are no user-defined or external entities to expand;
** only the five predefined entities and numeric character references are
** decoded. Every scan is bounded by `len` (no NUL needed), the depth counter
** is clamped, and truncated or garbage input ends in XML_EOF, never a crash.
*/
#ifndef FXML_H
#define FXML_H

#include "fcore.h"

enum { XML_EOF, XML_START, XML_END, XML_TEXT };

#define XML_MAX_DEPTH 256

typedef struct FmXml {
  const char *s;
  size_t len, pos;
  int depth;               /* elements open around the current token */
  bool latin1;             /* <?xml encoding="ISO-8859-1"?> / windows-1252: text is converted to UTF-8 */
  /* the current token */
  int type;                /* XML_* */
  const char *name;        /* START/END: qualified name ("itunes:duration"), not NUL-terminated */
  size_t name_len;
  const char *attrs;       /* START: the raw attribute text */
  size_t attrs_len;
  bool empty;              /* START: <x/> (no END token follows) */
  const char *text;        /* TEXT: raw text (entities not decoded) or CDATA content */
  size_t text_len;
  bool cdata;              /* TEXT: from <![CDATA[...]]> (taken literally) */
} FmXml;

void xml_init(FmXml *x, const char *s, size_t len);
/* The next token (comments, processing instructions and DOCTYPE skipped). */
int  xml_next(FmXml *x);
/* START/END name test: "itunes:image" matches exactly; a name without a
** prefix ("title") matches only unprefixed elements. */
bool xml_is(const FmXml *x, const char *qname);
/* START: true when the prefix of the name is `prefix` and the local part is `local`. */
bool xml_is_ns(const FmXml *x, const char *prefix, const char *local);
/* START: the decoded value of an attribute; false (out = "") when missing. */
bool xml_attr(const FmXml *x, const char *name, char *out, size_t cap);
/* START: the prefix an xmlns:prefix="uri" attribute binds to uri ("" when
** none). Feeds nearly always say "itunes", but the prefix is free. */
bool xml_ns_prefix(const FmXml *x, const char *uri, char *out, size_t cap);
/* TEXT: the decoded text appended to out (kept NUL-terminated, cut on a
** UTF-8 boundary when full); returns the new length. */
size_t xml_text_append(const FmXml *x, char *out, size_t cap);
/* START: collects all text inside the element (children's text included,
** tags dropped) into out, trims blanks, and moves past its END. */
void xml_inner_text(FmXml *x, char *out, size_t cap);
/* START: moves past the element's END (whole subtree skipped). */
void xml_skip(FmXml *x);
/* Decodes &amp; &lt; &gt; &quot; &apos; &#N; &#xH; from s[0..n) into out
** (unknown entities are kept as written). Returns the bytes written. */
size_t xml_decode(const char *s, size_t n, bool latin1, char *out, size_t cap);

#endif
