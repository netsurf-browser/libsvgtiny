/*
 * This file is part of Libsvgtiny
 * Licensed under the MIT License,
 *                http://opensource.org/licenses/mit-license.php
 * Copyright 2024 Vincent Sanders <vince@netsurf-browser.org>
 */

#include <stddef.h>
#include <math.h>
#include <float.h>

#include "svgtiny.h"
#include "svgtiny_internal.h"

#define SIGNIFICAND_MAX 100000000
#define EXPONENT_MAX 38
#define EXPONENT_MIN -38

/**
 * parse text string into a float
 *
 * A number is started by 0 (started by sign) or more spaces (0x20), tabs (0x09),
 * carridge returns (0xD) and newlines (0xA) followed by a decimal number.
 * A number is defined as https://www.w3.org/TR/css-syntax-3/#typedef-number-token
 *
 * This state machine parses number text into a sign, significand and exponent
 * then builds a single precision float from those values.
 *
 * The significand stores the first nine decimal digits of the number (floats
 * only have seven thus ensuring nothing is lost in conversion).
 *
 * The exponent is limited to 10^38 (again the float limit) and results in
 * FLT_MAX being returned with a range error.
 *
 * An exponent below 10^-38 will result in emitting the smallest value possible
 * FLT_MIN with a range error.
 *
 * This is not a strtof clone because it has an input length limit instead of
 * needing null terminated input, is not locale dependent and only processes
 * decimal numbers (not hex etc.). These limitations are necessary to process
 * the input correctly.
 */
static svgtiny_code
svgtiny_parse_number(const char *text,
		     size_t textlen,
		     const char **textend,
		     float *value)
{
	enum b10sign {
		SPOSITIVE,
		SNEGATIVE,
	};
	const char *dataend;
	const char *cur; /* text cursor */
	enum {
		STATE_WHITESPACE, /* processing whitespace */
		STATE_NUMBER, /* processing whole number */
		STATE_FRACT, /* processing fractional part */
		STATE_SIGNEXPONENT, /* processing exponent part */
		STATE_EXPONENT, /* processing exponent part have seen sign */
	} state = STATE_WHITESPACE;
	enum b10sign sign = SPOSITIVE; /* sign of number being constructed */
	unsigned int significand = 0; /* significand of number being constructed */
	int exponent = 0; /* exponent of the significand (distinct from exponent part) */
	enum b10sign exp_sign = SPOSITIVE; /* sign of exponent part */
	unsigned int exp_value = 0; /* value of the exponent part */
	unsigned int digit_count = 0; /* has an actual digit been seen */

	dataend = text + textlen;

	for (cur=text; cur < dataend ; cur++) {
		switch (state) {
		case STATE_WHITESPACE:
			switch (*cur) {
			case 0x9: case 0xA: case 0xD: case 0x20:
				/* skip whitespace */
				continue;

			case '.':
				/* new number with fraction part */
				digit_count = 0;
				state = STATE_FRACT;
				continue;

			case '-':
				sign = SNEGATIVE;
				digit_count = 0;
				state = STATE_NUMBER;
				continue;

			case '+':
				digit_count = 0;
				state = STATE_NUMBER;
				continue;

			case '0': case '1': case '2': case '3': case '4':
			case '5': case '6': case '7': case '8':	case '9':
				significand = (*cur - '0');
				digit_count = 1;
				state = STATE_NUMBER;
				continue;

			default:
				/* anything else completes conversion */
				goto svgtiny_parse_number_end;
			}
			break;

		case STATE_NUMBER:
			switch(*cur) {
			case '.':
				state = STATE_FRACT;
				continue;

			case '0': case '1': case '2': case '3':	case '4':
			case '5': case '6': case '7': case '8':	case '9':
				digit_count += 1;
				if (significand < SIGNIFICAND_MAX) {
					/* still space to acumulate digits in the significand */
					significand = (significand * 10) + (*cur - '0');
				} else {
					/* significand has accumulated all the
					 * digits it can so just extend the
					 * exponent */
					exponent += 1;
				}
				continue;

			case 'e':
			case 'E':
				if (digit_count == 0) {
					/* number has no digits before exponent which is a syntax error */
					goto svgtiny_parse_number_end;

				}
				state = STATE_SIGNEXPONENT;
				continue;

			default:
				/* anything else completes conversion */
				goto svgtiny_parse_number_end;
			}

			break;

		case STATE_FRACT:
			switch(*cur) {
			case '0': case '1': case '2': case '3':	case '4':
			case '5': case '6': case '7': case '8':	case '9':
				digit_count += 1;
				if (significand < SIGNIFICAND_MAX) {
					/* still space to acumulate digits in the significand */
					significand = (significand * 10) + (*cur - '0');
					exponent -= 1;
				}

				continue;

			case 'e':
			case 'E':
				if (digit_count == 0) {
					/* number has no digits before exponent which is a syntax error */
					goto svgtiny_parse_number_end;

				}
				state = STATE_SIGNEXPONENT;
				continue;

			default:
				/* anything else completes conversion */
				goto svgtiny_parse_number_end;

			}
			break;

		case STATE_SIGNEXPONENT:
			switch(*cur) {
			case '-':
				exp_sign = SNEGATIVE;
				state = STATE_EXPONENT;
				continue;

			case '+':
				state = STATE_EXPONENT;
				continue;

			case '0': case '1': case '2': case '3':	case '4':
			case '5': case '6': case '7': case '8':	case '9':
				if (exp_value < 1000) {
					/* still space to acumulate digits in the exponent value */
					exp_value = (exp_value * 10) + (*cur - '0');
				}
				state = STATE_EXPONENT;
				continue;

			default:
				/* anything else completes conversion */
				goto svgtiny_parse_number_end;

			}
			break;

		case STATE_EXPONENT:
			switch(*cur) {
			case '0': case '1': case '2': case '3':	case '4':
			case '5': case '6': case '7': case '8':	case '9':
				if (exp_value < 1000) {
					/* still space to acumulate digits in the exponent value */
					exp_value = (exp_value * 10) + (*cur - '0');
				}

				continue;

			default:
				/* anything else completes conversion */
				goto svgtiny_parse_number_end;

			}
			break;
		}
	}

svgtiny_parse_number_end:
	*textend = cur;

	if (state == STATE_WHITESPACE) {
		/* no characters except whitespace */
		return svgtiny_SVG_ERROR;
	}

	if (digit_count==0) {
		/* number had no digits (only +-.) which is a syntax error */
		return svgtiny_SVG_ERROR;
	}

	/* deal with exponent value */
	if (exp_sign == SNEGATIVE) {
		exponent -= exp_value;
	} else {
		exponent += exp_value;
	}

	/* deal with number too large to represent */
	if (exponent > EXPONENT_MAX) {
		if (sign == SPOSITIVE) {
			*value = FLT_MAX;
		} else {
			*value = -FLT_MAX;
		}
		return svgtiny_OK;
		/*return svgtiny_RANGE;*/
	}

	/* deal with number too small to represent */
	if (exponent < EXPONENT_MIN) {
		if (sign == SPOSITIVE) {
			*value = FLT_MIN;
		} else {
			*value = -FLT_MIN;
		}
		return svgtiny_OK;
		/*return svgtiny_RANGE;*/
	}

	if (sign == SPOSITIVE) {
		*value = (float)significand * powf(10, exponent);
	} else {
		*value = -(float)significand * powf(10, exponent);
	}

	return svgtiny_OK;
}


