#include "vipsdisp.h"

/*
#define DEBUG_VERBOSE
#define DEBUG
 */

/* The size of the checkerboard pattern we use for compositing. Must be a
 * power of two.
 */
#define CHECK_SIZE 16

struct _Imagedisplay {
	GtkDrawingArea parent_instance;

	/* The conversion whose output we display.
	 */
	Conversion *conversion;

	/* We implement a scrollable interface.
	 */
	GtkAdjustment *hadjustment;
	GtkAdjustment *vadjustment;
	guint hscroll_policy;
	guint vscroll_policy;

	/* Our geometry. 
	 *
	 * image_rect is the bounds of image space .. 0,0 to image->Xsize,
	 * image->Ysize
	 *
	 * widget_rect is the bounds of the widget .. 0,0 to GtkAllocation
	 *
	 * paint_rect is the sub-part of the widget that we paint to .. if we
	 * zoom out a long way, for example, we display the image centred in
	 * the widget. 
	 */
	VipsRect image_rect;
	VipsRect widget_rect;
	VipsRect paint_rect;

	/* A backing buffer the size of paint_rect. We use
	 * this from the draw handler to paint the screen, and we also paint to
	 * this from libvips as it calculates pixels.
	 *
	 * This is always Cairo-style ARGB.
	 */
	unsigned char *cairo_buffer;

	/* left/top is the position of the top-left corner of paint_rect within
	 * the image. Set from our adjustments. 
	 */
	int left;
	int top;

	/* The regions we use for fetching pixels from the rgb image and from
	 * the mask.
         *
         * We call it rgb, but it can also be an rgba image, in which case we
         * composite with a checkerboard during paint.
	 */
	VipsRegion *rgb_region;
	VipsRegion *mask_region;

};

/* imagedisplay is actually a drawing area the size of the widget on screen: we 
 * do all scrolling ourselves.
 */
G_DEFINE_TYPE_WITH_CODE( Imagedisplay, imagedisplay, GTK_TYPE_DRAWING_AREA,
	G_IMPLEMENT_INTERFACE( GTK_TYPE_SCROLLABLE, NULL ) );

enum {
	/* Set the conversion with this.
	 */
	PROP_CONVERSION = 1,

	/* The props we implement for the scrollable interface.
	 */
	PROP_HADJUSTMENT,
	PROP_HSCROLL_POLICY,
	PROP_VADJUSTMENT,
	PROP_VSCROLL_POLICY,

	SIG_LAST
};

static void
imagedisplay_adjustment_changed( GtkAdjustment *adjustment, 
	Imagedisplay *imagedisplay )
{
	if( gtk_widget_get_realized( GTK_WIDGET( imagedisplay ) ) ) {
		double left = 
			gtk_adjustment_get_value( imagedisplay->hadjustment );
		double top = 
			gtk_adjustment_get_value( imagedisplay->vadjustment );

#ifdef DEBUG
		printf( "imagedisplay_adjustment_changed: %g x %g\n", 
			left, top ); 
#endif /*DEBUG*/

		imagedisplay->left = left;
		imagedisplay->top = top;

		gtk_widget_queue_draw( GTK_WIDGET( imagedisplay ) ); 
	}
}

static gboolean
imagedisplay_set_adjustment( Imagedisplay *imagedisplay,
	GtkAdjustment **adjustment_slot, GtkAdjustment *new_adjustment )
{
#ifdef DEBUG
	printf( "imagedisplay_set_adjustment:\n" ); 
#endif /*DEBUG*/

	if( new_adjustment && 
		*adjustment_slot == new_adjustment )
		return( FALSE );

	if( *adjustment_slot ) {
		g_signal_handlers_disconnect_by_func( *adjustment_slot,
			    imagedisplay_adjustment_changed, imagedisplay );
		VIPS_UNREF( *adjustment_slot );
	}

	if( !new_adjustment )
		new_adjustment = 
			gtk_adjustment_new( 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 );

	g_signal_connect( new_adjustment, "value-changed",
		G_CALLBACK( imagedisplay_adjustment_changed ), imagedisplay );
	*adjustment_slot = g_object_ref_sink( new_adjustment );

	return( TRUE );
}

