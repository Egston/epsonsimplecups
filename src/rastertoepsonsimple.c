/*
 * This code is based on the Star Micronics Driver. The copyright notice
 * is preserved below, however, the code at this point bears very little relationship
 * to the original work. What is preserved is the option retrieval code, the CUPS api
 * retrieval code, and the structure of the main loop. Otherwise, most of the options
 * have been removed and the remaining options and the core implementation of raster
 * transfer are totally different.
 * GPL still applies.
 *
 * Stephen Hawley, 5/10/2015
 */

/*
 * Copyright (C) 2004-2015 Star Micronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include "bufferedscanlines.h"
#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#ifdef MACOSX
#include <cups/backend.h>
#include <cups/sidechannel.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#ifdef RPMBUILD

#include <dlfcn.h>


typedef cups_raster_t * (*cupsRasterOpen_fndef)(int fd, cups_mode_t mode);
typedef unsigned (*cupsRasterReadHeader2_fndef)(cups_raster_t *r, cups_page_header2_t *h);
typedef unsigned (*cupsRasterReadPixels_fndef)(cups_raster_t *r, unsigned char *p, unsigned len);
typedef void (*cupsRasterClose_fndef)(cups_raster_t *r);

static cupsRasterOpen_fndef cupsRasterOpen_fn;
static cupsRasterReadHeader2_fndef cupsRasterReadHeader2_fn;
static cupsRasterReadPixels_fndef cupsRasterReadPixels_fn;
static cupsRasterClose_fndef cupsRasterClose_fn;

#define CUPSRASTEROPEN (*cupsRasterOpen_fn)
#define CUPSRASTERREADHEADER2 (*cupsRasterReadHeader2_fn)
#define CUPSRASTERREADPIXELS (*cupsRasterReadPixels_fn)
#define CUPSRASTERCLOSE (*cupsRasterClose_fn)

typedef void (*ppdClose_fndef)(ppd_file_t *ppd);
typedef ppd_choice_t * (*ppdFindChoice_fndef)(ppd_option_t *o, const char *option);
typedef ppd_choice_t * (*ppdFindMarkedChoice_fndef)(ppd_file_t *ppd, const char *keyword);
typedef ppd_option_t * (*ppdFindOption_fndef)(ppd_file_t *ppd, const char *keyword);
typedef void (*ppdMarkDefaults_fndef)(ppd_file_t *ppd);
typedef ppd_file_t * (*ppdOpenFile_fndef)(const char *filename);

typedef void (*cupsFreeOptions_fndef)(int num_options, cups_option_t *options);
typedef int (*cupsParseOptions_fndef)(const char *arg, int num_options, cups_option_t **options);
typedef int (*cupsMarkOptions_fndef)(ppd_file_t *ppd, int num_options, cups_option_t *options);

static ppdClose_fndef ppdClose_fn;
static ppdFindChoice_fndef ppdFindChoice_fn;
static ppdFindMarkedChoice_fndef ppdFindMarkedChoice_fn;
static ppdFindOption_fndef ppdFindOption_fn;
static ppdMarkDefaults_fndef ppdMarkDefaults_fn;
static ppdOpenFile_fndef ppdOpenFile_fn;

static cupsFreeOptions_fndef cupsFreeOptions_fn;
static cupsParseOptions_fndef cupsParseOptions_fn;
static cupsMarkOptions_fndef cupsMarkOptions_fn;

#define PPDCLOSE            (*ppdClose_fn)
#define PPDFINDCHOICE       (*ppdFindChoice_fn)
#define PPDFINDMARKEDCHOICE (*ppdFindMarkedChoice_fn)
#define PPDFINDOPTION       (*ppdFindOption_fn)
#define PPDMARKDEFAULTS     (*ppdMarkDefaults_fn)
#define PPDOPENFILE         (*ppdOpenFile_fn)

#define CUPSFREEOPTIONS     (*cupsFreeOptions_fn)
#define CUPSPARSEOPTIONS    (*cupsParseOptions_fn)
#define CUPSMARKOPTIONS     (*cupsMarkOptions_fn)

#else

#define CUPSRASTEROPEN cupsRasterOpen
#define CUPSRASTERREADHEADER2 cupsRasterReadHeader2
#define CUPSRASTERREADPIXELS cupsRasterReadPixels
#define CUPSRASTERCLOSE cupsRasterClose

#define PPDCLOSE ppdClose
#define PPDFINDCHOICE ppdFindChoice
#define PPDFINDMARKEDCHOICE ppdFindMarkedChoice
#define PPDFINDOPTION ppdFindOption
#define PPDMARKDEFAULTS ppdMarkDefaults
#define PPDOPENFILE ppdOpenFile

#define CUPSFREEOPTIONS cupsFreeOptions
#define CUPSPARSEOPTIONS cupsParseOptions
#define CUPSMARKOPTIONS cupsMarkOptions

#endif

#define MAX(a,b) ( ((a) > (b)) ? (a) : (b) )
#define MIN(a,b) ( ((a) < (b)) ? (a) : (b) )

#define FALSE 0
#define TRUE  (!FALSE)

struct settings_ {
    float pageWidth;
    float pageHeight;

    int pageCutType;
    int docCutType;

    int marginReductionType;

    int doubleMode;
    int drawerKick;
};

struct command {
    int length;
    char* command;
};

/**
 * Flags for top/bottom margin reduction.
 *
 * Corresponds to values of configuration option MarginReductionType in PPD.
 */
