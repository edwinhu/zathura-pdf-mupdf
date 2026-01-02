/* SPDX-License-Identifier: Zlib */

#include <glib.h>
#include <mupdf/pdf.h>

#include "plugin.h"
#include "utils.h"

/* Temporary structure to hold annotation data before text extraction */
typedef struct {
  girara_list_t* rects;
  zathura_highlight_color_t color;
  fz_rect annot_rect;
} annot_data_t;

static void annot_data_free(annot_data_t* data) {
  if (data != NULL) {
    if (data->rects != NULL) {
      girara_list_free(data->rects);
    }
    g_free(data);
  }
}

static zathura_highlight_color_t map_color(int n, float c[4]) {
  if (n < 3) {
    return ZATHURA_HIGHLIGHT_YELLOW;
  }

  float r = c[0], g = c[1], b = c[2];

  if (r > 0.7f && g > 0.7f && b < 0.5f) {
    return ZATHURA_HIGHLIGHT_YELLOW;
  }
  if (g > 0.6f && g > r && g > b) {
    return ZATHURA_HIGHLIGHT_GREEN;
  }
  if (b > 0.5f && b > r) {
    return ZATHURA_HIGHLIGHT_BLUE;
  }
  if (r > 0.6f && r > g && r > b) {
    return ZATHURA_HIGHLIGHT_RED;
  }

  return ZATHURA_HIGHLIGHT_YELLOW;
}

