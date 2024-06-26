/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*  GMime
 *  Copyright (C) 2000-2022 Jeffrey Stedfast
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2.1
 *  of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free
 *  Software Foundation, 51 Franklin Street, Fifth Floor, Boston, MA
 *  02110-1301, USA.
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include "gmime-filter-yenc.h"


/**
 * SECTION: gmime-filter-yenc
 * @title: GMimeFilterYenc
 * @short_description: yEncode or yDecode
 * @see_also: #GMimeFilter
 *
 * A #GMimeFilter used to encode or decode the Usenet yEncoding.
 **/


static void g_mime_filter_yenc_class_init (GMimeFilterYencClass *klass);
static void g_mime_filter_yenc_init (GMimeFilterYenc *filter, GMimeFilterYencClass *klass);
static void g_mime_filter_yenc_finalize (GObject *object);

static GMimeFilter *filter_copy (GMimeFilter *filter);
static void filter_filter (GMimeFilter *filter, char *in, size_t len, size_t prespace,
			   char **out, size_t *outlen, size_t *outprespace);
static void filter_complete (GMimeFilter *filter, char *in, size_t len, size_t prespace,
			     char **out, size_t *outlen, size_t *outprespace);
static void filter_reset (GMimeFilter *filter);


static GMimeFilterClass *parent_class = NULL;


GType
g_mime_filter_yenc_get_type (void)
{
	static GType type = 0;
	
	if (!type) {
		static const GTypeInfo info = {
			sizeof (GMimeFilterYencClass),
			NULL, /* base_class_init */
			NULL, /* base_class_finalize */
			(GClassInitFunc) g_mime_filter_yenc_class_init,
			NULL, /* class_finalize */
			NULL, /* class_data */
			sizeof (GMimeFilterYenc),
			0,    /* n_preallocs */
			(GInstanceInitFunc) g_mime_filter_yenc_init,
		};
		
		type = g_type_register_static (GMIME_TYPE_FILTER, "GMimeFilterYenc", &info, 0);
	}
	
	return type;
}


static void
g_mime_filter_yenc_class_init (GMimeFilterYencClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GMimeFilterClass *filter_class = GMIME_FILTER_CLASS (klass);
	
	parent_class = g_type_class_ref (GMIME_TYPE_FILTER);
	
	object_class->finalize = g_mime_filter_yenc_finalize;
	
	filter_class->copy = filter_copy;
	filter_class->filter = filter_filter;
	filter_class->complete = filter_complete;
	filter_class->reset = filter_reset;
}

static void
g_mime_filter_yenc_init (GMimeFilterYenc *filter, GMimeFilterYencClass *klass)
{
	filter->part = 0;
	filter->pcrc = GMIME_YENCODE_CRC_INIT;
	filter->crc = GMIME_YENCODE_CRC_INIT;
}

static void
g_mime_filter_yenc_finalize (GObject *object)
{
	G_OBJECT_CLASS (parent_class)->finalize (object);
}


static GMimeFilter *
filter_copy (GMimeFilter *filter)
{
	GMimeFilterYenc *yenc = (GMimeFilterYenc *) filter;
	
	return g_mime_filter_yenc_new (yenc->encode);
}

