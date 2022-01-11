/*
 * This is free and unencumbered software released into the public domain.
 *
 * For more information, please refer to <https://unlicense.org>
 */

// TODO: GIMP has a mechanism for returning textual error messages to the core
// which are then displayed for you. This would be preferred to the plug-in
// showing its own dialog box with the message, which is what is done right
// now.

#include <stdbool.h>
#include <assert.h>

#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>

#define QOI_HEADER_SIZE 14
#define QOI_END_MARKER_SIZE 8
#define QOI_MAX_BYTES_PER_PIXEL 5

#define QOI_CHANNELS_RGB 3
#define QOI_CHANNELS_RGBA 4

#define QOI_SMALL_TAG_MASK 0xC0
#define QOI_OP_RGB 0xFE
#define QOI_OP_RGBA 0xFF
#define QOI_OP_INDEX 0x00
#define QOI_OP_DIFF 0x40
#define QOI_OP_LUMA 0x80
#define QOI_OP_RUN 0xC0

#define QOI_MAX_RUN_LENGTH 62
#define QOI_DIFF_LOWER_BOUND -2
#define QOI_DIFF_UPPER_BOUND 1
#define QOI_LUMA_GREEN_LOWER_BOUND -32
#define QOI_LUMA_GREEN_UPPER_BOUND 31
#define QOI_LUMA_RED_BLUE_LOWER_BOUND -8
#define QOI_LUMA_RED_BLUE_UPPER_BOUND 7

typedef struct {
	guint8 red;
	guint8 green;
	guint8 blue;
	guint8 alpha;
} QoiPixel;

typedef enum {
	QOI_COLORSPACE_SRGB = 0,
	QOI_COLORSPACE_LINEAR,
	QOI_COLORSPACE_COUNT,
} QoiColorspace;

typedef struct {
	QoiPixel     *pixels;
	guint32       width;
	guint32       height;
	QoiColorspace colorspace;
	bool          has_alpha;
} QoiImage;

typedef struct {
	gchar   magic[4];
	guint32 width;
	guint32 height;
	guint8  channels;
	guint8  colorspace;
} QoiHeader;

static const guint8 QOI_END_MARKER[] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01
};
static_assert(sizeof(QOI_END_MARKER) == QOI_END_MARKER_SIZE);

static inline guint32 guint32_swap_local_and_big_endian(guint32 value) {
#if G_BYTE_ORDER == G_LITTLE_ENDIAN
	return (
		((value & 0xFF000000) >> 24) |
		((value & 0x00FF0000) >>  8) |
		((value & 0x0000FF00) <<  8) |
		((value & 0x000000FF) << 24));
#else
	return value;
#endif
}

static inline bool qoi_pixel_equal(QoiPixel a, QoiPixel b) {
	return a.red == b.red && a.green == b.green && a.blue == b.blue && a.alpha == b.alpha;
}

static inline guint qoi_pixel_hash(QoiPixel pixel) {
	return (pixel.red * 3 + pixel.green * 5 + pixel.blue * 7 + pixel.alpha * 11) % 64;
}

