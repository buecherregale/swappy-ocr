#include <stdio.h>
#include <string.h>

#include <tesseract/capi.h>
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <leptonica/allheaders.h>

#include "clipboard.h"
#include "pixbuf.h"
#include "util.h"

#include "ocr.h"

#define SCALER_X 2
#define SCALER_Y 2
#define PADDING  5
#define TESS_LEVEL RIL_TEXTLINE

GtkWidget *draw_text_block_overlay(GtkWidget *parent,
        gint x, gint y, gint w, gint h,
        GCallback on_click,
        char *text) {
    GtkWidget *btn = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "ocr-overlay");
    gtk_widget_set_size_request(btn, w, h);
    gtk_fixed_put(GTK_FIXED(parent), btn, x, y);
    char *userdata = g_strdup(text);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_click), g_strdup(userdata));
    g_signal_connect_swapped(btn, "destroy", G_CALLBACK(g_free), userdata);
    gtk_widget_show(btn);
    return btn;
}

static void on_ocr_button_clicked(GtkButton *btn, gpointer text) {
    bool success = clipboard_copy_text_to_gdk_clipboard(text);
    if (!success) {
        g_printerr("Failed to copy text to clipboard\n");
    }
}

/**
* Convert the given GdkPixbuf 1:1 to a libtonica PIX for further processing.
*
* The returned PIX is onwed by the caller.
*/
PIX *convert_pixbuf_to_pix(GdkPixbuf *pixbuf) {
    int width      = gdk_pixbuf_get_width(pixbuf);
    int height     = gdk_pixbuf_get_height(pixbuf);
    int rowstride  = gdk_pixbuf_get_rowstride(pixbuf);
    int channels   = gdk_pixbuf_get_n_channels(pixbuf);
    gboolean alpha = gdk_pixbuf_get_has_alpha(pixbuf);

    guchar *src = gdk_pixbuf_get_pixels(pixbuf);

    PIX *pix = pixCreate(width, height, 32);
    if (!pix)
        return NULL;

    for (int y = 0; y < height; y++) {
        guchar *s = src + y * rowstride;

        for (int x = 0; x < width; x++) {
            l_uint32 pixel;

            l_uint8 r = s[0];
            l_uint8 g = s[1];
            l_uint8 b = s[2];
            l_uint8 a = alpha ? s[3] : 255;

            composeRGBAPixel(r, g, b, a, &pixel);
            pixSetPixel(pix, x, y, pixel);

            s += channels;
        }
    }
    return pix;
}

/**
* Attempts to optimize the given PIX for better character recognition.
* Binarizes the PIX, making it ready for tesseract.
*
* The given AND returned PIX are owned by the caller.
*/
PIX *optimize_pix_for_ocr(PIX *pix) {
    PIX *scaled = pixScale(pix, SCALER_X, SCALER_Y);
    PIX *gray = pixConvertTo8(scaled, 0);
    pixDestroy(&scaled);

    PIX *binary;
    PIX *tmp;

    pixOtsuAdaptiveThreshold(
        gray,
        200, 200,
        0, 0,
        0.1,
        &tmp, &binary);
 
    pixDestroy(&gray);
    pixDestroy(&tmp);

    return binary;
}

bool ocr_find_text(GtkWidget *clicked, struct swappy_state *state) {
    if (!state) return false;
    if (state->ocr->overlay_buttons != NULL)
        ocr_remove_overlay_buttons(state);

    GdkPixbuf *pixbuf = pixbuf_get_from_state(state);
    if (!pixbuf) {
        g_printerr("Could not get pixbuf from state\n");
        return false;
    }

    PIX *pix = convert_pixbuf_to_pix(pixbuf);
    if (pix == NULL) {
        g_printerr("Could not convert to PIX\n");
        return false;
    }

    PIX *binary = optimize_pix_for_ocr(pix);
    pixDestroy(&pix);

    // ocr
    TessBaseAPI *tessApi = state->ocr->api;
    if (tessApi == NULL) {
        tessApi = TessBaseAPICreate(); // could probably reuse this, but how often will the user click

        char *lang = state->config->tesseract_languages;
        if (lang == NULL) {
            g_printerr("No OCR languages set\n");
            return false;
        }
        bool err = TessBaseAPIInit3(tessApi, NULL, lang);
        if (err) {
            g_printerr("Failed to initialize tesseract.\n");
            return false;
        }
    }
    TessBaseAPISetImage2(tessApi, binary);
    TessBaseAPIRecognize(tessApi, NULL);
    TessResultIterator* ri = TessBaseAPIGetIterator(tessApi);

    if (ri) {
        GtkWidget *fixed = state->ui->fixed;
        GtkWidget **btns = NULL;
        size_t btnCount = 0;
        size_t btnCapacity = 0;

        do {
            int left, top, right, bottom;
            if (TessPageIteratorBoundingBox(
                TessResultIteratorGetPageIterator(ri),
                TESS_LEVEL,
                &left, &top, &right, &bottom)) {
                if (btnCount == btnCapacity) {
                    btnCapacity = btnCapacity ? btnCapacity * 2 : 8;
                    btns = realloc(btns, btnCapacity * sizeof(*btns));
                }
                char *blockText = TessResultIteratorGetUTF8Text(ri, TESS_LEVEL);

                GtkWidget *btn = draw_text_block_overlay(fixed,
                    left / SCALER_X + PADDING,
                    top / SCALER_Y + PADDING,
                    (right - left) / SCALER_X + 2 * PADDING,
                    (bottom - top) / SCALER_Y + 2 * PADDING,
                    G_CALLBACK(on_ocr_button_clicked), blockText);
                TessDeleteText(blockText); // got copied for gtk userdata
                btns[btnCount] = btn;
                ++btnCount;
            }
        } while (TessResultIteratorNext(ri, TESS_LEVEL));
        if (btnCount == btnCapacity) {
            btnCapacity += 1;
            btns = realloc(btns, btnCapacity * sizeof(*btns));
        }
        btns[btnCount] = NULL;
        state->ocr->overlay_buttons = btns;
    }

    // cleanup
    pixDestroy(&binary);

    TessBaseAPIEnd(tessApi);
    TessBaseAPIDelete(tessApi);

    g_object_unref(pixbuf);

    if (state->config && state->config->early_exit) {
        gtk_main_quit();
    }
    return true;
}

void ocr_remove_overlay_buttons(struct swappy_state *state) {
    if (!state->ocr->overlay_buttons || !state->ui->fixed)
        return;
    GtkWidget **buttons = state->ocr->overlay_buttons;
    GtkWidget *fixed = state->ui->fixed;
    for (size_t i = 0; buttons[i] != NULL; i++) {
        gtk_container_remove(GTK_CONTAINER(fixed), buttons[i]);
    }
    free(buttons);
    state->ocr->overlay_buttons = NULL;
}

void ocr_cleanup(struct swappy_state *state) {
    if (state->ocr->api) {
        TessBaseAPIEnd(state->ocr->api);
        TessBaseAPIDelete(state->ocr->api);
        state->ocr->api = NULL;
    }
    if (state->ocr->overlay_buttons) {
        free(state->ocr->overlay_buttons);
        state->ocr->overlay_buttons = NULL;
    }
}
