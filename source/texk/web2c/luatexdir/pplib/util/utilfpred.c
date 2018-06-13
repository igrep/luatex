/* predictor filters; common for flate and lzw */

#include "utilmem.h"
#include "utilfpred.h"
#include "utilfpreddef.h"

/*
Here we implement predictor filters used with flate and lzw compressions in PDF streams. The main idea of data prediction
is to compute and output the differences between data records instead of this records. Adjacent pixels in images are usually
similar, so differences between pixel values tends to be zero. And both Flate and LZW performs better when the input
is rather smooth. Although a preliminary use of predictors is related to bitmap data, The actual need for predictor filter
came from the fact that xref streams may also be predicted (usually with PNG up-predictor).

PDF specification allows to use several predictor algorithms, specified by /Predictor key in /DecodeParms dictionary:

   1 - no predictor (default)
   2 - TIFF horizontal predictor
  10 - PNG none predictor
  11 - PNG sub predictor
  12 - PNG up predictor
  13 - PNG average predictor
  14 - PNG paeth predictor

All PNG predictors works on bytes, regardless the image color-depth. While encoding, every input data byte is decreased
by the appropriate byte of the previous pixel. Even if the pixel does not fit a full byte, PNG predictors use an artificial
pixel size rounded up to a full byte. PNG predictors utilizes previous (left) pixel, pixel above and previous to above
pixel. In case of PNG, the type of the predictor is written on a dedicated byte on the beginning of every scanline. It
means all predictor functions must maintain and information about left, above and left-above pixels.

Despite the same differencing idea, TIFF predictors are different. The prediction process bases on pixel components,
which are not necessarily bytes (component of a pixel is added/substracted from a relevant component of a previous
pixel). In TIFF predictor 2, only the previous (the left) pixel is taken into account, there is no need to keep
an information about other surrounding pixels. Also there is no expicit algorithm marker in data; the same prediction
method is applied to all input rows.

Not surprisingly, predictor encoders and decoders are pretty similar. Encoders take some input value and the previous
input value (or 0 at the beginning of the scanline) and output a difference between them. Decoders takes an input value,
previously decoded value (or zero) and outputs their sum. When encoding, the result is cast to the proper unsigned integer,
when decoding, modulo 256 (or appropriate) is used, which makes encoding and decoding looseless.

Some extra bits trickery is involved in TIFF predictor function, when components doesn't fit bytes boundary. In that case,
an input is treated as an bits stream. Every input byte is "buffered" in a larger integer, as its lower bits (from right).
Every output value is taken from its higher (left) bits. In a special case of bits-per-component equal 1, we buffer all
pixel bits and use XOR to compute bits difference between pixels. I've excerpted that trick from poppler, but I'm not
really sure if it works any better, especially when the number of components per pixel is 1. In that case we do a hard
bit-by-bit work anyway.

Predictor codecs state keeps a notion of surrounding pixels. PNG predictors uses left, up and upleft
pixel data, while TIFF predictor (2) only needs the previous (left) pixel. Important to note that PNG
predictors always work on bytes, no matter of color-depth (bits per component), while TIFF predictor
works on pixel components, which not necessarily fits into a complete byte. However, for PNG predictor
the size of a pixel still matters, because 'left' and 'upleft' refers to a corresponding pixel byte,
not necessarily previous byte.

In PNG prediction, we record every pixel byte (in decoded form) in state->rowsave. At the end of a scanline
we copy state->rowsave to state->rowup, so that in the next scanline we can access up-pixel byte.
Left pixel byte is accessed as state->rowsave (the byte recently stored or virtual left edge byte \0).
Up-left pixel byte is accessed via state->rowup, but with state->pixelsize offset (same as left byte, possibly \0
at the left edge of the row). Both state->rowup and state->rowsave has a safe span of pixelsize bytes on the left,
that are permanently \0.
*/

enum {
  STATUS_LAST = 0,
  STATUS_CONTINUE = 1 // any value different then IOFEOF, IOFERR, ...
};