// The only reason the GIMP API is used in this function is to indicate
// progress to the user. Updating the progress for every pixel would slow down
// loading a lot, so it is only updated when either a QOI_OP_RGB or QOI_OP_RGBA
// chunk is decoded.
static bool load_image(const gchar *filename, QoiImage *result) {
	gimp_progress_init_printf("Opening '%s'", filename);

	gint file_size;
	guint8 *file_data = 0;

	errno = 0;
	FILE *file = fopen(filename, "rb");
	if (file) {
		if (fseek(file, 0, SEEK_END) != -1) {
			file_size = ftell(file);
			if (file_size != -1) {
				if (fseek(file, 0, SEEK_SET) != -1) {
					file_data = g_try_malloc(file_size);
					if (!file_data) {
						errno = ENOMEM;
					} else {
						if (fread(file_data, 1, file_size, file) != file_size) {
							g_free(file_data);
							file_data = 0;
						}
					}
				}
			}
		}

		// There is no point in checking for failure when closing the fail as
		// there is noting that can be done about it.
		fclose(file);
	}

	if (!file_data) {
		g_message("Could not read from file. %s", strerror(errno));
		return false;
	}

	gint32 file_index = 0;
	if (file_size < file_index + QOI_HEADER_SIZE) {
		g_message("The file ends unexpectedly.");
		g_free(file_data);
		return false;
	}

	QoiHeader header = *(QoiHeader *) &file_data[file_index];
	file_index += QOI_HEADER_SIZE;

	if (memcmp(header.magic, "qoif", 4) != 0) {
		g_message("'%s' is not a valid QOI file.", filename);
		g_free(file_data);
		return false;
	}

	switch (header.channels) {
		case QOI_CHANNELS_RGB: result->has_alpha = false; break;
		case QOI_CHANNELS_RGBA: result->has_alpha = true; break;
		default: {
			g_message("Unsupported or unknown number of channels: %u.", header.channels);
			g_free(file_data);
			return false;
		} break;
	}

	switch (header.colorspace) {
		case QOI_COLORSPACE_SRGB: break;
		case QOI_COLORSPACE_LINEAR: break;
		default: {
			g_message("Unsupported or unknown colorspace: %u.", header.colorspace);
			g_free(file_data);
			return false;
		} break;
	}
	result->colorspace = header.colorspace;

	result->width = guint32_swap_local_and_big_endian(header.width);
	result->height = guint32_swap_local_and_big_endian(header.height);

	if (result->width == 0 || result->width > GIMP_MAX_IMAGE_SIZE) {
		g_message("Invalid or unsupported width: %u.", header.width);
		g_free(file_data);
		return false;
	}

	if (result->height == 0 || result->height > GIMP_MAX_IMAGE_SIZE) {
		g_message("Invalid or unsupported height: %u.", header.height);
		g_free(file_data);
		return false;
	}

	result->pixels = g_try_malloc(result->width * result->height * sizeof(*result->pixels));
	if (!result->pixels) {
		g_message("Failed to acquire storage for pixels.");
		g_free(file_data);
		return false;
	}

	guint32 pixel_index     = 0;
	guint64 max_pixel_index = result->width * result->height;
	QoiPixel current_pixel  = { .alpha = 255 };
	QoiPixel array[64]      = { 0 };
	while (pixel_index < max_pixel_index) {
		// Make sure there enough file data for the end marker, as that means
		// there is enough data for any of the chunks as well.
		if (file_size < file_index + QOI_END_MARKER_SIZE) {
			g_message("The file ends unexpectedly.");
			g_free(result->pixels);
			g_free(file_data);
			return false;
		}

		guint8 tag = file_data[file_index++];
		if (tag == QOI_OP_RGB) {
			current_pixel.red   = file_data[file_index++];
			current_pixel.green = file_data[file_index++];
			current_pixel.blue  = file_data[file_index++];

			result->pixels[pixel_index++] = current_pixel;
			array[qoi_pixel_hash(current_pixel)] = current_pixel;
			gimp_progress_update((gdouble) pixel_index / (gdouble) max_pixel_index);
		} else if (tag == QOI_OP_RGBA) {
			current_pixel.red   = file_data[file_index++];
			current_pixel.green = file_data[file_index++];
			current_pixel.blue  = file_data[file_index++];
			current_pixel.alpha = file_data[file_index++];

			result->pixels[pixel_index++] = current_pixel;
			array[qoi_pixel_hash(current_pixel)] = current_pixel;
			gimp_progress_update((gdouble) pixel_index / (gdouble) max_pixel_index);
		} else if ((tag & QOI_SMALL_TAG_MASK) == QOI_OP_INDEX) {
			// There might be an end marker here as this chunk could be a 0
			// byte which is the same byte the end marker starts with, so check
			// just in case as the stream technically ends when the decoder
			// reaches the end marker.
			if (memcmp(&file_data[file_index - 1], QOI_END_MARKER, QOI_END_MARKER_SIZE) == 0) {
				// Breaking here means the decoder does an extra check for the
				// end marker, but that should be fast enough that it doesn't
				// matter.
				break;
			}

			guint8 index = tag & 0x3F;
			current_pixel = array[index];
			result->pixels[pixel_index++] = current_pixel;
		} else if ((tag & QOI_SMALL_TAG_MASK) == QOI_OP_DIFF) {
			gint dr = ((tag >> 4) & 0x03) + QOI_DIFF_LOWER_BOUND;
			gint dg = ((tag >> 2) & 0x03) + QOI_DIFF_LOWER_BOUND;
			gint db = ((tag >> 0) & 0x03) + QOI_DIFF_LOWER_BOUND;

			current_pixel.red   += dr;
			current_pixel.green += dg;
			current_pixel.blue  += db;

			result->pixels[pixel_index++] = current_pixel;
			array[qoi_pixel_hash(current_pixel)] = current_pixel;
		} else if ((tag & QOI_SMALL_TAG_MASK) == QOI_OP_LUMA) {
			guint8 dr_db = file_data[file_index++];

			gint dg = (tag & 0x3F)          + QOI_LUMA_GREEN_LOWER_BOUND;
			gint dr = ((dr_db >> 4) & 0x0F) + QOI_LUMA_RED_BLUE_LOWER_BOUND + dg;
			gint db = ((dr_db >> 0) & 0x0F) + QOI_LUMA_RED_BLUE_LOWER_BOUND + dg;

			current_pixel.red   += dr;
			current_pixel.green += dg;
			current_pixel.blue  += db;

			result->pixels[pixel_index++] = current_pixel;
			array[qoi_pixel_hash(current_pixel)] = current_pixel;
		} else if ((tag & QOI_SMALL_TAG_MASK) == QOI_OP_RUN) {
			guint8 run = (tag & 0x3F) + 1;

			// Make sure there is enough space for all of the encoded pixels
			if (pixel_index > max_pixel_index + run) {
				g_message("Too many encoded pixels.");
				g_free(result->pixels);
				g_free(file_data);
				return false;
			}

			while (run-- != 0) result->pixels[pixel_index++] = current_pixel;
		}
	}

	if (file_size < file_index + QOI_END_MARKER_SIZE) {
		g_message("The file ends unexpectedly.");
		g_free(result->pixels);
		g_free(file_data);
		return false;
	}

	if (memcmp(&file_data[file_index], QOI_END_MARKER, QOI_END_MARKER_SIZE) != 0) {
		g_message("Invalid end marker.");
		g_free(result->pixels);
		g_free(file_data);
		return false;
	}
	file_index += QOI_END_MARKER_SIZE;

	if (file_index != file_size) {
		g_message("File contains data past the end marker.");
		g_free(result->pixels);
		g_free(file_data);
		return false;
	}

	g_free(file_data);

	gimp_progress_end();

	return true;
}