static void
imagedisplay_set_adjustment_values( Imagedisplay *imagedisplay, 
	GtkAdjustment *adjustment, int axis_size, int window_size ) 
{
	double old_value;
	double new_value;
	double new_upper;
	double page_size;

#ifdef DEBUG
	printf( "imagedisplay_set_adjustment_values: window_size = %d, "
		"axis_size = %d\n", axis_size, window_size );
#endif /*DEBUG*/

	old_value = gtk_adjustment_get_value( adjustment );
	page_size = window_size;
	new_upper = VIPS_MAX( axis_size, page_size );

	g_object_set( adjustment,
		"lower", 0.0,
		"upper", new_upper,
		"page-size", page_size,
		"step-increment", page_size * 0.1,
		"page-increment", page_size * 0.9,
		NULL );

	new_value = VIPS_CLIP( 0, old_value, new_upper - page_size );
	if( new_value != old_value )
		gtk_adjustment_set_value( adjustment, new_value );
}

static void
imagedisplay_set_hadjustment_values( Imagedisplay *imagedisplay ) 
{
	imagedisplay_set_adjustment_values( imagedisplay, 
		imagedisplay->hadjustment, 
		imagedisplay->image_rect.width, 
		imagedisplay->paint_rect.width ); 
}

static void
imagedisplay_set_vadjustment_values( Imagedisplay *imagedisplay ) 
{
	imagedisplay_set_adjustment_values( imagedisplay, 
		imagedisplay->vadjustment, 
		imagedisplay->image_rect.height, 
		imagedisplay->paint_rect.height ); 
}

/* Copy over any pixels from the old buffer. If the new buffer is larger than 
 * the old one, we tile the old pixels ... it's better than having the screen
 * flash black.
 *
 * FIXME -- we could zoom / shrink / pan the old buffer? Or fill the new one
 * with a grid pattern?
 */
static void
imagedisplay_init_buffer( Imagedisplay *imagedisplay,
	VipsPel *new_buffer, int new_width, int new_height,
	VipsPel *old_buffer, int old_width, int old_height )
{
	int x, y;

	if( !old_buffer ) 
		return;

	for( y = 0; y < new_height; y++ )
		for( x = 0; x < new_width; x += old_width ) {
			int source_y = y % old_height;
			int pixels_to_copy = 
				VIPS_MIN( old_width, new_width - x );

			memcpy( new_buffer + 4 * (y * new_width + x),
				old_buffer + 4 * (source_y * old_width), 
				4 * pixels_to_copy );
		}
}

static void
imagedisplay_layout( Imagedisplay *imagedisplay )
{
	int old_buffer_width = imagedisplay->paint_rect.width;
	int old_buffer_height = imagedisplay->paint_rect.height;

	int buffer_width;
	int buffer_height;

#ifdef DEBUG
	printf( "imagedisplay_layout:\n" );
#endif /*DEBUG*/

	buffer_width = VIPS_MIN( imagedisplay->widget_rect.width,
                imagedisplay->image_rect.width );
	buffer_height = VIPS_MIN( imagedisplay->widget_rect.height,
                imagedisplay->image_rect.height );

	/* If the image is smaller than the widget, centre it.
	 */
	imagedisplay->paint_rect.width = buffer_width;
	imagedisplay->paint_rect.height = buffer_height;
	imagedisplay->paint_rect.left = VIPS_MAX( 0,
		(imagedisplay->widget_rect.width - buffer_width) / 2 ); 
	imagedisplay->paint_rect.top = VIPS_MAX( 0,
		(imagedisplay->widget_rect.height - buffer_height) / 2 ); 

	imagedisplay_set_hadjustment_values( imagedisplay );
	imagedisplay_set_vadjustment_values( imagedisplay );

	/* Reallocate the backing buffer, if necessary.
	 */
	if( !imagedisplay->cairo_buffer ||
		imagedisplay->paint_rect.width != old_buffer_width ||
		imagedisplay->paint_rect.height != old_buffer_height ) {
		unsigned char *new_buffer = g_malloc0( 
			imagedisplay->paint_rect.width * 
			imagedisplay->paint_rect.height * 4 );

		/* Fill the new buffer with default pixels somehow.
		 */
		imagedisplay_init_buffer( imagedisplay,
			new_buffer, buffer_width, buffer_height,
			imagedisplay->cairo_buffer, 
				old_buffer_width, old_buffer_height );
		g_free( imagedisplay->cairo_buffer );
		imagedisplay->cairo_buffer = new_buffer;
	}
}