/* here we do all of the basic yEnc filtering */
static void
filter_filter (GMimeFilter *filter, char *in, size_t len, size_t prespace,
	       char **out, size_t *outlen, size_t *outprespace)
{
	GMimeFilterYenc *yenc = (GMimeFilterYenc *) filter;
	const unsigned char *inbuf;
	unsigned char *outbuf;
	size_t newlen = 0;
	
	if (yenc->encode) {
		/* won't go to more than 2 * (x + 2) + 62 */
		g_mime_filter_set_size (filter, (len + 2) * 2 + 62, FALSE);
		outbuf = (unsigned char *) filter->outbuf;
		inbuf = (const unsigned char *) in;
		newlen = g_mime_yencode_step (inbuf, len, outbuf, &yenc->state,
					      &yenc->pcrc, &yenc->crc);
		g_assert (newlen <= (len + 2) * 2 + 62);
	} else {
		if (!(yenc->state & GMIME_YDECODE_STATE_DECODE)) {
			register char *inptr, *inend;
			size_t left;
			
			inptr = in;
			inend = inptr + len;
			
			/* we cannot start decoding until we have found an =ybegin line */
			if (!(yenc->state & GMIME_YDECODE_STATE_BEGIN)) {
				while (inptr < inend) {
					left = inend - inptr;
					if (left < 8) {
						if (!strncmp (inptr, "=ybegin ", left))
							g_mime_filter_backup (filter, inptr, left);
						break;
					} else if (!strncmp (inptr, "=ybegin ", 8)) {
						for (in = inptr; inptr < inend && *inptr != '\n'; inptr++);
						if (inptr < inend) {
							inptr++;
							yenc->state |= GMIME_YDECODE_STATE_BEGIN;
							/* we can start ydecoding if the next line isn't
							   a ypart... */
							in = inptr;
							len = inend - in;
						} else {
							/* we don't have enough... */
							g_mime_filter_backup (filter, in, left);
						}
						break;
					}
				
					/* go to the next line */
					while (inptr < inend && *inptr != '\n')
						inptr++;
					
					if (inptr < inend)
						inptr++;
				}
			}
			
			left = inend - inptr;
			if ((yenc->state & GMIME_YDECODE_STATE_BEGIN) && left > 0) {
				/* we have found an '=ybegin' line but we may yet have an "=ypart" line to
				   yield before decoding the content */
				if (left < 7 && !strncmp (inptr, "=ypart ", left)) {
					g_mime_filter_backup (filter, inptr, left);
				} else if (!strncmp (inptr, "=ypart ", 7)) {
					for (in = inptr; inptr < inend && *inptr != '\n'; inptr++);
					if (inptr < inend) {
						inptr++;
						yenc->state |= GMIME_YDECODE_STATE_PART | GMIME_YDECODE_STATE_DECODE;
						in = inptr;
						len = inend - in;
					} else {
						g_mime_filter_backup (filter, in, left);
					}
				} else {
					/* guess it doesn't have a =ypart line */
					yenc->state |= GMIME_YDECODE_STATE_DECODE;
				}
			}
		}
		
		if ((yenc->state & GMIME_YDECODE_STATE_DECODE) && !(yenc->state & GMIME_YDECODE_STATE_END)) {
			/* all yEnc headers have been found so we can now start decoding */
			g_mime_filter_set_size (filter, len + 3, FALSE);
			outbuf = (unsigned char *) filter->outbuf;
			inbuf = (const unsigned char *) in;
			newlen = g_mime_ydecode_step (inbuf, len, outbuf, &yenc->state,
						      &yenc->pcrc, &yenc->crc);
			g_assert (newlen <= len + 3);
		} else {
			newlen = 0;
		}
	}
	
	*outprespace = filter->outpre;
	*out = filter->outbuf;
	*outlen = newlen;
}

static void
filter_complete (GMimeFilter *filter, char *in, size_t len, size_t prespace,
		 char **out, size_t *outlen, size_t *outprespace)
{
	GMimeFilterYenc *yenc = (GMimeFilterYenc *) filter;
	const unsigned char *inbuf;
	unsigned char *outbuf;
	size_t newlen = 0;
	
	if (yenc->encode) {
		/* won't go to more than 2 * (x + 2) + 62 */
		g_mime_filter_set_size (filter, (len + 2) * 2 + 62, FALSE);
		outbuf = (unsigned char *) filter->outbuf;
		inbuf = (const unsigned char *) in;
		newlen = g_mime_yencode_close (inbuf, len, outbuf, &yenc->state,
					       &yenc->pcrc, &yenc->crc);
		g_assert (newlen <= (len + 2) * 2 + 62);
	} else {
		if ((yenc->state & GMIME_YDECODE_STATE_DECODE) && !(yenc->state & GMIME_YDECODE_STATE_END)) {
			/* all yEnc headers have been found so we can now start decoding */
			g_mime_filter_set_size (filter, len + 3, FALSE);
			outbuf = (unsigned char *) filter->outbuf;
			inbuf = (const unsigned char *) in;
			newlen = g_mime_ydecode_step (inbuf, len, outbuf, &yenc->state,
						      &yenc->pcrc, &yenc->crc);
			g_assert (newlen <= len + 3);
		} else {
			newlen = 0;
		}
	}
	
	*outprespace = filter->outpre;
	*out = filter->outbuf;
	*outlen = newlen;
}

/* should this 'flush' outstanding state/data bytes? */
static void
filter_reset (GMimeFilter *filter)
{
	GMimeFilterYenc *yenc = (GMimeFilterYenc *) filter;
	
	if (yenc->encode)
		yenc->state = GMIME_YENCODE_STATE_INIT;
	else
		yenc->state = GMIME_YDECODE_STATE_INIT;
	
	yenc->pcrc = GMIME_YENCODE_CRC_INIT;
	yenc->crc = GMIME_YENCODE_CRC_INIT;
}


/**
 * g_mime_filter_yenc_new:
 * @encode: encode vs decode
 *
 * Creates a new yEnc filter.
 *
 * Returns: a new yEnc filter.
 **/