// The only reason the GIMP API is used in this function is to indicate
// progress to the user. Updating the progress for every pixel would slow down
// saving a lot, so it is only updated when either a QOI_OP_RGB or QOI_OP_RGBA
// chunk is encoded.
static bool save_image(QoiImage image, const gchar *filename) {
	gimp_progress_init_printf("Exporting '%s'", filename);

	guint8 *file_data = g_try_malloc(
		QOI_HEADER_SIZE +
		image.width * image.height * QOI_MAX_BYTES_PER_PIXEL +
		QOI_END_MARKER_SIZE
	);
	if (!file_data) {
		return false;
	}
	gint32 file_index = 0;

	QoiHeader header;
	header.magic[0]   = 'q';
	header.magic[1]   = 'o';
	header.magic[2]   = 'i';
	header.magic[3]   = 'f';
	header.width      = guint32_swap_local_and_big_endian(image.width);
	header.height     = guint32_swap_local_and_big_endian(image.height);
	header.channels   = image.has_alpha ? QOI_CHANNELS_RGBA : QOI_CHANNELS_RGB;
	header.colorspace = image.colorspace;

	*(QoiHeader *) &file_data[file_index] = header;
	file_index += QOI_HEADER_SIZE;

	guint64 max_pixel_index = image.width * image.height;
	QoiPixel previous_pixel = { .alpha = 255 };
	QoiPixel array[64]      = { 0 };
	for (guint32 pixel_index = 0; pixel_index < max_pixel_index;) {
		QoiPixel current_pixel = image.pixels[pixel_index];
		guint32  hash          = qoi_pixel_hash(current_pixel);

		if (qoi_pixel_equal(previous_pixel, current_pixel)) {
			guint8 run = 0;
			bool process_next = true;
			while (process_next) {
				++run;
				++pixel_index;

				process_next = (
					pixel_index < max_pixel_index &&
					qoi_pixel_equal(previous_pixel, image.pixels[pixel_index])
				);
				if (run == QOI_MAX_RUN_LENGTH || !process_next) {
					file_data[file_index++] = QOI_OP_RUN | (run - 1);
					run = 0;
				}
			}
		} else if (qoi_pixel_equal(current_pixel, array[hash])) {
			file_data[file_index++] = QOI_OP_INDEX | hash;
			previous_pixel = current_pixel;
			++pixel_index;
		} else if (!image.has_alpha || current_pixel.alpha == previous_pixel.alpha) {
			gint32 dr = (gint32) (current_pixel.red   - previous_pixel.red);
			gint32 dg = (gint32) (current_pixel.green - previous_pixel.green);
			gint32 db = (gint32) (current_pixel.blue  - previous_pixel.blue);
			gint32 dr_dg = dr - dg;
			gint32 db_dg = db - dg;

			if (
				QOI_DIFF_LOWER_BOUND <= dr && dr <= QOI_DIFF_UPPER_BOUND &&
				QOI_DIFF_LOWER_BOUND <= dg && dg <= QOI_DIFF_UPPER_BOUND &&
				QOI_DIFF_LOWER_BOUND <= db && db <= QOI_DIFF_UPPER_BOUND
			) {
				file_data[file_index++] =
					QOI_OP_DIFF |
					((dr - QOI_DIFF_LOWER_BOUND) << 4) |
					((dg - QOI_DIFF_LOWER_BOUND) << 2) |
					((db - QOI_DIFF_LOWER_BOUND) << 0);
			} else if (
				QOI_LUMA_GREEN_LOWER_BOUND <= dg && dg <= QOI_LUMA_GREEN_UPPER_BOUND &&
				QOI_LUMA_RED_BLUE_LOWER_BOUND <= dr_dg && dr_dg <= QOI_LUMA_RED_BLUE_UPPER_BOUND &&
				QOI_LUMA_RED_BLUE_LOWER_BOUND <= db_dg && db_dg <= QOI_LUMA_RED_BLUE_UPPER_BOUND
			) {
				file_data[file_index++] = QOI_OP_LUMA | (dg - QOI_LUMA_GREEN_LOWER_BOUND);
				file_data[file_index++] =
					((dr_dg - QOI_LUMA_RED_BLUE_LOWER_BOUND) << 4) |
					((db_dg - QOI_LUMA_RED_BLUE_LOWER_BOUND) << 0);
			} else {
				file_data[file_index++] = QOI_OP_RGB;
				file_data[file_index++] = current_pixel.red;
				file_data[file_index++] = current_pixel.green;
				file_data[file_index++] = current_pixel.blue;
				// pixel_index hasn't been incremented yet, add one to it
				gimp_progress_update((gdouble) (pixel_index + 1) / (gdouble) max_pixel_index);
			}

			array[hash] = current_pixel;
			previous_pixel = current_pixel;
			++pixel_index;
		} else {
			file_data[file_index++] = QOI_OP_RGBA;
			file_data[file_index++] = current_pixel.red;
			file_data[file_index++] = current_pixel.green;
			file_data[file_index++] = current_pixel.blue;
			file_data[file_index++] = current_pixel.alpha;
			array[hash] = current_pixel;
			previous_pixel = current_pixel;
			++pixel_index;
			gimp_progress_update((gdouble) pixel_index / (gdouble) max_pixel_index);
		}
	}

	memcpy(&file_data[file_index], QOI_END_MARKER, QOI_END_MARKER_SIZE);
	file_index += QOI_END_MARKER_SIZE;

	FILE *fd = fopen(filename, "wb");
	if (!fd) {
		g_free(file_data);
		return false;
	}

	if (fwrite(file_data, 1, file_index, fd) != file_index) {
		fclose(fd);
		g_free(file_data);
		return false;
	}

	fclose(fd);

	g_free(file_data);

	gimp_progress_end();

	return true;
}