/* New display image, so we need new mask and RGB regions.
 */
static void
imagedisplay_conversion_display_changed( Conversion *conversion, 
	Imagedisplay *imagedisplay ) 
{
#ifdef DEBUG
	printf( "imagedisplay_conversion_display_changed:\n" ); 
#endif /*DEBUG*/

	VIPS_UNREF( imagedisplay->mask_region );
	VIPS_UNREF( imagedisplay->rgb_region );

	imagedisplay->rgb_region = vips_region_new( conversion->rgb );
	imagedisplay->mask_region = vips_region_new( conversion->mask );

	imagedisplay->image_rect.width = conversion->rgb->Xsize;
	imagedisplay->image_rect.height = conversion->rgb->Ysize;

	imagedisplay_layout( imagedisplay );

	gtk_widget_queue_draw( GTK_WIDGET( imagedisplay ) ); 
}

static void
imagedisplay_conversion_area_changed( Conversion *conversion, VipsRect *dirty, 
	Imagedisplay *imagedisplay ) 
{
#ifdef DEBUG_VERBOSE
	printf( "imagedisplay_conversion_area_changed: "
		"left = %d, top = %d, width = %d, height = %d\n",
		dirty->left, dirty->top,
		dirty->width, dirty->height );
#endif /*DEBUG_VERBOSE*/

	/* Sadly, gtk4 only has this and we can't redraw areas.
	 */
	gtk_widget_queue_draw( GTK_WIDGET( imagedisplay ) );
}

static void
imagedisplay_set_conversion( Imagedisplay *imagedisplay, 
	Conversion *conversion )
{
	g_assert( !imagedisplay->conversion );

	imagedisplay->conversion = conversion;
	g_object_ref( imagedisplay->conversion );

	g_signal_connect_object( conversion, "display-changed", 
		G_CALLBACK( imagedisplay_conversion_display_changed ), 
		imagedisplay, 0 );
	g_signal_connect_object( conversion, "area-changed", 
		G_CALLBACK( imagedisplay_conversion_area_changed ), 
		imagedisplay, 0 );
}

static void
imagedisplay_set_property( GObject *object, 
	guint prop_id, const GValue *value, GParamSpec *pspec )
{
	Imagedisplay *imagedisplay = (Imagedisplay *) object;

	switch( prop_id ) {
	case PROP_HADJUSTMENT:
		if( imagedisplay_set_adjustment( imagedisplay, 
			&imagedisplay->hadjustment, 
			g_value_get_object( value ) ) ) { 
			imagedisplay_set_hadjustment_values( imagedisplay );
			g_object_notify( G_OBJECT( imagedisplay ), 
				"hadjustment" );
		}
		break;

	case PROP_VADJUSTMENT:
		if( imagedisplay_set_adjustment( imagedisplay, 
			&imagedisplay->vadjustment, 
			g_value_get_object( value ) ) ) { 
			imagedisplay_set_vadjustment_values( imagedisplay );
			g_object_notify( G_OBJECT( imagedisplay ), 
				"vadjustment" );
		}
		break;

	case PROP_HSCROLL_POLICY:
		if( imagedisplay->hscroll_policy != 
			g_value_get_enum( value ) ) {
			imagedisplay->hscroll_policy = 
				g_value_get_enum( value );
			gtk_widget_queue_resize( GTK_WIDGET( imagedisplay ) );
			g_object_notify_by_pspec( object, pspec );
		}
		break;

	case PROP_VSCROLL_POLICY:
		if( imagedisplay->vscroll_policy != 
			g_value_get_enum( value ) ) {
			imagedisplay->vscroll_policy = 
				g_value_get_enum( value );
			gtk_widget_queue_resize( GTK_WIDGET( imagedisplay ) );
			g_object_notify_by_pspec( object, pspec );
		}
		break;

	case PROP_CONVERSION:
		imagedisplay_set_conversion( imagedisplay, 
			g_value_get_object( value ) );
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID( object, prop_id, pspec );
		break;
	}
}