enum {
    noMarginReduction = 0,
    topMarginReduction = 1,
    bottomMarginReduction = 2,
    bothMarginReduction = 3
};


static const struct command printerInitializeCommand ={2, (char[2])
    {0x1b, '@'}};

static const struct command pageCutCommand ={4, (char[4])
    {29, 'V', 'A', 20}};

static const struct command drawerKickCommand ={5, (char[5])
    {27, 112, 48, 55, 121}};

inline void debugPrintSettings(struct settings_ * settings) {
    fprintf(stderr, "DEBUG: pageCutType = %d\n", settings->pageCutType);
    fprintf(stderr, "DEBUG: docCutType = %d\n", settings->docCutType);
    fprintf(stderr, "DEBUG: marginReductionType = %d\n", settings->marginReductionType);
    fprintf(stderr, "DEBUG: doubleMode = %d\n", settings->doubleMode);
    fprintf(stderr, "DEBUG: pageWidth = %f\n", settings->pageWidth);
    fprintf(stderr, "DEBUG: pageHeight = %f\n", settings->pageHeight);
}

inline void outputCommand(struct command output) {
    int i = 0;

    for (; i < output.length; i++) {
        putchar(output.command[i]);
    }
}

inline int getOptionChoiceIndex(const char * choiceName, ppd_file_t * ppd) {
    ppd_choice_t * choice;
    ppd_option_t * option;

    choice = PPDFINDMARKEDCHOICE(ppd, choiceName);
    if (choice == NULL) {
        if ((option = PPDFINDOPTION(ppd, choiceName)) == NULL) return -1;
        if ((choice = PPDFINDCHOICE(option, option->defchoice)) == NULL) return -1;
    }

    return atoi(choice->choice);
}

