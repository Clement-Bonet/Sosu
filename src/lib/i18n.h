#ifndef SOSU_I18N_H
#define SOSU_I18N_H

#include "source.h"

/* The texts to translate of a package (spec §10.7), for `sosuc i18n extract`: the literal texts
   of its .ui.sosu files (labels, placeholders, the titles of windows, with their `plural:` and
   `context:`), and the literal arguments of `tr`, `tr_in`, `tr_plural` and `tr_plural_in` in its
   .sosu files. Written as the template of the catalogs, in the format of gettext (`messages.locale.default.sosu`). */

typedef struct I18nTemplate I18nTemplate;

I18nTemplate *i18n_template_new(void);
/* Adds the texts of `file`; `shown_path` is how the references name it (`src/screen.ui.sosu`). */
void i18n_template_add(I18nTemplate *t, const SourceFile *file, const char *shown_path);
/* The template: a header, then each text once, with its references, in the order found. */
char *i18n_template_text(const I18nTemplate *t);
size_t i18n_template_count(const I18nTemplate *t);
void i18n_template_free(I18nTemplate *t);

#endif
