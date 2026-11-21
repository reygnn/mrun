/* version.h - the one place mrun's version is set; mrun -v reports it.
 * MRUN_GIT, from the Makefile, adds the commit built: +DATE.TIME.HASH. */
#ifndef MRUN_VERSION_H
#define MRUN_VERSION_H

#include "build/gitversion.h"
#define MRUN_VERSION "0.1.0" MRUN_GIT

#endif