predictor_state * predictor_decoder_init (predictor_state *state, int predictor, int rowsamples, int components, int compbits)
{
  int rowsize, pixelsize;
#define storage_pos(b, p, size) ((b = p), (p += size))
  uint8_t *buffer, *p;
  size_t buffersize;

  pixelsize = (components * compbits + 7) >> 3; // to bytes, rounded up
  rowsize = (rowsamples * components * compbits + 7) >> 3;

  state->default_predictor = state->current_predictor = predictor;
  state->rowsamples = rowsamples;
  state->components = components;
  state->compbits = compbits;

  if (predictor == 2)
  { /* tiff predictor */
    size_t compbuf, pixbuf;
    compbuf = state->components * sizeof(predictor_component_t);
    pixbuf = 1 * sizeof(predictor_pixel1b_t);
    state->pixbufsize = (int)(compbuf > pixbuf ? compbuf : pixbuf);
    buffersize = rowsize + state->pixbufsize;
    buffer = (uint8_t *)util_calloc(buffersize, 1);
    state->prevcomp = (predictor_component_t *)(state->rowin + rowsize);
    state->sampleindex = state->compindex = 0;
    state->bitsin = state->bitsout = 0;
    state->compin = state->compout = 0;
  }
  else
  { /* png predictors */
    buffersize = (3 * rowsize + 2 * pixelsize + 1) * sizeof(uint8_t);
    p = buffer = (uint8_t *)util_calloc(buffersize, 1);
    storage_pos(state->rowin, p, 1 + rowsize); // one extra byte for prediction algorithm tag
    p += pixelsize;                            // pixelsize extra bytes for virtual left pixel at the edge, eg. rowup[-1] (permanently \0)
    storage_pos(state->rowup, p, rowsize);     // actual row byte
    p += pixelsize;                            // ditto
    storage_pos(state->rowsave, p, rowsize);
    state->pixelsize = pixelsize;
    state->predictorbyte = 0;
  }
  state->buffer = buffer;
  state->rowsize = rowsize;
  state->rowindex = 0;
  state->rowend = 0;
  state->status = STATUS_CONTINUE;
  return state;
}

predictor_state * predictor_encoder_init (predictor_state *state, int predictor, int rowsamples, int components, int compbits)
{
  return predictor_decoder_init(state, predictor, rowsamples, components, compbits);
}

void predictor_decoder_close (predictor_state *state)
{
  util_free(state->buffer);
}

void predictor_encoder_close (predictor_state *state)
{
  util_free(state->buffer);
}

/*
Predictor type identifiers (pdf spec 76). lpdf doesn't hire the codec if predictor is 1. Predictor 15 indicates
that the type of PNG prediction algorithm may change in subsequent lines. We always check algorithm marker anyway.
*/

enum predictor_code {
  NONE_PREDICTOR = 1,
  TIFF_PREDICTOR = 2,
  PNG_NONE_PREDICTOR = 10,
  PNG_SUB_PREDICTOR = 11,
  PNG_UP_PREDICTOR = 12,
  PNG_AVERAGE_PREDICTOR = 13,
  PNG_PAETH_PREDICTOR = 14,
  PNG_OPTIMUM_PREDICTOR = 15
};

/*
All predoctor codecs first read the entire data row into a buffer. This is not crucial for the process,
but allows to separate read/write states. In particular, there is one place in which codec functions
may return on EOD.
*/

#define start_row(state) (state->rowindex = 0, state->rowin = state->buffer)

static int read_scanline (predictor_state *state, iof *I, int size)
{
  int rowtail, left;
  while ((rowtail = size - state->rowend) > 0)
  {
    left = (int)iof_left(I);
    if (left >= rowtail)
    {
      memcpy(state->buffer + state->rowend, I->pos, (size_t)rowtail);
      state->rowend += rowtail;
      I->pos += rowtail;
      start_row(state);
      break;
    }
    else
    {
      if ((rowtail = left) > 0)
      {
        memcpy(state->buffer + state->rowend, I->pos, (size_t)rowtail);
        state->rowend += rowtail;
        I->pos += rowtail;
      }
      if (iof_input(I) == 0)
      {
        if (state->rowend == 0) // no scanline to process, no more input
          return state->flush ? IOFEOF : IOFEMPTY;
        /* If we are here, there is an incomplete scanline in buffer:
           - if there is a chance for more (state->flush == 0), than wait for more
           - otherwise encode/decode the last incomplete line?
           pdf spec p. 76 says that "A row occupies a whole number of bytes",
           so this situation should be considered abnormal (not found so far).
         */
        if (!state->flush)
          return IOFEMPTY;
        printf("incomplete scanline in predictor filter\n");
        //return IOFERR;
        state->status = STATUS_LAST;
        state->rowsize -= size - state->rowend;
        start_row(state);
        break;
      }
    }
  }
  return STATUS_CONTINUE;
}

#define read_row(state, I, size, status) if ((status = read_scanline(state, I, size)) != STATUS_CONTINUE) return status

#define ensure_output_bytes(O, n) if (!iof_ensure(O, n)) return IOFFULL