girara_list_t* pdf_page_get_annotations(zathura_page_t* page, void* data,
                                         zathura_error_t* error) {
  g_debug("pdf_page_get_annotations called");

  if (page == NULL || data == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    return NULL;
  }

  mupdf_page_t* mupdf_page = data;
  zathura_document_t* document = zathura_page_get_document(page);

  if (document == NULL || mupdf_page->page == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    return NULL;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);
  if (mupdf_document == NULL || mupdf_document->ctx == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_INVALID_ARGUMENTS;
    }
    return NULL;
  }

  /* Create result list with highlight cleanup function */
  girara_list_t* list = girara_list_new_with_free((girara_free_function_t)zathura_highlight_free);
  if (list == NULL) {
    if (error != NULL) {
      *error = ZATHURA_ERROR_OUT_OF_MEMORY;
    }
    return NULL;
  }

  g_mutex_lock(&mupdf_document->mutex);

  fz_context* ctx = mupdf_document->ctx;

  /* Get pdf_page from fz_page */
  pdf_page* ppage = pdf_page_from_fz_page(ctx, mupdf_page->page);
  if (ppage == NULL) {
    g_debug("pdf_page_from_fz_page returned NULL");
    g_mutex_unlock(&mupdf_document->mutex);
    girara_list_free(list);
    if (error != NULL) {
      *error = ZATHURA_ERROR_UNKNOWN;
    }
    return NULL;
  }

  /* Get page height for coordinate conversion */
  double page_height = zathura_page_get_height(page);
  unsigned int page_id = zathura_page_get_index(page);

  g_debug("Processing page %u (height: %f)", page_id, page_height);

  /* Extract text from page if not already extracted */
  if (mupdf_page->extracted_text == false) {
    mupdf_page_extract_text(mupdf_document, mupdf_page);
  }

  /* Create temporary list to hold annotation data */
  girara_list_t* annot_data_list = girara_list_new_with_free((girara_free_function_t)annot_data_free);
  if (annot_data_list == NULL) {
    g_mutex_unlock(&mupdf_document->mutex);
    girara_list_free(list);
    if (error != NULL) {
      *error = ZATHURA_ERROR_OUT_OF_MEMORY;
    }
    return NULL;
  }

  /* Phase 1: Collect annotation data inside fz_try (using consistent document context) */
  int annot_count = 0;
  int highlight_count = 0;
  fz_try(ctx) {
    for (pdf_annot* annot = pdf_first_annot(ctx, ppage); annot != NULL; annot = pdf_next_annot(ctx, annot)) {
      annot_count++;
      /* Filter for highlight, underline, and strikeout annotations */
      enum pdf_annot_type type = pdf_annot_type(ctx, annot);
      g_debug("Annotation %d: type=%d", annot_count, type);
      if (type != PDF_ANNOT_HIGHLIGHT &&
          type != PDF_ANNOT_UNDERLINE &&
          type != PDF_ANNOT_STRIKE_OUT) {
        continue;
      }
      highlight_count++;

      /* Get quad point count */
      int quad_count = pdf_annot_quad_point_count(ctx, annot);
      if (quad_count <= 0) {
        continue;
      }

      /* Create list of rectangles for this annotation */
      girara_list_t* rects = girara_list_new_with_free(g_free);
      if (rects == NULL) {
        continue;
      }

      /* Initialize bounding box from quad points */
      fz_rect annot_rect = fz_empty_rect;

      /* Process each quad point */
      for (int i = 0; i < quad_count; i++) {
        fz_quad quad = pdf_annot_quad_point(ctx, annot, i);

        /* Convert quad to rectangle - PDF coordinates to zathura coordinates */
        fz_rect r = fz_rect_from_quad(quad);

        /* Expand bounding box to include this quad */
        annot_rect = fz_union_rect(annot_rect, r);

        zathura_rectangle_t* rect = g_try_malloc0(sizeof(zathura_rectangle_t));
        if (rect == NULL) {
          continue;
        }

        /* Convert coordinates: PDF has origin at bottom-left, Y up
         * Zathura has origin at top-left, Y down */
        rect->x1 = r.x0;
        rect->x2 = r.x1;
        rect->y1 = page_height - r.y1;  /* Flip Y */
        rect->y2 = page_height - r.y0;  /* Flip Y */

        girara_list_append(rects, rect);
      }

      /* Skip if no rectangles were created */
      if (girara_list_size(rects) == 0) {
        girara_list_free(rects);
        continue;
      }

      /* Get annotation color */
      int n;
      float color[4];
      pdf_annot_color(ctx, annot, &n, color);
      zathura_highlight_color_t hl_color = map_color(n, color);

      /* Store annotation data for text extraction outside fz_try */
      annot_data_t* data = g_try_malloc0(sizeof(annot_data_t));
      if (data != NULL) {
        data->rects = rects;
        data->color = hl_color;
        data->annot_rect = annot_rect;
        girara_list_append(annot_data_list, data);
      } else {
        girara_list_free(rects);
      }
    }
  }
  fz_catch(ctx) {
    g_debug("Exception caught during annotation processing");
    g_mutex_unlock(&mupdf_document->mutex);
    girara_list_free(annot_data_list);
    girara_list_free(list);
    if (error != NULL) {
      *error = ZATHURA_ERROR_UNKNOWN;
    }
    return NULL;
  }

  /* Phase 2: Extract text outside fz_try using page context (like select.c) */
  GIRARA_LIST_FOREACH_BODY(annot_data_list, annot_data_t*, data,
    char* text = NULL;
    if (mupdf_page->text != NULL) {
      fz_point a = {data->annot_rect.x0, data->annot_rect.y0};
      fz_point b = {data->annot_rect.x1, data->annot_rect.y1};
      text = fz_copy_selection(mupdf_page->ctx, mupdf_page->text, a, b, 0);
      if (text != NULL) {
        g_debug("Extracted text: %.50s%s", text, strlen(text) > 50 ? "..." : "");
      }
    }

    zathura_highlight_t* highlight = zathura_highlight_new(page_id, data->rects, data->color, text);
    if (text != NULL) {
      fz_free(mupdf_page->ctx, text);
    }
    if (highlight != NULL) {
      g_debug("Created highlight with %zu rectangles", girara_list_size(data->rects));
      girara_list_append(list, highlight);
      /* Prevent double-free: rects now owned by highlight */
      data->rects = NULL;
    }
  );

  girara_list_free(annot_data_list);
  g_debug("Total annotations: %d, highlights: %d, returning %zu items", annot_count, highlight_count, girara_list_size(list));
  g_mutex_unlock(&mupdf_document->mutex);

  if (error != NULL) {
    *error = ZATHURA_ERROR_OK;
  }

  return list;
}