GMimeFilter *
g_mime_filter_yenc_new (gboolean encode)
{
	GMimeFilterYenc *new;
	
	new = g_object_new (GMIME_TYPE_FILTER_YENC, NULL);
	new->encode = encode;
	
	if (encode)
		new->state = GMIME_YENCODE_STATE_INIT;
	else
		new->state = GMIME_YDECODE_STATE_INIT;
	
	return (GMimeFilter *) new;
}


/**
 * g_mime_filter_yenc_set_state:
 * @yenc: yEnc filter
 * @state: encode/decode state
 *
 * Sets the current state of the yencoder/ydecoder
 **/
void
g_mime_filter_yenc_set_state (GMimeFilterYenc *yenc, int state)
{
	g_return_if_fail (GMIME_IS_FILTER_YENC (yenc));
	
	yenc->state = state;
}


/**
 * g_mime_filter_yenc_set_crc:
 * @yenc: yEnc filter
 * @crc: crc32
 *
 * Sets the current crc32 value on the yEnc filter @yenc to @crc.
 **/
void
g_mime_filter_yenc_set_crc (GMimeFilterYenc *yenc, guint32 crc)
{
	g_return_if_fail (GMIME_IS_FILTER_YENC (yenc));
	
	yenc->crc = crc;
}


#if 0
/* FIXME: once we parse out the yenc part id, we can re-enable this interface */
/**
 * g_mime_filter_yenc_get_part:
 * @yenc: yEnc filter
 *
 * Gets the part id of the current decoded yEnc stream or %-1 on fail.
 *
 * Returns: the part id of the current decoded yEnc stream or %-1 on
 * fail.
 **/
int
g_mime_filter_yenc_get_part (GMimeFilterYenc *yenc)
{
	g_return_val_if_fail (GMIME_IS_FILTER_YENC (yenc), -1);
	
	if (yenc->state & GMIME_YDECODE_STATE_PART)
		return yenc->part;
	
	return -1;
}
#endif

/**
 * g_mime_filter_yenc_get_pcrc:
 * @yenc: yEnc filter
 *
 * Get the computed part crc or (guint32) -1 on fail.
 *
 * Returns: the computed part crc or (guint32) -1 on fail.
 **/
guint32
g_mime_filter_yenc_get_pcrc (GMimeFilterYenc *yenc)
{
	g_return_val_if_fail (GMIME_IS_FILTER_YENC (yenc), -1);
	
	return GMIME_YENCODE_CRC_FINAL (yenc->pcrc);
}


/**
 * g_mime_filter_yenc_get_crc:
 * @yenc: yEnc filter
 *
 * Get the computed crc or (guint32) -1 on fail.
 *
 * Returns: the computed crc or (guint32) -1 on fail.
 **/
guint32
g_mime_filter_yenc_get_crc (GMimeFilterYenc *yenc)
{
	g_return_val_if_fail (GMIME_IS_FILTER_YENC (yenc), -1);
	
	return GMIME_YENCODE_CRC_FINAL (yenc->crc);
}