/**
 * parse text points into path points
 *
 * \param data Source text to parse
 * \param datalen Length of source text
 * \param pointv output vector of path elements.
 * \param pointc on input has number of path elements in pointv on exit has
 *               the number of elements placed in the output vector.
 * \return svgtiny_OK on success else error code.
 *
 * parses a poly[line|gon] points text into a series of path elements.
 * The syntax is defined in https://www.w3.org/TR/SVG11/shapes.html#PointsBNF or
 * https://svgwg.org/svg2-draft/shapes.html#DataTypePoints
 *
 * This is a series of numbers separated by 0 (started by sign)
 * or more tabs (0x9), spaces (0x20), carrige returns (0xD) and newlines (0xA)
 * there may also be a comma in the separating whitespace after the preamble
 * A number is defined as https://www.w3.org/TR/css-syntax-3/#typedef-number-token
 *
 */
svgtiny_code
svgtiny_parse_poly_points(const char *text,
			  size_t textlen,
			  float *pointv,
			  unsigned int *pointc)
{
	const char *textend = text + textlen;
	const char *numberend = NULL;
	const char *cursor = text; /* text cursor */
	int even = 0; /* is the current point even */
	float point = 0; /* the odd point of the coordinate pair */
	float oddpoint = 0;
	svgtiny_code err;

	*pointc = 0;

	while (cursor < textend) {
		err = svgtiny_parse_number(cursor,
					   textend - cursor,
					   &numberend,
					   &point);
		if (err != svgtiny_OK) {
			break;
		}
		cursor = numberend;

		if (even) {
			even = 0;
			pointv[(*pointc)++] = svgtiny_PATH_LINE;
			pointv[(*pointc)++] = oddpoint;
			pointv[(*pointc)++] = point;
		} else {
			even = 1;
			oddpoint=point;
		}

		/* skip whitespace or comma */
		while(cursor < textend) {
			if ((*cursor != 0x20) &&
			    (*cursor != 0x09) &&
			    (*cursor != 0x0A) &&
			    (*cursor != 0x0D) &&
			    (*cursor != 0x2C)) {
				break;
			}
			cursor++;
		}
	}

	return svgtiny_OK;
}