#define DATE "2022"
#define LOAD_PROC "file-qoi-load"
#define SAVE_PROC "file-qoi-save"

typedef struct {
	QoiColorspace colorspace;
	bool          export_alpha;
} QoiExportOptions;

static GimpExportReturn show_export_dialog(gint32 *image, gint32 *drawable, QoiExportOptions *options) {
	GimpExportReturn export = GIMP_EXPORT_IGNORE;

	gimp_get_data(SAVE_PROC, options);

	gimp_ui_init("file-qoi", 0);

	export = gimp_export_image(image, drawable, "QOI", GIMP_EXPORT_CAN_HANDLE_RGB | GIMP_EXPORT_CAN_HANDLE_ALPHA);

	GtkWidget *dialog = gimp_export_dialog_new("QOI", "export", 0);
	gtk_window_set_resizable(GTK_WINDOW(dialog), false);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
	gtk_box_pack_start(GTK_BOX(gimp_export_dialog_get_content_area(dialog)), vbox, true, true, 0);
	gtk_widget_show(vbox);

	GtkWidget *toggle = gtk_check_button_new_with_label("Use alpha");
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), options->export_alpha);
	gtk_container_add(GTK_CONTAINER(vbox), toggle);
	gtk_widget_show(toggle);

	// As colorspaces are enumerated from 0, if the options are in the same
	// order as they are in the enum, the active index will match the
	// colorspace. If the values for colorspaces changes for future versions of
	// the format, this can change to something more complicated.
	GtkWidget *combo = gtk_combo_box_text_new();
	gtk_combo_box_text_insert_text(GTK_COMBO_BOX_TEXT(combo), QOI_COLORSPACE_SRGB, "SRGB");
	gtk_combo_box_text_insert_text(GTK_COMBO_BOX_TEXT(combo), QOI_COLORSPACE_LINEAR, "Linear");

	// Set one element as active so there is always a valid colorspace selected
	gtk_combo_box_set_active(GTK_COMBO_BOX(combo), options->colorspace);
	gtk_container_add(GTK_CONTAINER(vbox), combo);
	gtk_widget_show(combo);

	gint response = gtk_dialog_run(GTK_DIALOG(dialog));
	if (response == GTK_RESPONSE_CANCEL) {
		export = GIMP_EXPORT_CANCEL;
	}

	options->export_alpha = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
	options->colorspace = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));

	gtk_widget_destroy(dialog);

	gimp_set_data(SAVE_PROC, options, sizeof(*options));

	return export;
}