inline void getPageWidthPageHeight(ppd_file_t * ppd, struct settings_ * settings) {
    ppd_choice_t * choice;
    ppd_option_t * option;

    char width[20];
    int widthIdx;

    char height[20];
    int heightIdx;

    char * pageSize;
    int idx;

    int state;

    choice = PPDFINDMARKEDCHOICE(ppd, "PageSize");
    if (choice == NULL) {
        option = PPDFINDOPTION(ppd, "PageSize");
        choice = PPDFINDCHOICE(option, option->defchoice);
    }

    widthIdx = 0;
    memset(width, 0x00, sizeof (width));

    heightIdx = 0;
    memset(height, 0x00, sizeof (height));

    pageSize = choice->choice;
    idx = 0;

    state = 0; // 0 = init, 1 = width, 2 = height, 3 = complete, 4 = fail

    while (pageSize[idx] != 0x00) {
        if (state == 0) {
            if (pageSize[idx] == 'X') {
                state = 1;

                idx++;
                continue;
            }
        } else if (state == 1) {
            if ((pageSize[idx] >= '0') && (pageSize[idx] <= '9')) {
                width[widthIdx++] = pageSize[idx];

                idx++;
                continue;
            } else if (pageSize[idx] == 'D') {
                width[widthIdx++] = '.';

                idx++;
                continue;
            } else if (pageSize[idx] == 'M') {
                idx++;
                continue;
            } else if (pageSize[idx] == 'Y') {
                state = 2;

                idx++;
                continue;
            }
        } else if (state == 2) {
            if ((pageSize[idx] >= '0') && (pageSize[idx] <= '9')) {
                height[heightIdx++] = pageSize[idx];

                idx++;
                continue;
            } else if (pageSize[idx] == 'D') {
                height[heightIdx++] = '.';

                idx++;
                continue;
            } else if (pageSize[idx] == 'M') {
                state = 3;
                break;
            }
        }

        state = 4;
        break;
    }

    if (state == 3) {
        settings->pageWidth = atof(width);
        settings->pageHeight = atof(height);
    } else {
        settings->pageWidth = 0;
        settings->pageHeight = 0;
    }
}

inline void initializeSettings(char * commandLineOptionSettings, struct settings_ * settings) {
    ppd_file_t * ppd = NULL;
    cups_option_t * options = NULL;
    int numOptions;

    ppd = PPDOPENFILE(getenv("PPD"));

    PPDMARKDEFAULTS(ppd);

    numOptions = CUPSPARSEOPTIONS(commandLineOptionSettings, 0, &options);
    if ((numOptions != 0) && (options != NULL)) {
        CUPSMARKOPTIONS(ppd, numOptions, options);

        CUPSFREEOPTIONS(numOptions, options);
    }

    memset(settings, 0x00, sizeof (struct settings_));

    settings->pageCutType = getOptionChoiceIndex("PageCutType", ppd);
    settings->docCutType = getOptionChoiceIndex("DocCutType", ppd);
    settings->marginReductionType = getOptionChoiceIndex("MarginReductionType", ppd);
    settings->doubleMode = getOptionChoiceIndex("PixelDoublingType", ppd);
    settings->drawerKick = getOptionChoiceIndex("CashDrawerType", ppd);

    getPageWidthPageHeight(ppd, settings);

    PPDCLOSE(ppd);

    debugPrintSettings(settings);
}

void jobSetup() {
    fputs("DEBUG: Initializing printer\n", stderr);
    outputCommand(printerInitializeCommand);
}

void endPage(struct settings_ settings) {
    fputs("DEBUG: End of page\n", stderr);
    if (settings.pageCutType) {
        fputs("DEBUG: Cutting page\n", stderr);
        outputCommand(pageCutCommand);
    }
}

void endJob(struct settings_ settings) {
    fputs("DEBUG: End of document\n", stderr);
    if (settings.docCutType) {
        fputs("DEBUG: Cutting page\n", stderr);
        outputCommand(pageCutCommand);
    }
    if (settings.drawerKick) {
        fputs("DEBUG: Kicking drawer\n", stderr);
        outputCommand(drawerKickCommand);
    }
}

#define GET_LIB_FN_OR_EXIT_FAILURE(fn_ptr,lib,fn_name)                                      \
{                                                                                           \
    fn_ptr = dlsym(lib, fn_name);                                                           \
    if ((dlerror()) != NULL)                                                                \
    {                                                                                       \
        fputs("ERROR: required fn not exported from dynamically loaded libary\n", stderr);  \
        if (libCupsImage != 0) dlclose(libCupsImage);                                       \
        if (libCups      != 0) dlclose(libCups);                                            \
        return EXIT_FAILURE;                                                                \
    }                                                                                       \
}

