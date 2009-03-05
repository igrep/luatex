/* progname.h: Declarations for argv[0] equivalents.

   Copyright 1994, 1996, 2008 Karl Berry.
   Copyright 1999, 2005 Olaf Weber.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this library; if not, see <http://www.gnu.org/licenses/>.  */

#ifndef KPATHSEA_PROGNAME_H
#define KPATHSEA_PROGNAME_H

#include <kpathsea/c-proto.h>
#include <kpathsea/types.h>

extern KPSEDLL string program_invocation_name;
extern KPSEDLL string program_invocation_short_name;
extern KPSEDLL string kpse_program_name;

/* Return directory ARGV0 comes from.  Check PATH if ARGV0 is not
   absolute.  */

extern KPSEDLL string selfdir P1H(const_string argv0);

/* Set the first two variables above (if they're not predefined) to a copy
   of ARGV0 and everything in ARGV0 after the last directory separator,
   respectively.  Set kpse_program_name to a copy of PROGNAME or the
   or the value of program_invocation_short_name if PROGNAME is NULL.
   This function also determines the AUTO* variables. */

extern KPSEDLL void kpse_set_program_name P2H(const_string argv0,
                                      const_string progname);

extern KPSEDLL void kpathsea_set_program_name P3H(kpathsea kpse,
                                      const_string argv0,
                                      const_string progname);

/* See also `kpse_reset_program_name' which is defined in tex-file.c

   That function is to be used to set kpse_program_name to a different
   value.  It clears the path searching information, to ensure that
   the search paths are appropriate to the new name. */

/* DEPRECATED
   Set first two variables above (if they're not predefined) to a copy of
   ARGV0 and everything in ARGV0 after the last directory separator,
   respectively.  kpse_program_name is _always_ set to a copy of everything
   in ARGV0 after the last directory separator. */

extern KPSEDLL void kpse_set_progname P1H(const_string argv0);

#endif /* not KPATHSEA_PROGNAME_H */