static gint32 create_gimp_image_from_qoi_image(QoiImage qoi_image, const gchar *filename) {
	// Layers only need to be deleted they are not added to an image. If they
	// are added to an image, deleting the image will delete the layer as well.
	// This is why gimp_item_delete is only called at one of the points of
	// failure, when the layer fails to attach to the image.

	gimp_progress_init("Transfering pixels");
	gegl_init(0, 0);

	gint32 image = gimp_image_new(qoi_image.width, qoi_image.height, GIMP_RGB);
	if (image == -1) {
		gegl_exit();
		return -1;
	}

	gimp_image_set_filename(image, filename);

	gint32 layer = gimp_layer_new(
		image,
		"Background",
		qoi_image.width, qoi_image.height,
		qoi_image.has_alpha ? GIMP_RGBA_IMAGE : GIMP_RGB_IMAGE,
		100, GIMP_NORMAL_MODE
	);
	if (layer == -1) {
		gimp_image_delete(image);
		gegl_exit();
		return -1;
	}

	if (!gimp_image_insert_layer(image, layer, 0, 0)) {
		gimp_item_delete(layer);
		gimp_image_delete(image);
		gegl_exit();
		return -1;
	}

	GeglBuffer *buffer = gimp_drawable_get_buffer(layer);
	if (!buffer) {
		gimp_image_delete(image);
		gegl_exit();
		return -1;
	}

	const Babl *format = 0;
	switch (qoi_image.colorspace) {
		case QOI_COLORSPACE_SRGB: format = babl_format("R~G~B~A u8"); break;
		case QOI_COLORSPACE_LINEAR: format = babl_format("RGBA u8"); break;
		default: assert(!"Not reached!"); break;
	}

	// It is faster to do a single call to gegl_buffer_set, but to give users
	// some feedback on what is happening, one call per row of pixels is perforemed and
	// the progress is updated after each.
	for (guint32 y = 0; y < qoi_image.height; ++y) {
		// This procedure doesn't indicate if it fails, it just doesn't put any pixels in the image.
		gegl_buffer_set(
			buffer,
			GEGL_RECTANGLE(0, y, qoi_image.width, 1), 0,
			format, &qoi_image.pixels[y * qoi_image.width],
			GEGL_AUTO_ROWSTRIDE
		);
		gimp_progress_update((gdouble) y / (gdouble) qoi_image.height);
	}
	g_object_unref(buffer);

	gegl_exit();
	gimp_progress_end();

	return image;
}