#ifdef RPMBUILD
#define CLEANUP                                                         \
{                                                                       \
    fputs("DEBUG: Cleaning up\n", stderr);                              \
    outputCommand(printerInitializeCommand);                            \
    if (rasterData   != NULL) free(rasterData);                         \
    if (emptyLinePattern != NULL) free(emptyLinePattern);               \
    CUPSRASTERCLOSE(ras);                                               \
    if (fd != 0)                                                        \
    {                                                                   \
        close(fd);                                                      \
    }                                                                   \
    dlclose(libCupsImage);                                              \
    dlclose(libCups);                                                   \
}
#else
#define CLEANUP                                                         \
{                                                                       \
    fputs("DEBUG: Cleaning up\n", stderr);                              \
    outputCommand(printerInitializeCommand);                            \
    if (rasterData   != NULL) free(rasterData);                         \
    if (emptyLinePattern != NULL) free(emptyLinePattern);               \
    CUPSRASTERCLOSE(ras);                                               \
    if (fd != 0)                                                        \
    {                                                                   \
        close(fd);                                                      \
    }                                                                   \
}
#endif

int main(int argc, char *argv[]) {
    int fd = 0; /* File descriptor providing CUPS raster data  */
    cups_raster_t * ras = NULL; /* Raster stream for printing  */
    cups_page_header2_t header; /* CUPS Page header */
    int page = 0; /* Current page */

    unsigned y = 0; /* Vertical position in page 0 <= y <= header.cupsHeight */

    unsigned char * rasterData = NULL; /* Pointer to raster data buffer */
    unsigned char * emptyLinePattern = NULL;
    struct settings_ settings; /* Configuration settings */

    unsigned int bytesPerScanline = 0;

#ifdef RPMBUILD
    void * libCupsImage = NULL; /* Pointer to libCupsImage library */
    void * libCups = NULL; /* Pointer to libCups library */


    libCups = dlopen("libcups.so", RTLD_NOW | RTLD_GLOBAL);
    if (!libCups) {
        fputs("ERROR: libcups.so load failure\n", stderr);
        return EXIT_FAILURE;
    }

    libCupsImage = dlopen("libcupsimage.so", RTLD_NOW | RTLD_GLOBAL);
    if (!libCupsImage) {
        fputs("ERROR: libcupsimage.so load failure\n", stderr);
        dlclose(libCups);
        return EXIT_FAILURE;
    }

    GET_LIB_FN_OR_EXIT_FAILURE(ppdClose_fn, libCups, "ppdClose");
    GET_LIB_FN_OR_EXIT_FAILURE(ppdFindChoice_fn, libCups, "ppdFindChoice");
    GET_LIB_FN_OR_EXIT_FAILURE(ppdFindMarkedChoice_fn, libCups, "ppdFindMarkedChoice");
    GET_LIB_FN_OR_EXIT_FAILURE(ppdFindOption_fn, libCups, "ppdFindOption");
    GET_LIB_FN_OR_EXIT_FAILURE(ppdMarkDefaults_fn, libCups, "ppdMarkDefaults");
    GET_LIB_FN_OR_EXIT_FAILURE(ppdOpenFile_fn, libCups, "ppdOpenFile");
    GET_LIB_FN_OR_EXIT_FAILURE(cupsFreeOptions_fn, libCups, "cupsFreeOptions");
    GET_LIB_FN_OR_EXIT_FAILURE(cupsParseOptions_fn, libCups, "cupsParseOptions");
    GET_LIB_FN_OR_EXIT_FAILURE(cupsMarkOptions_fn, libCups, "cupsMarkOptions");
    GET_LIB_FN_OR_EXIT_FAILURE(cupsRasterOpen_fn, libCupsImage, "cupsRasterOpen");
    GET_LIB_FN_OR_EXIT_FAILURE(cupsRasterReadHeade2r_fn, libCupsImage, "cupsRasterReadHeader2");
    GET_LIB_FN_OR_EXIT_FAILURE(cupsRasterReadPixels_fn, libCupsImage, "cupsRasterReadPixels");
    GET_LIB_FN_OR_EXIT_FAILURE(cupsRasterClose_fn, libCupsImage, "cupsRasterClose");
#endif

    if (argc < 6 || argc > 7) {
        fputs("ERROR: rastertoepsonsimple job-id user title copies options [file]\n", stderr);

#ifdef RPMBUILD
        dlclose(libCupsImage);
        dlclose(libCups);
#endif

        return EXIT_FAILURE;
    }

    if (argc == 7) {
        if ((fd = open(argv[6], O_RDONLY)) == -1) {
            perror("ERROR: Unable to open raster file - ");
            sleep(1);

#ifdef RPMBUILD
            dlclose(libCupsImage);
            dlclose(libCups);
#endif

            return EXIT_FAILURE;
        }
    } else {
        fd = 0;
    }

    initializeSettings(argv[5], &settings);

    jobSetup();
    ras = CUPSRASTEROPEN(fd, CUPS_RASTER_READ);

    page = 0;

    while (CUPSRASTERREADHEADER2(ras, &header)) {
        t_bufferscan *bs = NULL;
        int emptyLinesDiscardedCount = 0;
        int insideTopMargin = TRUE;

        fprintf(
            stderr,
            "Raster page header: (cupsHeight = %d, cupsBytesPerLine = %d)\n",
            header.cupsHeight,
            header.cupsBytesPerLine
        );

        outputCommand(printerInitializeCommand);

        if ((header.cupsHeight == 0) || (header.cupsBytesPerLine == 0)) {
            break;
        }
        bytesPerScanline = header.cupsBytesPerLine;

        page++;
        fprintf(stderr, "PAGE: %d %d\n", page, header.NumCopies);

        rasterData = realloc(rasterData, bytesPerScanline);
        if (rasterData == NULL) {
            CLEANUP;
            return EXIT_FAILURE;
        }

        emptyLinePattern = realloc(emptyLinePattern, bytesPerScanline);
        if (emptyLinePattern == NULL) {
            CLEANUP;
            return EXIT_FAILURE;
        }
        memset(emptyLinePattern, 0, bytesPerScanline);

        bs = bufferscan_new(
            bytesPerScanline,
            MIN(header.cupsHeight, 1595), // limited to 200mm, ~128kB
            settings.doubleMode,
            stdout
        );
        if (!bs) {
            CLEANUP;
            return EXIT_FAILURE;
        }

        for (y = 0; y < header.cupsHeight; y++) {
            unsigned bytes;

            memset(rasterData, 0, bytesPerScanline);

            if ((bytes = CUPSRASTERREADPIXELS(ras, rasterData, bytesPerScanline)) < 1) {
                break;
            }
            if (bytes != bytesPerScanline) {
                CLEANUP;
                return EXIT_FAILURE;
            }

            if (memcmp(rasterData, emptyLinePattern, bytes) == 0) {
                if (insideTopMargin) {
                    if (settings.marginReductionType & topMarginReduction) {
                        continue;
                    }
                } else {
                    if (settings.marginReductionType & bothMarginReduction) {
                        ++emptyLinesDiscardedCount;
                        continue;
                    }
                }
            }
            insideTopMargin = FALSE;

            if (emptyLinesDiscardedCount > 0) {
                while (emptyLinesDiscardedCount--) {
                    bufferscan_addline(bs, emptyLinePattern);
                }
            }

            bufferscan_addline(bs, rasterData);
        }

        bufferscan_flush(bs);
        bufferscan_dispose(bs);
        bs = NULL;

        endPage(settings);
    }

    endJob(settings);

    CLEANUP;

    if (page == 0) {
        fputs("ERROR: No pages found!\n", stderr);
    } else {
        fputs("INFO: Ready to print.\n", stderr);
    }

    fputs("DEBUG: rastertosimple filter finished.\n", stderr);

    return (page == 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