static void
imagedisplay_get_property( GObject *object, 
	guint prop_id, GValue *value, GParamSpec *pspec )
{
	Imagedisplay *imagedisplay = (Imagedisplay *) object;

	switch( prop_id ) {
	case PROP_HADJUSTMENT:
		g_value_set_object( value, imagedisplay->hadjustment );
		break;

	case PROP_VADJUSTMENT:
		g_value_set_object( value, imagedisplay->vadjustment );
		break;

	case PROP_HSCROLL_POLICY:
		g_value_set_enum( value, imagedisplay->hscroll_policy );
		break;

	case PROP_VSCROLL_POLICY:
		g_value_set_enum( value, imagedisplay->vscroll_policy );
		break;

	case PROP_CONVERSION:
		g_value_set_object( value, imagedisplay->conversion );
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID( object, prop_id, pspec );
		break;
	}
}

static void
imagedisplay_dispose( GObject *object )
{
	Imagedisplay *imagedisplay = (Imagedisplay *) object;

#ifdef DEBUG
	printf( "imagedisplay_dispose:\n" ); 
#endif /*DEBUG*/

	VIPS_UNREF( imagedisplay->rgb_region );
	VIPS_UNREF( imagedisplay->mask_region );
	VIPS_UNREF( imagedisplay->conversion );
	VIPS_FREE( imagedisplay->cairo_buffer ); 

	G_OBJECT_CLASS( imagedisplay_parent_class )->dispose( object );
}

/* Convert libvips RGBA to cairo premultiplied ABGR.
 */
static void
imagedisplay_rgba_to_cairo( Imagedisplay *imagedisplay, 
	unsigned char *cairo, VipsPel *vips, 
	int width, int height,
	int cairo_stride, int vips_stride )
{
	int x, y;

	for( y = 0; y < height; y++ ) {
		VipsPel * restrict p = vips + y * vips_stride;
		unsigned char * restrict q = cairo + y * cairo_stride;

		for( x = 0; x < width; x++ ) {
			int r = p[0];
			int g = p[1];
			int b = p[2];
			int a = p[3];

			q[0] = b * a / 255;
			q[1] = g * a / 255;
			q[2] = r * a / 255;
			q[3] = a;

			p += 4;
			q += 4;
		}
	}
}

/* libvips RGB, to cairo premultiplied ABGR.
 */
static void
imagedisplay_rgb_to_cairo( Imagedisplay *imagedisplay, 
	unsigned char *cairo, VipsPel *vips, 
	int width, int height,
	int cairo_stride, int vips_stride )
{
	int x, y;

	for( y = 0; y < height; y++ ) {
		VipsPel * restrict p = vips + y * vips_stride;
		unsigned char * restrict q = cairo + y * cairo_stride;

		for( x = 0; x < width; x++ ) {
			int r = p[0];
			int g = p[1];
			int b = p[2];

			q[0] = b;
			q[1] = g;
			q[2] = r;
			q[3] = 255;

			p += 3;
			q += 4;
		}
	}
}

/* Transform between our coordinate spaces:
 *
 * image: libvips coordinates ... we clip against the image size when going to 
 * libvips space.
 *
 * gtk: coordinates for gtk and for cairo drawing .. no clipping.
 *
 * buffer: coordinates for our backing buffer ... clip against buffer size when
 * going to this space.
 */

void
imagedisplay_image_to_gtk( Imagedisplay *imagedisplay, VipsRect *rect )
{
	rect->left -= imagedisplay->left;
	rect->top -= imagedisplay->top;

	rect->left += imagedisplay->paint_rect.left;
	rect->top += imagedisplay->paint_rect.top;
}

static void
imagedisplay_gtk_to_buffer( Imagedisplay *imagedisplay, VipsRect *rect )
{
	vips_rect_intersectrect( rect, &imagedisplay->paint_rect, rect ); 

	rect->left -= imagedisplay->paint_rect.left;
	rect->top -= imagedisplay->paint_rect.top;
}

static void
imagedisplay_buffer_to_gtk( Imagedisplay *imagedisplay, VipsRect *rect )
{
	rect->left += imagedisplay->paint_rect.left;
	rect->top += imagedisplay->paint_rect.top;
}

