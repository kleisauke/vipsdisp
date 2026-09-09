/* Smudge a region on an image.
 *
 * Copied from libvips to get a direct draw path,
 */

#include <stdio.h>
#include <stdlib.h>

#include <vips/vips.h>

#define SMUDGE(TYPE) \
	for (int y = 0; y < clip.height; y++) { \
		TYPE *q = (TYPE *) VIPS_IMAGE_ADDR(image, clip.left, clip.top + y); \
		TYPE *p0 = (TYPE *) pixels + y * n_across; \
		TYPE *p1 = p0 + n_across; \
		TYPE *p2 = p1 + n_across; \
\
		for (int x = 0; x < clip.width * bands; x++) { \
			q[x] = (p0[0] +     p0[bands] + p0[2 * bands] + \
					p1[0] + 8 * p1[bands] + p1[2 * bands] + \
					p2[0] +     p2[bands] + p2[2 * bands] + 8) / 16; \
\
			p0 += 1; \
			p1 += 1; \
			p2 += 1; \
		} \
	}

int
draw_smudge(VipsImage *image, VipsRect *area)
{
	/* Double bands for complex images.
	 */
	int bands = image->Bands *
		(vips_band_format_iscomplex(image->BandFmt) ? 2 : 1);
	int esize = VIPS_IMAGE_SIZEOF_ELEMENT(image);

	VipsRect clip = {0, 0, image->Xsize, image->Ysize};
	vips_rect_intersectrect(area, &clip, &clip);
	if (vips_rect_isempty(&clip))
		return 0;

	/* Take a copy of the pixels to blur.
	 */
	int n_across = bands * clip.width;
	g_autofree VipsPel *pixels =
		VIPS_ARRAY(NULL, esize * n_across * clip.height, VipsPel);
	for (int y = 0; y < clip.height; y++)
		VIPS_MEMCPY(pixels + esize * n_across * y,
				VIPS_IMAGE_ADDR(image, clip.left, y + clip.top),
				esize * n_across);

	/* Don't do the margins.
	 */
	vips_rect_marginadjust(&clip, -1);
	if (vips_rect_isempty(&clip))
		return 0;

	switch (vips_image_get_format(image)) {
	case VIPS_FORMAT_UCHAR:
		SMUDGE(unsigned char);
		break;
	case VIPS_FORMAT_CHAR:
		SMUDGE(char);
		break;
	case VIPS_FORMAT_USHORT:
		SMUDGE(unsigned short);
		break;
	case VIPS_FORMAT_SHORT:
		SMUDGE(short);
		break;
	case VIPS_FORMAT_UINT:
		SMUDGE(unsigned int);
		break;
	case VIPS_FORMAT_INT:
		SMUDGE(int);
		break;
	case VIPS_FORMAT_FLOAT:
		SMUDGE(float);
		break;
	case VIPS_FORMAT_DOUBLE:
		SMUDGE(double);
		break;
	case VIPS_FORMAT_COMPLEX:
		SMUDGE(float);
		break;
	case VIPS_FORMAT_DPCOMPLEX:
		SMUDGE(double);
		break;

	default:
		g_assert_not_reached();
	}

	return 0;
}