#define tobyte(c) ((unsigned char)(c))
#define tocomp(c) ((unsigned short)(c))

#define row_byte(state) (state->rowin[state->rowindex])

#define up_pixel_byte(state)     (state->rowup[state->rowindex])
#define upleft_pixel_byte(state) (state->rowup[state->rowindex - state->pixelsize])
#define left_pixel_byte(state)   (state->rowsave[state->rowindex - state->pixelsize])

#define save_pixel_byte(state, c) (state->rowsave[state->rowindex] = c)

#define left_pixel_component(state) (state->prevcomp[state->compindex]) // tiff predictor with 2, 4, 8, 16 components
#define left_pixel_value(state) (state->prevpixel[0])                   // tiff predictor with 1bit components

#define save_pixel_component(state, c) ((void)\
  ((state->prevcomp[state->compindex] = c), \
   (++state->compindex < state->components || (state->compindex = 0))))

#define save_pixel_value(state, c) (state->prevpixel[0] = c)

/* Once the codec function is done with the scanline, we set imaginary left pixel data to zero, and reset row counters to
zero in order to allow buffering another input scanline. */

#define reset_row(state) state->rowend = 0

#define reset_png_row(state) (memcpy(state->rowup, state->rowsave, state->rowsize), state->predictorbyte = 0, reset_row(state))

#define reset_tiff_row(state) \
  memset(state->prevcomp, 0, state->pixbufsize), \
  state->bitsin = state->bitsout = 0, \
  state->compin = state->compout = 0, \
  reset_row(state), \
  state->sampleindex = state->compindex = 0

/* PNG paeth predictor function; http://www.libpng.org/pub/png/book/chapter09.html
Compute the base value p := left + up - upleft, then choose that byte the closest
(of the smallest absolute difference) to the base value. Left byte has a precedence. */


static int paeth (predictor_state *state)
{
  int p, p1, p2, p3;
  p = left_pixel_byte(state) + up_pixel_byte(state) - upleft_pixel_byte(state);
  p1 = p >= left_pixel_byte(state)   ? (p - left_pixel_byte(state))   : (left_pixel_byte(state) - p);
  p2 = p >= up_pixel_byte(state)     ? (p - up_pixel_byte(state))     : (up_pixel_byte(state) - p);
  p3 = p >= upleft_pixel_byte(state) ? (p - upleft_pixel_byte(state)) : (upleft_pixel_byte(state) - p);
  return (p1 <= p2 && p1 <= p3) ? left_pixel_byte(state) : (p2 <= p3 ? up_pixel_byte(state) : upleft_pixel_byte(state));
}

/* predictor decoder */