void
imagedisplay_gtk_to_image( Imagedisplay *imagedisplay, VipsRect *rect )
{
	rect->left -= imagedisplay->paint_rect.left;
	rect->top -= imagedisplay->paint_rect.top;

	rect->left += imagedisplay->left;
	rect->top += imagedisplay->top;

	vips_rect_intersectrect( rect, &imagedisplay->image_rect, rect ); 
}

static void
imagedisplay_image_to_buffer( Imagedisplay *imagedisplay, VipsRect *rect )
{
	imagedisplay_image_to_gtk( imagedisplay, rect );
	imagedisplay_gtk_to_buffer( imagedisplay, rect );
}

static void
imagedisplay_buffer_to_image( Imagedisplay *imagedisplay, VipsRect *rect )
{
	imagedisplay_buffer_to_gtk( imagedisplay, rect );
	imagedisplay_gtk_to_image( imagedisplay, rect );
}

/* Fill a single tile from libvips. The tile fits within a single tile 
 * cache entry, and within the image.
 */
static void
imagedisplay_fill_tile( Imagedisplay *imagedisplay, VipsRect *tile )
{
	VipsRect target;
	VipsRect clip;

	/* Must fit in a single tile, and within the image.
	 */
	g_assert( tile->width <= TILE_SIZE );
	g_assert( tile->height <= TILE_SIZE );
	g_assert( VIPS_ROUND_UP( tile->left, TILE_SIZE ) - 
		VIPS_ROUND_DOWN( VIPS_RECT_RIGHT( tile ), TILE_SIZE ) <= 
			TILE_SIZE );
	g_assert( VIPS_ROUND_UP( tile->top, TILE_SIZE ) - 
		VIPS_ROUND_DOWN( VIPS_RECT_BOTTOM( tile ), TILE_SIZE ) <= 
			TILE_SIZE );
	g_assert( vips_rect_includesrect( &imagedisplay->image_rect, tile ) ); 

	/* Map into buffer space and clip.
	 */
	target = *tile;
	imagedisplay_image_to_buffer( imagedisplay, &target );
	clip = target;
	imagedisplay_buffer_to_image( imagedisplay, &clip ); 
	if( vips_rect_isempty( &clip ) )
		return;

#ifdef DEBUG_VERBOSE
	printf( "imagedisplay_fill_tile: vips computes "
		"left = %d, top = %d, width = %d, height = %d\n",
		clip.left, clip.top,
		clip.width, clip.height );
#endif /*DEBUG_VERBOSE*/

	/* Request pixels. We ask the mask first, to get an idea of what's 
	 * currently in cache, then request tiles of pixels. We must always
	 * request pixels, even if the mask is blank, because the request
	 * will trigger a notify later which will reinvoke us.
	 */
	if( vips_region_prepare( imagedisplay->mask_region, &clip ) ) {
#ifdef DEBUG_VERBOSE
		printf( "vips_region_prepare: %s\n", vips_error_buffer() ); 
		vips_error_clear();
		abort();
#endif /*DEBUG_VERBOSE*/

		return;
	}

	if( vips_region_prepare( imagedisplay->rgb_region, &clip ) ) {
#ifdef DEBUG_VERBOSE
		printf( "vips_region_prepare: %s\n", vips_error_buffer() ); 
		vips_error_clear();
		abort();
#endif /*DEBUG_VERBOSE*/

		return;
	}

	/* tile is within a single tile, so we only need to test the first byte
	 * of the mask. 
	 */
	if( VIPS_REGION_ADDR( imagedisplay->mask_region, 
		clip.left, clip.top )[0] ) {
		int cairo_stride = 4 * imagedisplay->paint_rect.width;
		unsigned char *cairo_start = imagedisplay->cairo_buffer +
			target.top * cairo_stride + target.left * 4;
                int vips_stride = VIPS_REGION_LSKIP( imagedisplay->rgb_region );
                VipsPel *vips_start = 
                        VIPS_REGION_ADDR( imagedisplay->rgb_region, 
				clip.left, clip.top );

#ifdef DEBUG_VERBOSE
		printf( "imagedisplay_fill_tile: copy to backing buffer "
			"left = %d, top = %d, width = %d, height = %d\n",
			clip.left, clip.top,
			clip.width, clip.height );
#endif /*DEBUG_VERBOSE*/

                if( imagedisplay->rgb_region->im->Bands == 4 )
                        imagedisplay_rgba_to_cairo( imagedisplay, 
                                cairo_start, vips_start,
                                clip.width, clip.height,
                                cairo_stride, vips_stride );
                else
                        imagedisplay_rgb_to_cairo( imagedisplay, 
                                cairo_start, vips_start,
                                clip.width, clip.height,
                                cairo_stride, vips_stride );
	}
}