static bool get_qoi_image_from_gimp(gint32 drawable, QoiExportOptions options, QoiImage *result) {
	gimp_progress_init("Transfering pixels");

	result->colorspace = options.colorspace;
	result->has_alpha = options.export_alpha;

	gegl_init(0, 0);

	GeglBuffer *buffer = gimp_drawable_get_buffer(drawable);
	if (!buffer) {
		gegl_exit();
		return false;
	}

	result->width = gegl_buffer_get_width(buffer);
	result->height = gegl_buffer_get_height(buffer);

	result->pixels = g_try_malloc(result->width * result->height * sizeof(*result->pixels));
	if (!result->pixels) {
		g_object_unref(buffer);
		gegl_exit();
		return false;
	}

	const Babl *format = 0;
	switch (options.colorspace) {
		case QOI_COLORSPACE_SRGB: format = babl_format("R~G~B~A u8"); break;
		case QOI_COLORSPACE_LINEAR: format = babl_format("RGBA u8"); break;
		default: assert(!"Not reached!"); break;
	}

	// It is faster to do a single call to gegl_buffer_get, but to give users
	// some feedback on what is happening, one call per row of pixels is perforemed and
	// the progress is updated after each.
	for (guint32 y = 0; y < result->height; ++y) {
		// This procedure doesn't indicate if it fails, it just doesn't put any pixels in the image.
		gegl_buffer_get(
			buffer,
			GEGL_RECTANGLE(0, y, result->width, 1), 1,
			format, &result->pixels[y * result->width],
			GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE
		);
		gimp_progress_update((gdouble) y / (gdouble) result->height);
	}
	g_object_unref(buffer);

	gegl_exit();
	gimp_progress_end();

	return true;
}