iof_status predictor_decode_state (iof *I, iof *O, predictor_state *state)
{
  int status, c, d, outbytes;
  while (state->status == STATUS_CONTINUE)
  {
    if (state->default_predictor >= 10) // PNG predictor?
    {
      read_row(state, I, state->rowsize + 1, status);
      if (state->predictorbyte == 0)
      { // we could actually check state->rowin <> state->buffer, but we need this flag for encoder anyway
        state->current_predictor = row_byte(state) + 10;
        state->predictorbyte = 1;
        ++state->rowin;
      }
    }
    else
    {
      read_row(state, I, state->rowsize, status);
    }
    switch (state->current_predictor)
    {
      case NONE_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = row_byte(state);
          iof_set(O, c);
        }
        reset_row(state);
        break;
      case TIFF_PREDICTOR:
        switch (state->compbits)
        {
          case 1:
            outbytes = (state->components + 7) >> 3;
            for ( ; state->sampleindex < state->rowsamples; ++state->sampleindex)
            {
              ensure_output_bytes(O, outbytes);
              while (state->bitsin < state->components)
              {
                state->compin = (state->compin << 8) | row_byte(state);
                state->bitsin += 8;
                ++state->rowindex;
              }
              state->bitsin -= state->components;
              d = state->compin >> state->bitsin;
              state->compin &= (1 << state->bitsin) - 1;
              c = d ^ left_pixel_value(state);
              save_pixel_value(state, c);
              state->compout = (state->compout << state->components) | c;
              state->bitsout += state->components;
              while (state->bitsout >= 8)
              {
                state->bitsout -= 8;
                iof_set(O, state->compout >> state->bitsout);
                state->compout &= (1 << state->bitsout) - 1;
              }
            }
            if (state->bitsout > 0)
            {
              ensure_output_bytes(O, 1);
              iof_set(O, state->compin << (8 - state->bitsout));
            }
            break;
          case 2: case 4:
            for ( ; state->sampleindex < state->rowsamples; ++state->sampleindex)
            {
              for ( ; state->compindex < state->components; ) // state->compindex is ++ed  by save_pixel_component()
              {
                ensure_output_bytes(O, 1);
                if (state->bitsin < state->compbits)
                {
                  state->compin = (state->compin << 8) | row_byte(state);
                  state->bitsin += 8;
                  ++state->rowindex;
                }
                state->bitsin -= state->compbits;
                d = state->compin >> state->bitsin;
                state->compin &= (1 << state->bitsin) - 1;
                c = (d + left_pixel_component(state)) & 0xff;
                save_pixel_component(state, c);
                state->compout = (state->compout << state->compbits) | c;
                state->bitsout += state->compbits;
                if (state->bitsout >= 8)
                {
                  state->bitsout -= 8;
                  iof_set(O, state->compout >> state->bitsout);
                  state->compout &= (1 << state->bitsout) - 1;
                }
              }
            }
            if (state->bitsout > 0)
            {
              ensure_output_bytes(O, 1);
              iof_set(O, state->compin << (8 - state->bitsout));
            }
            break;
          case 8:
            for ( ; state->rowindex < state->rowsize; ++state->rowindex)
            {
              ensure_output_bytes(O, 1);
              c = (row_byte(state) + left_pixel_component(state)) & 0xff;
              save_pixel_component(state, c);
              iof_set(O, c);
            }
            break;
          case 16:
            for ( ; state->rowindex < state->rowsize - 1; ++state->rowindex)
            {
              ensure_output_bytes(O, 2);
              d = row_byte(state) << 8;
              ++state->rowindex;
              d |= row_byte(state);
              c = (d + left_pixel_component(state)) & 0xff;
              save_pixel_component(state, c);
              iof_set2(O, c >> 8, c & 0xff);
            }
            break;
          default:
            return IOFERR;
        }
        reset_tiff_row(state);
        break;
      case PNG_NONE_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = row_byte(state);
          save_pixel_byte(state, c); // next row may need it
          iof_set(O, c);
        }
        reset_png_row(state);
        break;
      case PNG_SUB_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = (row_byte(state) + left_pixel_byte(state)) & 0xff;
          save_pixel_byte(state, c);
          iof_set(O, c);
        }
        reset_png_row(state);
        break;
      case PNG_UP_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = (row_byte(state) + up_pixel_byte(state)) & 0xff;
          save_pixel_byte(state, c);
          iof_set(O, c);
        }
        reset_png_row(state);
        break;
      case PNG_AVERAGE_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = (row_byte(state) + ((up_pixel_byte(state) + left_pixel_byte(state)) / 2)) & 0xff;
          save_pixel_byte(state, c);
          iof_set(O, c);
        }
        reset_png_row(state);
        break;
      case PNG_PAETH_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = (row_byte(state) + paeth(state)) & 0xff;
          save_pixel_byte(state, c);
          iof_set(O, c);
        }
        reset_png_row(state);
        break;
      //case PNG_OPTIMUM_PREDICTOR: // valid as default_redictor, but not as algorithm identifier byte
      default:
        return IOFERR;
    }
  }
  return state->status == STATUS_LAST ? IOFERR : IOFEOF;
}

/* predictor encoder */