/* Fill a rectangle with a set of libvips tiles.
 *
 * render processes tiles in FIFO order, so we need to add in reverse order
 * of processing. We want repaint to happen in a spiral from the centre out,
 * so we have to add in a spiral from the outside in.
 */
static void
imagedisplay_fill_rect( Imagedisplay *imagedisplay, VipsRect *expose )
{
	int left, top, right, bottom;

	left = VIPS_ROUND_DOWN( expose->left, TILE_SIZE );
	top = VIPS_ROUND_DOWN( expose->top, TILE_SIZE );
	right = VIPS_ROUND_UP( VIPS_RECT_RIGHT( expose ), TILE_SIZE );
	bottom = VIPS_ROUND_UP( VIPS_RECT_BOTTOM( expose ), TILE_SIZE );

	/* Do the four edges, then step in. Loop until the centre is empty.
	 */
	for(;;) {
		VipsRect tile;
		VipsRect clip;
		int x, y;

		tile.width = TILE_SIZE;
		tile.height = TILE_SIZE;

		if( right - left <= 0 ||
			bottom - top <= 0 )
			break;

		/* Top edge.
		 */
		for( x = left; x < right; x += TILE_SIZE ) {
			tile.left = x;
			tile.top = top;
			vips_rect_intersectrect( &tile, expose, &clip );
			if( !vips_rect_isempty( &clip ) )
				imagedisplay_fill_tile( imagedisplay, &clip );
		}
		top += TILE_SIZE;

		if( right - left <= 0 ||
			bottom - top <= 0 )
			break;

		/* Bottom edge.
		 */
		for( x = left; x < right; x += TILE_SIZE ) {
			tile.left = x;
			tile.top = bottom - TILE_SIZE;
			vips_rect_intersectrect( &tile, expose, &clip );
			if( !vips_rect_isempty( &clip ) )
				imagedisplay_fill_tile( imagedisplay, &clip );
		}
		bottom -= TILE_SIZE;

		if( right - left <= 0 ||
			bottom - top <= 0 )
			break;

		/* Left edge.
		 */
		for( y = top; y < bottom; y += TILE_SIZE ) {
			tile.left = left;
			tile.top = y;
			vips_rect_intersectrect( &tile, expose, &clip );
			if( !vips_rect_isempty( &clip ) )
				imagedisplay_fill_tile( imagedisplay, &clip );
		}
		left += TILE_SIZE;

		if( right - left <= 0 ||
			bottom - top <= 0 )
			break;

		/* Right edge.
		 */
		for( y = top; y < bottom; y += TILE_SIZE ) {
			tile.left = right - TILE_SIZE;
			tile.top = y;
			vips_rect_intersectrect( &tile, expose, &clip );
			if( !vips_rect_isempty( &clip ) )
				imagedisplay_fill_tile( imagedisplay, &clip );
		}
		right -= TILE_SIZE;

		if( right - left <= 0 ||
			bottom - top <= 0 )
			break;
	}
}

/* Fill the given area with checks in the standard style for showing 
 * compositing effects.
 *
 * It would make sense to do this as a repeating surface, but most 
 * implementations of RENDER currently have broken implementations of 
 * repeat + transform, even when the transform is a translation.
 *
 * Copied from gtk4-demo/drawingarea.c
 */
