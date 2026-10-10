/* fwiki.h -- Wikipedia: read the encyclopedia inside the app.
**
** A full-window view like the online views: today's featured content,
** search, articles as text in sections (contents, text size), the article's
** pictures, videos and sounds in the app's own viewers and players, the
** same article in other languages, related articles and its links, random
** articles, saved articles and history. All through Wikipedia's public APIs
** (the Action API api.php and the REST API), no key.
*/
#ifndef FWIKI_H
#define FWIKI_H

#include "fcore.h"
#include "fui.h"

bool wiki_is_open(void);
/* Opens the view (its home: today's featured content, or the last page). */
void wiki_open(void);
/* Opens straight at an article; lang "" = the view's language. */
void wiki_open_article(const char *lang, const char *title);
void wiki_close(void);
void wiki_frame(FmRect area);
void wiki_pump(void);                  /* every frame: finished requests */
void wiki_shutdown(void);
/* --demo-wiki-page: once the article is open, go to "media", "toc", "langs" or "related". */
void wiki_demo_page(const char *page);

/* Saved articles, newest first (the home screen's Favorites strip). */
int  wiki_saved_count(void);
bool wiki_saved_get(int i, char *lang, size_t lcap, char *title, size_t tcap, char *desc, size_t dcap, char *thumb,
                    size_t thcap);
bool wiki_saved_has(const char *lang, const char *title);

#endif