iof_status predictor_encode_state (iof *I, iof *O, predictor_state *state)
{
  int status, c, d, outbytes;
  while (state->status == STATUS_CONTINUE)
  {
    read_row(state, I, state->rowsize, status);
    if (state->current_predictor >= 10 && state->predictorbyte == 0)
    {
      ensure_output_bytes(O, 1);
      iof_set(O, state->current_predictor - 10);
      state->predictorbyte = 1;
    }
    switch (state->current_predictor)
    {
      case NONE_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = row_byte(state);
          iof_set(O, c);
        }
        reset_row(state);
        break;
      case TIFF_PREDICTOR:
        switch (state->compbits)
        {
          case 1:
            outbytes = (state->components + 7) >> 3;
            for ( ; state->sampleindex < state->rowsamples; ++state->sampleindex)
            {
              ensure_output_bytes(O, outbytes);
              while (state->bitsin < state->components)
              {
                state->compin = (state->compin << 8) | row_byte(state);
                state->bitsin += 8;
                ++state->rowindex;
              }
              state->bitsin -= state->components;
              c = state->compin >> state->bitsin;
              state->compin &= (1 << state->bitsin) - 1;
              d = c ^ left_pixel_value(state);
              save_pixel_value(state, c);
              state->compout = (state->compout << state->components) | d;
              state->bitsout += state->components;
              while (state->bitsout >= 8)
              {
                state->bitsout -= 8;
                iof_set(O, state->compout >> state->bitsout);
                state->compout &= (1 << state->bitsout) - 1;
              }
            }
            if (state->bitsout > 0)
            {
              ensure_output_bytes(O, 1);
              iof_set(O, state->compin << (8 - state->bitsout));
            }
            break;
          case 2: case 4:
            for ( ; state->sampleindex < state->rowsamples; ++state->sampleindex)
            {
              for ( ; state->compindex < state->components; )
              {
                ensure_output_bytes(O, 1);
                if (state->bitsin < state->compbits)
                {
                  state->compin = (state->compin << 8) | row_byte(state);
                  state->bitsin += 8;
                  ++state->rowindex;
                }
                state->bitsin -= state->compbits;
                c = state->compin >> state->bitsin;
                state->compin &= (1 << state->bitsin) - 1;
                d = tocomp(c - left_pixel_component(state));
                save_pixel_component(state, c);
                state->compout = (state->compout << state->compbits) | d;
                state->bitsout += state->compbits;
                if (state->bitsout >= 8)
                {
                  state->bitsout -= 8;
                  iof_set(O, state->compout >> state->bitsout);
                  state->compout &= (1 << state->bitsout) - 1;
                }
              }
            }
            if (state->bitsout > 0)
            {
              ensure_output_bytes(O, 1);
              iof_set(O, state->compin << (8 - state->bitsout));
            }
            break;
          case 8:
            for ( ; state->rowindex < state->rowsize; ++state->rowindex)
            {
              ensure_output_bytes(O, 1);
              c = row_byte(state);
              d = tobyte(c - left_pixel_component(state));
              save_pixel_component(state, c);
              iof_set(O, d);
            }
            break;
          case 16:
            for ( ; state->rowindex < state->rowsize - 1; ++state->rowindex)
            {
              ensure_output_bytes(O, 2);
              c = row_byte(state) << 8;
              ++state->rowindex;
              c |= row_byte(state);
              d = tocomp(c - left_pixel_component(state));
              save_pixel_component(state, c);
              iof_set2(O, d >> 8, d & 0xff);
            }
            break;
          default:
            return IOFERR;
        }
        reset_tiff_row(state);
        break;
      case PNG_NONE_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = row_byte(state);
          save_pixel_byte(state, c); // next row may need it
          iof_set(O, c);
        }
        reset_png_row(state);
        break;
      case PNG_SUB_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = row_byte(state);
          d = tobyte(c - left_pixel_byte(state));
          save_pixel_byte(state, c);
          iof_set(O, d);
        }
        reset_png_row(state);
        break;
      case PNG_OPTIMUM_PREDICTOR: // not worthy to perform optimization
      case PNG_UP_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = row_byte(state);
          d = tobyte(c - up_pixel_byte(state));
          save_pixel_byte(state, c);
          iof_set(O, d);
        }
        reset_png_row(state);
        break;
      case PNG_AVERAGE_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = row_byte(state);
          d = tobyte(c - ((up_pixel_byte(state) + left_pixel_byte(state)) >> 1));
          save_pixel_byte(state, c);
          iof_set(O, d);
        }
        reset_png_row(state);
        break;
      case PNG_PAETH_PREDICTOR:
        for ( ; state->rowindex < state->rowsize; ++state->rowindex)
        {
          ensure_output_bytes(O, 1);
          c = row_byte(state);
          d = tobyte(c - paeth(state));
          save_pixel_byte(state, c);
          iof_set(O, d);
        }
        reset_png_row(state);
        break;
      default:
        return IOFERR;
    }
  }
  return state->status == STATUS_LAST ? IOFERR : IOFEOF;
}

iof_status predictor_decode (iof *I, iof *O, int predictor, int rowsamples, int components, int compbits)
{
  predictor_state state;
  int ret;
  predictor_decoder_init(&state, predictor, rowsamples, components, compbits);
  state.flush = 1;
  ret = predictor_decode_state(I, O, &state);
  predictor_decoder_close(&state);
  return ret;
}

iof_status predictor_encode (iof *I, iof *O, int predictor, int rowsamples, int components, int compbits)
{
  predictor_state state;
  int ret;
  predictor_encoder_init(&state, predictor, rowsamples, components, compbits);
  state.flush = 1;
  ret = predictor_encode_state(I, O, &state);
  predictor_encoder_close(&state);
  return ret;
}