static void
imagedisplay_fill_checks( cairo_t *cr, 
	int left, int top, int width, int height )
{
	int right = left + width;
	int bottom = top + height;

	int x, y;

	cairo_rectangle( cr, left, top, width, height );
	cairo_set_source_rgb( cr, 0.4, 0.4, 0.4 );
	cairo_fill( cr );

	/* Only works for CHECK_SIZE a power of 2 
	 */
	for( x = left & (-CHECK_SIZE); x < right; x += CHECK_SIZE ) 
		for( y = top & (-CHECK_SIZE); y < bottom; y += CHECK_SIZE )
			if( (x / CHECK_SIZE + y / CHECK_SIZE) % 2 == 0 )
				cairo_rectangle( cr, 
					x, y, CHECK_SIZE, CHECK_SIZE );

	cairo_set_source_rgb( cr, 0.7, 0.7, 0.7 );
	cairo_fill( cr );
}

/* Draw a rectangle of the image from the backing buffer.
 */
static void
imagedisplay_draw_cairo( Imagedisplay *imagedisplay, 
	cairo_t *cr, VipsRect *expose )
{
	VipsRect buffer;
	VipsRect gtk;

	gtk = *expose;
	imagedisplay_image_to_gtk( imagedisplay, &gtk );
	buffer = gtk;
	imagedisplay_gtk_to_buffer( imagedisplay, &buffer );

	if( !vips_rect_isempty( &buffer ) ) {
		int cairo_stride = 4 * imagedisplay->paint_rect.width;
		unsigned char *cairo_start = imagedisplay->cairo_buffer +
			buffer.top * cairo_stride + buffer.left * 4;
		cairo_surface_t *surface;

#ifdef DEBUG_VERBOSE
		printf( "imagedisplay_draw_cairo: drawing "
			"left = %d, top = %d, width = %d, height = %d\n",
			gtk.left, gtk.top,
			buffer.width, buffer.height );
#endif /*DEBUG_VERBOSE*/

                surface = cairo_image_surface_create_for_data( 
                        cairo_start, CAIRO_FORMAT_ARGB32, 
                        buffer.width, buffer.height, 
                        cairo_stride );  

                if( imagedisplay->rgb_region->im->Bands == 4 ) {
                        /* Clip to the image area to stop the checkerboard
                         * overpainting.
                         */
                        cairo_rectangle( cr, 
                                gtk.left, gtk.top, gtk.width, gtk.height );
                        cairo_clip( cr );

                        /* Paint background checkerboard.
                         */
                        imagedisplay_fill_checks( cr, 
                                gtk.left, gtk.top, gtk.width, gtk.height );

                        cairo_set_source_surface( cr, 
                                surface, gtk.left, gtk.top ); 
                        cairo_set_operator( cr, CAIRO_OPERATOR_OVER );
                }
                else {
                        cairo_set_source_surface( cr, 
                                surface, gtk.left, gtk.top ); 
                }

                cairo_paint( cr );

                cairo_surface_destroy( surface ); 
	}
}

static void
imagedisplay_draw( GtkDrawingArea *area, 
	cairo_t *cr, int width, int height, gpointer user_data )
{
	Imagedisplay *imagedisplay = (Imagedisplay *) area;
	GtkWidget *widget = GTK_WIDGET( imagedisplay );

	if( imagedisplay->conversion->loaded && 
		imagedisplay->rgb_region ) {
		cairo_rectangle_list_t *rectangle_list = 
			cairo_copy_clip_rectangle_list( cr );

		if( rectangle_list->status == CAIRO_STATUS_SUCCESS ) { 
			int i;

			for( i = 0; i < rectangle_list->num_rectangles; i++ ) {
				cairo_rectangle_t *rectangle = 
					&rectangle_list->rectangles[i];
				VipsRect expose;

				/* Turn into an image-space rect.
				 */
				expose.left = rectangle->x;
				expose.top = rectangle->y;
				expose.width = rectangle->width;
				expose.height = rectangle->height;
				imagedisplay_gtk_to_image( imagedisplay, 
					&expose ); 

				if( !vips_rect_isempty( &expose ) ) {
					imagedisplay_fill_rect( imagedisplay, 
						&expose );
					imagedisplay_draw_cairo( imagedisplay, 
						cr, &expose );
				}
			}
		}

		cairo_rectangle_list_destroy( rectangle_list );
	}

	if( gtk_widget_has_focus( widget ) ) {
		/* It'd be great to get the colour and style from the theme,
		 * somehow.
		 */
		cairo_set_source_rgb( cr, 0.5, 0.5, 1 );
		cairo_set_line_width( cr, 1 );
		cairo_rectangle( cr, 3, 3,
			gtk_widget_get_allocated_width( widget ) - 6,
			gtk_widget_get_allocated_height( widget ) - 6 );
		cairo_stroke( cr );
	}
}

