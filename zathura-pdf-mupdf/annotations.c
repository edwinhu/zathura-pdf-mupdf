/* SPDX-License-Identifier: Zlib */

#include <glib.h>
#include <math.h>
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

static void get_color_rgb(zathura_highlight_color_t color, float rgb[3]) {
  switch (color) {
    case ZATHURA_HIGHLIGHT_YELLOW:
      rgb[0] = 1.0f; rgb[1] = 1.0f; rgb[2] = 0.0f;
      break;
    case ZATHURA_HIGHLIGHT_GREEN:
      rgb[0] = 0.0f; rgb[1] = 1.0f; rgb[2] = 0.0f;
      break;
    case ZATHURA_HIGHLIGHT_BLUE:
      rgb[0] = 0.0f; rgb[1] = 0.5f; rgb[2] = 1.0f;
      break;
    case ZATHURA_HIGHLIGHT_RED:
      rgb[0] = 1.0f; rgb[1] = 0.0f; rgb[2] = 0.0f;
      break;
    default:
      rgb[0] = 1.0f; rgb[1] = 1.0f; rgb[2] = 0.0f;
  }
}

zathura_error_t pdf_page_export_annotations(zathura_page_t* page, void* data,
                                             girara_list_t* highlights) {
  g_debug("pdf_page_export_annotations called");

  if (page == NULL || data == NULL || highlights == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  mupdf_page_t* mupdf_page = data;
  zathura_document_t* document = zathura_page_get_document(page);

  if (document == NULL || mupdf_page->page == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);
  if (mupdf_document == NULL || mupdf_document->ctx == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  /* Get page height for coordinate conversion */
  double page_height = zathura_page_get_height(page);
  unsigned int page_id = zathura_page_get_index(page);

  g_mutex_lock(&mupdf_document->mutex);

  fz_context* ctx = mupdf_document->ctx;

  /* Get pdf_page from fz_page */
  pdf_page* ppage = pdf_page_from_fz_page(ctx, mupdf_page->page);
  if (ppage == NULL) {
    g_debug("pdf_page_from_fz_page returned NULL");
    g_mutex_unlock(&mupdf_document->mutex);
    return ZATHURA_ERROR_UNKNOWN;
  }

  g_debug("Exporting %zu highlights to page %u (height: %f)",
          girara_list_size(highlights), page_id, page_height);

  int exported = 0;
  zathura_error_t result = ZATHURA_ERROR_OK;

  fz_try(ctx) {
    /* Iterate through highlights and create PDF annotations */
    GIRARA_LIST_FOREACH_BODY(highlights, zathura_highlight_t*, hl,
      /* Only export highlights for this page */
      if (hl->page != page_id) {
        continue;
      }

      /* Convert zathura rectangles to PDF quad points */
      girara_list_t* rects = hl->rects;
      if (rects == NULL || girara_list_size(rects) == 0) {
        g_debug("Highlight has no rectangles, skipping");
        continue;
      }

      /* Allocate array for all quad points */
      size_t num_quads = girara_list_size(rects);
      fz_quad* quads = fz_malloc_array(ctx, num_quads, fz_quad);

      /* Convert each rectangle to a quad point */
      size_t quad_index = 0;
      GIRARA_LIST_FOREACH_BODY(rects, zathura_rectangle_t*, rect,
        /* Convert zathura coordinates (top-left origin, Y down)
         * to PDF coordinates (bottom-left origin, Y up) */
        quads[quad_index].ul.x = rect->x1;
        quads[quad_index].ul.y = page_height - rect->y1;  /* upper-left */
        quads[quad_index].ur.x = rect->x2;
        quads[quad_index].ur.y = page_height - rect->y1;  /* upper-right */
        quads[quad_index].ll.x = rect->x1;
        quads[quad_index].ll.y = page_height - rect->y2;  /* lower-left */
        quads[quad_index].lr.x = rect->x2;
        quads[quad_index].lr.y = page_height - rect->y2;  /* lower-right */
        quad_index++;
      );

      /* Create PDF highlight annotation */
      pdf_annot* annot = pdf_create_annot(ctx, ppage, PDF_ANNOT_HIGHLIGHT);
      if (annot == NULL) {
        g_debug("Failed to create annotation");
        fz_free(ctx, quads);
        continue;
      }

      /* Set quad points for the annotation */
      pdf_set_annot_quad_points(ctx, annot, num_quads, quads);

      /* Free the quad array */
      fz_free(ctx, quads);

      /* Set annotation color */
      float rgb[3];
      get_color_rgb(hl->color, rgb);
      pdf_set_annot_color(ctx, annot, 3, rgb);

      /* Set text content if available */
      if (hl->text != NULL && hl->text[0] != '\0') {
        pdf_set_annot_contents(ctx, annot, hl->text);
      }

      /* Update annotation appearance */
      pdf_update_annot(ctx, annot);

      exported++;
      g_debug("Exported highlight %d with %zu rectangles", exported, num_quads);
    );
  }
  fz_catch(ctx) {
    g_debug("Exception caught during annotation export");
    result = ZATHURA_ERROR_UNKNOWN;
  }

  g_mutex_unlock(&mupdf_document->mutex);

  g_debug("Exported %d highlights to page %u", exported, page_id);
  return result;
}

/* Check if two rectangles match within tolerance */
static bool rect_matches(zathura_rectangle_t* zr, double x0, double y0, double x1, double y1,
                         double page_height, double eps) {
  /* Convert PDF coordinates to zathura coordinates for comparison */
  double z_y1 = page_height - y1;  /* PDF y1 -> zathura y1 */
  double z_y2 = page_height - y0;  /* PDF y0 -> zathura y2 */

  return (fabs(zr->x1 - x0) < eps && fabs(zr->x2 - x1) < eps &&
          fabs(zr->y1 - z_y1) < eps && fabs(zr->y2 - z_y2) < eps);
}

/* Check if annotation geometry matches given rectangles */
static bool annot_geometry_matches(fz_context* ctx, pdf_annot* annot,
                                   girara_list_t* rects, double page_height) {
  int quad_count = pdf_annot_quad_point_count(ctx, annot);
  size_t rect_count = girara_list_size(rects);

  if ((size_t)quad_count != rect_count) {
    return false;
  }

  const double eps = 1.0;

  for (int i = 0; i < quad_count; i++) {
    fz_quad quad = pdf_annot_quad_point(ctx, annot, i);
    fz_rect r = fz_rect_from_quad(quad);

    zathura_rectangle_t* zr = girara_list_nth(rects, i);
    if (zr == NULL || !rect_matches(zr, r.x0, r.y0, r.x1, r.y1, page_height, eps)) {
      return false;
    }
  }
  return true;
}

zathura_error_t pdf_page_delete_annotation(zathura_page_t* page, void* data,
                                            girara_list_t* rects) {
  g_debug("pdf_page_delete_annotation called");

  if (page == NULL || data == NULL || rects == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  mupdf_page_t* mupdf_page = data;
  zathura_document_t* document = zathura_page_get_document(page);

  if (document == NULL || mupdf_page->page == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  mupdf_document_t* mupdf_document = zathura_document_get_data(document);
  if (mupdf_document == NULL || mupdf_document->ctx == NULL) {
    return ZATHURA_ERROR_INVALID_ARGUMENTS;
  }

  /* Get page height for coordinate conversion */
  double page_height = zathura_page_get_height(page);
  unsigned int page_id = zathura_page_get_index(page);

  g_mutex_lock(&mupdf_document->mutex);

  fz_context* ctx = mupdf_document->ctx;

  /* Get pdf_page from fz_page */
  pdf_page* ppage = pdf_page_from_fz_page(ctx, mupdf_page->page);
  if (ppage == NULL) {
    g_debug("pdf_page_from_fz_page returned NULL");
    g_mutex_unlock(&mupdf_document->mutex);
    return ZATHURA_ERROR_UNKNOWN;
  }

  g_debug("Deleting annotation on page %u (height: %f) with %zu rectangles",
          page_id, page_height, girara_list_size(rects));

  bool found = false;
  zathura_error_t result = ZATHURA_ERROR_OK;

  fz_try(ctx) {
    /* Iterate through annotations to find matching one */
    for (pdf_annot* annot = pdf_first_annot(ctx, ppage); annot != NULL; ) {
      /* Filter for highlight, underline, and strikeout annotations */
      enum pdf_annot_type type = pdf_annot_type(ctx, annot);
      if (type != PDF_ANNOT_HIGHLIGHT &&
          type != PDF_ANNOT_UNDERLINE &&
          type != PDF_ANNOT_STRIKE_OUT) {
        annot = pdf_next_annot(ctx, annot);
        continue;
      }

      /* Check if geometry matches */
      if (annot_geometry_matches(ctx, annot, rects, page_height)) {
        g_debug("Found matching annotation, deleting");
        /* Delete the annotation */
        pdf_delete_annot(ctx, ppage, annot);
        found = true;
        break;
      }

      annot = pdf_next_annot(ctx, annot);
    }

    if (!found) {
      g_debug("No matching annotation found");
      result = ZATHURA_ERROR_UNKNOWN;
    }
  }
  fz_catch(ctx) {
    g_debug("Exception caught during annotation deletion");
    result = ZATHURA_ERROR_UNKNOWN;
  }

  g_mutex_unlock(&mupdf_document->mutex);

  if (found) {
    g_debug("Successfully deleted annotation on page %u", page_id);
  }

  return result;
}