static void query() {
	static const GimpParamDef load_args[] = {
		{ GIMP_PDB_INT32,    "run_mode",     "Run mode" },
		{ GIMP_PDB_STRING,   "filename",     "The name of the file to load" },
		{ GIMP_PDB_STRING,   "raw_filename", "The name entered" },
	};

	static const GimpParamDef load_return_vals[] = {
		{ GIMP_PDB_IMAGE,    "image",        "Output image" },
	};

	static const GimpParamDef save_args[] = {
		{ GIMP_PDB_INT32,    "run_mode",     "Run mode" },
		{ GIMP_PDB_IMAGE,    "image",        "Input inmage" },
		{ GIMP_PDB_DRAWABLE, "drawable",     "Drawable to save" },
		{ GIMP_PDB_STRING,   "filename",     "The name of the file to load" },
		{ GIMP_PDB_STRING,   "raw_filename", "The name entered" },
	};

	gimp_install_procedure(
		LOAD_PROC,
		"Loads Quite OK Image (QOI) files",
		"Loads Quite OK Image (QOI) files",
		0,
		0,
		DATE,
		"Quite OK Image format",
		"RGB*",
		GIMP_PLUGIN,
		G_N_ELEMENTS(load_args), G_N_ELEMENTS(load_return_vals),
		load_args, load_return_vals
	);
	gimp_register_file_handler_mime(LOAD_PROC, "image/qoi");
	gimp_register_magic_load_handler(LOAD_PROC, "qoi", "", "0,string,qoif");

	gimp_install_procedure(
		SAVE_PROC,
		"Saves Quite OK Image (QOI) files",
		"Saves Quite OK Image (QOI) files",
		0,
		0,
		DATE,
		"Quite OK Image format",
		"RGB*",
		GIMP_PLUGIN,
		G_N_ELEMENTS(save_args), 0,
		save_args, 0
	);
	gimp_register_file_handler_mime(SAVE_PROC, "image/qoi");
	gimp_register_save_handler(SAVE_PROC, "qoi", "");
}

static void run(
	const gchar *name,
	gint nparams, const GimpParam *params,
	gint *nreturn_vals, GimpParam **return_vals
) {
	// Initialize the return code to execution error. This way, the return code
	// only has to change on success, which makes error handling easier.
	static GimpParam values[2] = {
		[0] = { .type = GIMP_PDB_STATUS, .data.d_status = GIMP_PDB_EXECUTION_ERROR, },
	};
	*return_vals = values;
	*nreturn_vals = 1;

	if (strcmp(name, LOAD_PROC) == 0 && nparams >= 2) {
		gchar *filename = params[1].data.d_string;

		QoiImage qoi_image;
		if (load_image(filename, &qoi_image)) {
			gint32 image = create_gimp_image_from_qoi_image(qoi_image, filename);
			if (image != -1) {
				values[0].data.d_status = GIMP_PDB_SUCCESS;
				values[1].type = GIMP_PDB_IMAGE;
				values[1].data.d_image = image;
				*nreturn_vals = 2;
			}
		}

		g_free(qoi_image.pixels);
	} else if (strcmp(name, SAVE_PROC) == 0 && nparams >= 4) {
		GimpRunMode run_mode = params[0].data.d_int32;
		gint32      image    = params[1].data.d_image;
		gint32      drawable = params[2].data.d_drawable;
		gchar      *filename = params[3].data.d_string;

		GimpExportReturn export = GIMP_EXPORT_IGNORE;
		QoiExportOptions options = {
			.export_alpha = true,
			.colorspace = QOI_COLORSPACE_SRGB,
		};

		switch (run_mode) {
			case GIMP_RUN_NONINTERACTIVE: break;
			case GIMP_RUN_WITH_LAST_VALS: {
				gimp_get_data(SAVE_PROC, &options);
			} break;
			case GIMP_RUN_INTERACTIVE: {
				export = show_export_dialog(&image, &drawable, &options);
			} break;
		}

		if (export == GIMP_EXPORT_CANCEL) {
			values[0].data.d_status = GIMP_PDB_CANCEL;
			return;
		}

		QoiImage qoi_image;
		if (get_qoi_image_from_gimp(drawable, options, &qoi_image)) {
			if (save_image(qoi_image, filename)) {
				values[0].data.d_status = GIMP_PDB_SUCCESS;
			}
		}

		g_free(qoi_image.pixels);

		if (export == GIMP_EXPORT_EXPORT) {
			gimp_image_delete(image);
		}
	}
}

static const GimpPlugInInfo PLUG_IN_INFO = {
	0,
	0,
	query,
	run
};

MAIN();