static void
imagedisplay_resize( GtkWidget *widget, int width, int height )
{
	Imagedisplay *imagedisplay = (Imagedisplay *) widget;

#ifdef DEBUG
	printf( "imagedisplay_resize: %d x %d\n",
		width, height ); 
#endif /*DEBUG*/

	imagedisplay->widget_rect.width = width;
	imagedisplay->widget_rect.height = height;
	imagedisplay_layout( imagedisplay );
}

static void
imagedisplay_focus_enter( GtkEventController *controller, 
	Imagedisplay *imagedisplay )
{
	gtk_widget_queue_draw( GTK_WIDGET( imagedisplay ) ); 
}

static void
imagedisplay_focus_leave( GtkEventController *controller, 
	Imagedisplay *imagedisplay )
{
	gtk_widget_queue_draw( GTK_WIDGET( imagedisplay ) ); 
}

static void 
imagedisplay_click( GtkEventController *controller, 
	int n_press, double x, double y, Imagedisplay *imagedisplay )
{
	gtk_widget_grab_focus( GTK_WIDGET( imagedisplay ) );
}

static void
imagedisplay_init( Imagedisplay *imagedisplay )
{
	GtkEventController *controller;

#ifdef DEBUG
	printf( "imagedisplay_init:\n" ); 
#endif /*DEBUG*/

	gtk_widget_set_focusable( GTK_WIDGET( imagedisplay ), TRUE );
	gtk_drawing_area_set_draw_func( GTK_DRAWING_AREA( imagedisplay ),
		imagedisplay_draw, NULL, NULL );
	g_signal_connect( GTK_DRAWING_AREA( imagedisplay ), "resize",
		G_CALLBACK( imagedisplay_resize ), NULL);

	controller = gtk_event_controller_focus_new();
	g_signal_connect( controller, "enter", 
		G_CALLBACK( imagedisplay_focus_enter ), imagedisplay );
	g_signal_connect( controller, "leave", 
		G_CALLBACK( imagedisplay_focus_leave ), imagedisplay );
	gtk_widget_add_controller( GTK_WIDGET( imagedisplay ), controller );

	controller = GTK_EVENT_CONTROLLER( gtk_gesture_click_new() );
	g_signal_connect( controller, "pressed", 
		G_CALLBACK( imagedisplay_click ), imagedisplay );
	gtk_widget_add_controller( GTK_WIDGET( imagedisplay ), controller );
}

static void
imagedisplay_class_init( ImagedisplayClass *class )
{
	GObjectClass *gobject_class = G_OBJECT_CLASS( class );

#ifdef DEBUG
	printf( "imagedisplay_class_init:\n" ); 
#endif /*DEBUG*/

	gobject_class->dispose = imagedisplay_dispose;
	gobject_class->set_property = imagedisplay_set_property;
	gobject_class->get_property = imagedisplay_get_property;

	g_object_class_install_property( gobject_class, PROP_CONVERSION,
		g_param_spec_object( "conversion",
			_( "conversion" ),
			_( "The conversion to be displayed" ),
			TYPE_CONVERSION,
			G_PARAM_READWRITE ) );

	g_object_class_override_property( gobject_class, 
		PROP_HADJUSTMENT, "hadjustment" );
	g_object_class_override_property( gobject_class, 
		PROP_VADJUSTMENT, "vadjustment" );
	g_object_class_override_property( gobject_class, 
		PROP_HSCROLL_POLICY, "hscroll-policy" );
	g_object_class_override_property( gobject_class, 
		PROP_VSCROLL_POLICY, "vscroll-policy" );
}

Imagedisplay *
imagedisplay_new( Conversion *conversion ) 
{
	Imagedisplay *imagedisplay;

#ifdef DEBUG
	printf( "imagedisplay_new:\n" ); 
#endif /*DEBUG*/

	imagedisplay = g_object_new( imagedisplay_get_type(),
		"conversion", conversion,
		NULL );

	return( imagedisplay ); 
}