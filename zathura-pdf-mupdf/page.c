/* SPDX-License-Identifier: Zlib */

#include <math.h>
#include <mupdf/pdf.h>
#include "plugin.h"

zathura_error_t pdf_page_init(zathura_page_t* page) {
  if (page == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  zathura_document_t* document     = zathura_page_get_document(page);
  mupdf_document_t* mupdf_document = zathura_document_get_data(document);
  mupdf_page_t* mupdf_page         = calloc(1, sizeof(mupdf_page_t));
  unsigned int index               = zathura_page_get_index(page);

  if (mupdf_page == NULL) {
    return ZATHURA_ERROR_OUT_OF_MEMORY;
  }

  g_mutex_lock(&mupdf_document->mutex);
  mupdf_page->ctx = mupdf_document->ctx;
  if (mupdf_page->ctx == NULL) {
    goto error_free;
  }

  /* load page */
  fz_try(mupdf_page->ctx) {
    mupdf_page->page = fz_load_page(mupdf_document->ctx, mupdf_document->document, index);
  }
  fz_catch(mupdf_page->ctx) {
    goto error_free;
  }

  mupdf_page->bbox = fz_bound_page(mupdf_document->ctx, (fz_page*)mupdf_page->page);

  /* setup text */
  mupdf_page->extracted_text = false;

  mupdf_page->text = fz_new_stext_page(mupdf_page->ctx, mupdf_page->bbox);
  if (mupdf_page->text == NULL) {
    goto error_free;
  }
  g_mutex_unlock(&mupdf_document->mutex);

  zathura_page_set_data(page, mupdf_page);

  /* get page dimensions */
  zathura_page_set_width(page, mupdf_page->bbox.x1 - mupdf_page->bbox.x0);
  zathura_page_set_height(page, mupdf_page->bbox.y1 - mupdf_page->bbox.y0);

  return ZATHURA_ERROR_OK;

error_free:
  g_mutex_unlock(&mupdf_document->mutex);

  pdf_page_clear(page, mupdf_page);

  return ZATHURA_ERROR_UNKNOWN;
}

zathura_error_t pdf_page_clear(zathura_page_t* page, void* data) {
  if (page == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  mupdf_page_t* mupdf_page         = data;
  zathura_document_t* document     = zathura_page_get_document(page);
  mupdf_document_t* mupdf_document = zathura_document_get_data(document);

  g_mutex_lock(&mupdf_document->mutex);
  if (mupdf_page != NULL) {
    if (mupdf_page->text != NULL) {
      fz_drop_stext_page(mupdf_page->ctx, mupdf_page->text);
    }

    if (mupdf_page->page != NULL) {
      fz_drop_page(mupdf_document->ctx, mupdf_page->page);
    }

    free(mupdf_page);
  }
  g_mutex_unlock(&mupdf_document->mutex);

  return ZATHURA_ERROR_UNKNOWN;
}

zathura_error_t pdf_page_get_label(zathura_page_t* page, void* data, char** label) {
  if (page == NULL || data == NULL || label == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  mupdf_page_t* mupdf_page     = data;
  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }
  mupdf_document_t* mupdf_document = zathura_document_get_data(document);

  char buf[16];

  g_mutex_lock(&mupdf_document->mutex);
  fz_try(mupdf_page->ctx) {
    fz_page_label(mupdf_page->ctx, mupdf_page->page, buf, sizeof(buf));
  }
  fz_catch(mupdf_page->ctx) {
    g_mutex_unlock(&mupdf_document->mutex);
    return ZATHURA_ERROR_UNKNOWN;
  }
  g_mutex_unlock(&mupdf_document->mutex);

  // fz_page_label() may return an empty string if the label is undefined.
  if (buf[0] != '\0') {
    *label = g_strdup(buf);
  } else {
    *label = NULL;
  }

  return ZATHURA_ERROR_OK;
}

girara_list_t* pdf_page_get_notes(zathura_page_t* page, void* data, zathura_error_t* error) {
  mupdf_page_t* mupdf_page = data;
  if (mupdf_page == NULL || mupdf_page->page == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    return NULL;
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    return NULL;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);

  girara_list_t* notes = girara_list_new2((girara_free_function_t)zathura_note_free);
  if (notes == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_OUT_OF_MEMORY;
    }
    return NULL;
  }

  fz_context* ctx = mupdf_page->ctx;

  g_mutex_lock(&mupdf_document->mutex);

  /* Get PDF-specific page - may fail for non-PDF documents */
  pdf_page* ppage = pdf_page_from_fz_page(ctx, mupdf_page->page);
  if (ppage == NULL) {
    /* Not a PDF page, return empty list */
    g_mutex_unlock(&mupdf_document->mutex);
    if (error != NULL) {
      *error = ZATHURA_ERROR_OK;
    }
    return notes;
  }

  fz_try(ctx) {
    for (pdf_annot* annot = pdf_first_annot(ctx, ppage); annot != NULL; annot = pdf_next_annot(ctx, annot)) {
      if (pdf_annot_type(ctx, annot) == PDF_ANNOT_TEXT) {
        fz_rect rect = pdf_annot_rect(ctx, annot);

        zathura_note_t* note = g_try_malloc0(sizeof(zathura_note_t));
        if (note == NULL) {
          continue;
        }

        note->page = zathura_page_get_index(page);
        note->x = rect.x0;
        note->y = rect.y0;
        note->id = g_strdup_printf("embedded-%u-%.0f-%.0f", note->page, note->x, note->y);

        const char* contents = pdf_annot_contents(ctx, annot);
        note->content = contents ? g_strdup(contents) : NULL;

        girara_list_append(notes, note);
        g_debug("pdf_page_get_notes: Found TEXT annotation (sticky note) on page %u at (%.0f, %.0f)",
                note->page, note->x, note->y);
      }
    }
  } fz_catch(ctx) {
    g_warning("pdf_page_get_notes: MuPDF exception while reading notes: %s", fz_caught_message(ctx));
  }

  g_mutex_unlock(&mupdf_document->mutex);

  g_message("pdf_page_get_notes: Found %zu notes on page %u",
            girara_list_size(notes), zathura_page_get_index(page));

  if (error != NULL) {
    *error = ZATHURA_ERROR_OK;
  }
  return notes;
}

zathura_error_t pdf_page_delete_note(zathura_page_t* page, void* data, double x, double y) {
  mupdf_page_t* mupdf_page = data;
  if (mupdf_page == NULL || mupdf_page->page == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);
  fz_context* ctx = mupdf_page->ctx;

  g_mutex_lock(&mupdf_document->mutex);

  /* Get PDF-specific page - may fail for non-PDF documents */
  pdf_page* ppage = pdf_page_from_fz_page(ctx, mupdf_page->page);
  if (ppage == NULL) {
    g_mutex_unlock(&mupdf_document->mutex);
    return ZATHURA_ERROR_UNKNOWN;
  }

  zathura_error_t result = ZATHURA_ERROR_UNKNOWN;
  const double eps = 1.0;  /* Coordinate tolerance for matching */

  fz_try(ctx) {
    for (pdf_annot* annot = pdf_first_annot(ctx, ppage); annot != NULL; annot = pdf_next_annot(ctx, annot)) {
      if (pdf_annot_type(ctx, annot) == PDF_ANNOT_TEXT) {
        fz_rect rect = pdf_annot_rect(ctx, annot);

        /* Match by position */
        if (fabs(rect.x0 - x) < eps && fabs(rect.y0 - y) < eps) {
          g_message("pdf_page_delete_note: Found annotation at (%.1f, %.1f), deleting", rect.x0, rect.y0);
          pdf_delete_annot(ctx, ppage, annot);
          result = ZATHURA_ERROR_OK;
          break;
        }
      }
    }
  } fz_catch(ctx) {
    g_warning("pdf_page_delete_note: MuPDF exception during deletion");
    result = ZATHURA_ERROR_UNKNOWN;
  }

  g_mutex_unlock(&mupdf_document->mutex);
  return result;
}

zathura_error_t pdf_page_update_note_content(zathura_page_t* page, void* data, double x, double y, const char* content) {
  mupdf_page_t* mupdf_page = data;
  if (mupdf_page == NULL || mupdf_page->page == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  if (content == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);
  fz_context* ctx = mupdf_page->ctx;

  g_debug("pdf_page_update_note_content: called with coords (%.2f, %.2f), content='%.30s...'", x, y, content);

  g_mutex_lock(&mupdf_document->mutex);

  /* Get PDF-specific page - may fail for non-PDF documents */
  pdf_page* ppage = pdf_page_from_fz_page(ctx, mupdf_page->page);
  if (ppage == NULL) {
    g_mutex_unlock(&mupdf_document->mutex);
    g_debug("pdf_page_update_note_content: ppage is NULL (not a PDF?)");
    return ZATHURA_ERROR_UNKNOWN;
  }

  zathura_error_t result = ZATHURA_ERROR_UNKNOWN;
  const double eps = 1.0;  /* Coordinate tolerance for matching */

  /* Count TEXT annotations BEFORE the update */
  int text_annot_count_before = 0;
  fz_try(ctx) {
    for (pdf_annot* annot = pdf_first_annot(ctx, ppage); annot != NULL; annot = pdf_next_annot(ctx, annot)) {
      if (pdf_annot_type(ctx, annot) == PDF_ANNOT_TEXT) {
        text_annot_count_before++;
        fz_rect rect = pdf_annot_rect(ctx, annot);
        g_debug("pdf_page_update_note_content: BEFORE - TEXT annot #%d at (%.2f, %.2f)",
                text_annot_count_before, rect.x0, rect.y0);
      }
    }
  } fz_catch(ctx) {
    g_warning("pdf_page_update_note_content: exception counting annotations before");
  }
  g_debug("pdf_page_update_note_content: BEFORE update - %d TEXT annotations exist", text_annot_count_before);

  fz_try(ctx) {
    for (pdf_annot* annot = pdf_first_annot(ctx, ppage); annot != NULL; annot = pdf_next_annot(ctx, annot)) {
      if (pdf_annot_type(ctx, annot) == PDF_ANNOT_TEXT) {
        fz_rect rect = pdf_annot_rect(ctx, annot);

        /* Match by position */
        if (fabs(rect.x0 - x) < eps && fabs(rect.y0 - y) < eps) {
          g_debug("pdf_page_update_note_content: MATCH FOUND at (%.2f, %.2f) - rect is (%.2f, %.2f, %.2f, %.2f)",
                  rect.x0, rect.y0, rect.x0, rect.y0, rect.x1, rect.y1);
          g_message("pdf_page_update_note_content: Found annotation at (%.1f, %.1f), updating content", rect.x0, rect.y0);
          pdf_set_annot_contents(ctx, annot, content);
          g_debug("pdf_page_update_note_content: called pdf_set_annot_contents()");
          pdf_update_annot(ctx, annot);
          g_debug("pdf_page_update_note_content: called pdf_update_annot()");
          result = ZATHURA_ERROR_OK;
          break;
        } else {
          g_debug("pdf_page_update_note_content: no match - annot at (%.2f, %.2f), looking for (%.2f, %.2f), diff=(%.2f, %.2f)",
                  rect.x0, rect.y0, x, y, fabs(rect.x0 - x), fabs(rect.y0 - y));
        }
      }
    }
  } fz_catch(ctx) {
    g_warning("pdf_page_update_note_content: MuPDF exception during update");
    result = ZATHURA_ERROR_UNKNOWN;
  }

  /* Count TEXT annotations AFTER the update */
  int text_annot_count_after = 0;
  fz_try(ctx) {
    for (pdf_annot* annot = pdf_first_annot(ctx, ppage); annot != NULL; annot = pdf_next_annot(ctx, annot)) {
      if (pdf_annot_type(ctx, annot) == PDF_ANNOT_TEXT) {
        text_annot_count_after++;
        fz_rect rect = pdf_annot_rect(ctx, annot);
        g_debug("pdf_page_update_note_content: AFTER - TEXT annot #%d at (%.2f, %.2f)",
                text_annot_count_after, rect.x0, rect.y0);
      }
    }
  } fz_catch(ctx) {
    g_warning("pdf_page_update_note_content: exception counting annotations after");
  }
  g_debug("pdf_page_update_note_content: AFTER update - %d TEXT annotations exist (was %d)",
          text_annot_count_after, text_annot_count_before);

  if (text_annot_count_after != text_annot_count_before) {
    g_warning("pdf_page_update_note_content: ANNOTATION COUNT CHANGED! before=%d, after=%d",
              text_annot_count_before, text_annot_count_after);
  }

  if (result != ZATHURA_ERROR_OK) {
    g_debug("pdf_page_update_note_content: NO MATCHING annotation found at (%.2f, %.2f)", x, y);
  }

  g_mutex_unlock(&mupdf_document->mutex);
  return result;
}

/**
 * Map PDF annotation color to zathura highlight color.
 * PDF colors are 0-1 floats, we match against known highlight colors.
 */
static zathura_highlight_color_t map_pdf_color_to_highlight(float r, float g, float b) {
  /* Yellow: (1.0, 1.0, 0.0) or similar */
  if (r > 0.8 && g > 0.8 && b < 0.3) {
    return ZATHURA_HIGHLIGHT_YELLOW;
  }
  /* Green: (0.0, 1.0, 0.0) or similar */
  if (r < 0.3 && g > 0.7 && b < 0.3) {
    return ZATHURA_HIGHLIGHT_GREEN;
  }
  /* Blue: (0.0, 0.0, 1.0) or similar light blue/cyan */
  if (r < 0.5 && g < 0.8 && b > 0.6) {
    return ZATHURA_HIGHLIGHT_BLUE;
  }
  /* Red: (1.0, 0.0, 0.0) or similar */
  if (r > 0.7 && g < 0.3 && b < 0.3) {
    return ZATHURA_HIGHLIGHT_RED;
  }
  /* Default to yellow for unknown colors */
  return ZATHURA_HIGHLIGHT_YELLOW;
}

girara_list_t* pdf_page_get_annotations(zathura_page_t* page, void* data, zathura_error_t* error) {
  mupdf_page_t* mupdf_page = data;
  if (mupdf_page == NULL || mupdf_page->page == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    return NULL;
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    return NULL;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);

  girara_list_t* highlights = girara_list_new2((girara_free_function_t)zathura_highlight_free);
  if (highlights == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_OUT_OF_MEMORY;
    }
    return NULL;
  }

  fz_context* ctx = mupdf_page->ctx;
  unsigned int page_index = zathura_page_get_index(page);

  g_mutex_lock(&mupdf_document->mutex);

  /* Get PDF-specific page - may fail for non-PDF documents */
  pdf_page* ppage = pdf_page_from_fz_page(ctx, mupdf_page->page);
  if (ppage == NULL) {
    /* Not a PDF page, return empty list */
    g_mutex_unlock(&mupdf_document->mutex);
    if (error != NULL) {
      *error = ZATHURA_ERROR_OK;
    }
    return highlights;
  }

  fz_try(ctx) {
    for (pdf_annot* annot = pdf_first_annot(ctx, ppage); annot != NULL; annot = pdf_next_annot(ctx, annot)) {
      enum pdf_annot_type type = pdf_annot_type(ctx, annot);

      /* Only process highlight, underline, and strikeout annotations */
      if (type != PDF_ANNOT_HIGHLIGHT && type != PDF_ANNOT_UNDERLINE && type != PDF_ANNOT_STRIKE_OUT) {
        continue;
      }

      /* Get number of quad points */
      int n_quads = pdf_annot_quad_point_count(ctx, annot);
      if (n_quads <= 0) {
        g_debug("pdf_page_get_annotations: annotation has no quad points, skipping");
        continue;
      }

      /* Create rectangle list from quad points */
      girara_list_t* rects = girara_list_new2(g_free);
      if (rects == NULL) {
        continue;
      }

      for (int i = 0; i < n_quads; i++) {
        fz_quad quad = pdf_annot_quad_point(ctx, annot, i);

        /* Convert quad to rectangle: use bounding box of the quad
         * Quad points are: ul, ur, ll, lr (upper-left, upper-right, lower-left, lower-right)
         */
        zathura_rectangle_t* rect = g_try_malloc(sizeof(zathura_rectangle_t));
        if (rect == NULL) {
          continue;
        }

        /* Calculate bounding rectangle from quad points */
        double min_x = fmin(fmin(quad.ul.x, quad.ur.x), fmin(quad.ll.x, quad.lr.x));
        double max_x = fmax(fmax(quad.ul.x, quad.ur.x), fmax(quad.ll.x, quad.lr.x));
        double min_y = fmin(fmin(quad.ul.y, quad.ur.y), fmin(quad.ll.y, quad.lr.y));
        double max_y = fmax(fmax(quad.ul.y, quad.ur.y), fmax(quad.ll.y, quad.lr.y));

        rect->x1 = min_x;
        rect->y1 = min_y;
        rect->x2 = max_x;
        rect->y2 = max_y;

        girara_list_append(rects, rect);
      }

      if (girara_list_size(rects) == 0) {
        girara_list_free(rects);
        continue;
      }

      /* Get annotation color */
      int n_colors = 0;
      float color_values[4] = {0};
      pdf_annot_color(ctx, annot, &n_colors, color_values);

      zathura_highlight_color_t hl_color = ZATHURA_HIGHLIGHT_YELLOW;
      if (n_colors >= 3) {
        hl_color = map_pdf_color_to_highlight(color_values[0], color_values[1], color_values[2]);
      }

      /* Try to get the text content from the annotation's contents or subject */
      const char* contents = pdf_annot_contents(ctx, annot);
      char* text = contents && contents[0] ? g_strdup(contents) : NULL;

      /* Create the highlight */
      zathura_highlight_t* highlight = zathura_highlight_new(page_index, rects, hl_color, text);
      if (highlight != NULL) {
        /* Generate a unique ID for embedded annotations.
         * Use pdf_bound_annot() instead of pdf_annot_rect() because MuPDF 1.27+
         * throws an exception for pdf_annot_rect() on markup annotations
         * (Highlight, Underline, StrikeOut) since they use QuadPoints not Rect. */
        fz_rect annot_rect = pdf_bound_annot(ctx, annot);
        g_free(highlight->id);
        highlight->id = g_strdup_printf("embedded-%u-%.0f-%.0f", page_index, annot_rect.x0, annot_rect.y0);

        girara_list_append(highlights, highlight);
        g_debug("pdf_page_get_annotations: Found %s annotation on page %u with %d quads, color=%d",
                type == PDF_ANNOT_HIGHLIGHT ? "HIGHLIGHT" :
                type == PDF_ANNOT_UNDERLINE ? "UNDERLINE" : "STRIKE_OUT",
                page_index, n_quads, hl_color);
      } else {
        girara_list_free(rects);
      }

      g_free(text);
    }
  } fz_catch(ctx) {
    g_warning("pdf_page_get_annotations: MuPDF exception while reading annotations: %s", fz_caught_message(ctx));
  }

  g_mutex_unlock(&mupdf_document->mutex);

  if (error != NULL) {
    *error = ZATHURA_ERROR_OK;
  }

  g_message("pdf_page_get_annotations: Found %zu annotations on page %u",
            girara_list_size(highlights), page_index);

  return highlights;
}

zathura_error_t pdf_page_export_notes(zathura_page_t* page, void* data, girara_list_t* notes) {
  mupdf_page_t* mupdf_page = data;
  if (mupdf_page == NULL || mupdf_page->page == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  if (notes == NULL || girara_list_size(notes) == 0) {
    return ZATHURA_ERROR_OK;  /* Nothing to export */
  }

  zathura_document_t* document = zathura_page_get_document(page);
  if (document == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);
  fz_context* ctx = mupdf_page->ctx;

  g_mutex_lock(&mupdf_document->mutex);

  /* Get PDF-specific page - may fail for non-PDF documents */
  pdf_page* ppage = pdf_page_from_fz_page(ctx, mupdf_page->page);
  if (ppage == NULL) {
    g_mutex_unlock(&mupdf_document->mutex);
    g_warning("pdf_page_export_notes: Not a PDF page");
    return ZATHURA_ERROR_UNKNOWN;
  }

  zathura_error_t result = ZATHURA_ERROR_OK;
  unsigned int exported_count = 0;

  fz_try(ctx) {
    GIRARA_LIST_FOREACH_BODY(notes, zathura_note_t*, note,
      /* Create a PDF_ANNOT_TEXT annotation (sticky note) */
      pdf_annot* annot = pdf_create_annot(ctx, ppage, PDF_ANNOT_TEXT);
      if (annot == NULL) {
        g_warning("pdf_page_export_notes: Failed to create annotation");
        continue;
      }

      /* Set annotation position - create a small rect for the sticky note icon */
      /* Standard sticky note icon size is approximately 24x24 units */
      fz_rect rect = fz_make_rect(note->x, note->y, note->x + 24.0, note->y + 24.0);
      pdf_set_annot_rect(ctx, annot, rect);

      /* Set the note content */
      if (note->content != NULL && note->content[0] != '\0') {
        pdf_set_annot_contents(ctx, annot, note->content);
      }

      /* Update the annotation to apply changes */
      pdf_update_annot(ctx, annot);

      exported_count++;
      g_debug("pdf_page_export_notes: Exported note at (%.1f, %.1f) with content: %.30s...",
              note->x, note->y, note->content ? note->content : "(empty)");
    );
  } fz_catch(ctx) {
    g_warning("pdf_page_export_notes: MuPDF exception during export");
    result = ZATHURA_ERROR_UNKNOWN;
  }

  g_mutex_unlock(&mupdf_document->mutex);

  g_message("pdf_page_export_notes: Exported %u notes to page %u",
            exported_count, zathura_page_get_index(page));

  return result;
}