static const int yenc_crc_table[256] = {
	0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
	0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
	0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
	0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
	0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
	0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
	0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
	0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
	0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
	0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
	0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
	0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
	0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
	0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
	0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
	0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
	0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
	0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
	0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
	0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
	0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
	0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
	0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
	0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
	0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
	0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
	0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
	0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
	0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
	0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
	0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
	0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

#define yenc_crc_add(crc, c) (yenc_crc_table[(((int) (crc)) ^ ((unsigned char) (c))) & 0xff] ^ ((((int) (crc)) >> 8) & 0x00ffffff))

#define YENC_NEWLINE_ESCAPE (GMIME_YDECODE_STATE_EOLN | GMIME_YDECODE_STATE_ESCAPE)


/**
 * g_mime_ydecode_step:
 * @inbuf: input buffer
 * @inlen: input buffer length
 * @outbuf: output buffer
 * @state: ydecode state
 * @pcrc: part crc state
 * @crc: crc state
 *
 * Performs a 'decode step' on a chunk of yEncoded data of length
 * @inlen pointed to by @inbuf and writes to @outbuf. Assumes the =ybegin
 * and =ypart lines have already been stripped off.
 *
 * To get the crc32 value of the part, use #GMIME_YENCODE_CRC_FINAL
 * (@pcrc). If there are more parts, you should reuse @crc without
 * re-initializing. Once all parts have been decoded, you may get the
 * combined crc32 value of all the parts using #GMIME_YENCODE_CRC_FINAL
 * (@crc).
 *
 * Returns: the number of bytes decoded.
 **/
size_t
g_mime_ydecode_step (const unsigned char *inbuf, size_t inlen, unsigned char *outbuf,
		     int *state, guint32 *pcrc, guint32 *crc)
{
	register const unsigned char *inptr;
	register unsigned char *outptr;
	const unsigned char *inend;
	unsigned char c;
	int ystate;
	
	if (*state & GMIME_YDECODE_STATE_END)
		return 0;
	
	ystate = *state;
	
	inend = inbuf + inlen;
	outptr = outbuf;
	
	inptr = inbuf;
	while (inptr < inend) {
		c = *inptr++;
		
		if ((ystate & YENC_NEWLINE_ESCAPE) == YENC_NEWLINE_ESCAPE) {
			ystate &= ~GMIME_YDECODE_STATE_EOLN;
			
			if (c == 'y') {
				/* we probably have a =yend here */
				ystate |= GMIME_YDECODE_STATE_END;
				break;
			}
		}
		
		if (c == '\n') {
			ystate |= GMIME_YDECODE_STATE_EOLN;
			continue;
		}
		
		if (ystate & GMIME_YDECODE_STATE_ESCAPE) {
			ystate &= ~GMIME_YDECODE_STATE_ESCAPE;
			c -= 64;
		} else if (c == '=') {
			ystate |= GMIME_YDECODE_STATE_ESCAPE;
			continue;
		}
		
		ystate &= ~GMIME_YDECODE_STATE_EOLN;
		
		*outptr++ = c -= 42;
		
		*pcrc = yenc_crc_add (*pcrc, c);
		*crc = yenc_crc_add (*crc, c);
	}
	
	*state = ystate;
	
	return outptr - outbuf;
}


/**
 * g_mime_yencode_step:
 * @inbuf: input buffer
 * @inlen: input buffer length
 * @outbuf: output buffer
 * @state: yencode state
 * @pcrc: part crc state
 * @crc: crc state
 *
 * Performs an yEncode 'encode step' on a chunk of raw data of length
 * @inlen pointed to by @inbuf and writes to @outbuf.
 *
 * @state should be initialized to #GMIME_YENCODE_STATE_INIT before
 * beginning making the first call to this function. Subsequent calls
 * should reuse @state.
 *
 * Along the same lines, @pcrc and @crc should be initialized to
 * #GMIME_YENCODE_CRC_INIT before using.
 *
 * Returns: the number of bytes encoded.
 **/
size_t
g_mime_yencode_step (const unsigned char *inbuf, size_t inlen, unsigned char *outbuf,
		     int *state, guint32 *pcrc, guint32 *crc)
{
	register const unsigned char *inptr;
	register unsigned char *outptr;
	const unsigned char *inend;
	register int already;
	unsigned char c;
	
	inend = inbuf + inlen;
	outptr = outbuf;
	
	already = *state;
	
	inptr = inbuf;
	while (inptr < inend) {
		c = *inptr++;
		
		*pcrc = yenc_crc_add (*pcrc, c);
		*crc = yenc_crc_add (*crc, c);
		
		c += 42;
		
		if (c == '\0' || c == '\t' || c == '\r' || c == '\n' || c == '=') {
			*outptr++ = '=';
			*outptr++ = c + 64;
			already += 2;
		} else {
			*outptr++ = c;
			already++;
		}
		
		if (already >= 128) {
			*outptr++ = '\n';
			already = 0;
		}
	}
	
	*state = already;
	
	return outptr - outbuf;
}


/**
 * g_mime_yencode_close:
 * @inbuf: input buffer
 * @inlen: input buffer length
 * @outbuf: output buffer
 * @state: yencode state
 * @pcrc: part crc state
 * @crc: crc state
 *
 * Call this function when finished encoding data with
 * g_mime_yencode_step() to flush off the remaining state.
 *
 * #GMIME_YENCODE_CRC_FINAL (@pcrc) will give you the crc32 of the
 * encoded "part". If there are more "parts" to encode, you should
 * re-use @crc when encoding the next "parts" and then use
 * #GMIME_YENCODE_CRC_FINAL (@crc) to get the combined crc32 value of
 * all the parts.
 *
 * Returns: the number of bytes encoded.
 **/
size_t
g_mime_yencode_close (const unsigned char *inbuf, size_t inlen, unsigned char *outbuf,
		      int *state, guint32 *pcrc, guint32 *crc)
{
	register unsigned char *outptr = outbuf;
	
	if (inlen)
		outptr += g_mime_yencode_step (inbuf, inlen, outbuf, state, pcrc, crc);
	
	if (*state)
		*outptr++ = '\n';
	
	*state = GMIME_YENCODE_STATE_INIT;
	
	return outptr - outbuf;
}
