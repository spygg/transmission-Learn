/******************************************************************************
 * Copyright (c) 2005 Eric Petit
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#ifndef TR_TRANSMISSION_H
#define TR_TRANSMISSION_H 1

#include <inttypes.h>

#define SHA_DIGEST_LENGTH    20
#define MAX_PATH_LENGTH      1024
#define TR_MAX_TORRENT_COUNT 20

/***********************************************************************
 * tr_init
 ***********************************************************************
 * Initializes libtransmission. Returns a obscure handle to be passed to
 * all functions below.
 **********************************************************************/
typedef struct tr_handle_s tr_handle_t;

tr_handle_t * tr_init          ();

/***********************************************************************
 * tr_setBindPort
 ***********************************************************************
 * 
 **********************************************************************/
void          tr_setBindPort   ( tr_handle_t *, int );

/***********************************************************************
 * tr_setUploadLimit
 ***********************************************************************
 * 
 **********************************************************************/
void          tr_setUploadLimit( tr_handle_t *, int );

/***********************************************************************
 * tr_torrentCount
 ***********************************************************************
 * Number of torrents currently open
 **********************************************************************/
int           tr_torrentCount  ( tr_handle_t * );

/***********************************************************************
 * tr_torrentRates
 ***********************************************************************
 * Get the total download and upload rates
 **********************************************************************/
void          tr_torrentRates  ( tr_handle_t *, float *, float * );

/***********************************************************************
 * tr_torrentInit
 ***********************************************************************
 * Opens and parses torrent file at 'path'. If the file exists and is a
 * valid torrent file, returns 0. Returns a non-zero value otherwise.
 **********************************************************************/
int           tr_torrentInit   ( tr_handle_t *, const char * path );

/***********************************************************************
 * tr_torrentScrape
 ***********************************************************************
 * Asks the tracker for the count of seeders and leechers. Returns 0
 * and fills 's' and 'l' if successful. Otherwise returns 1 if the
 * tracker doesn't support the scrape protocol, is unreachable or
 * replied with some error. tr_torrentScrape may block up to 20 seconds
 * before returning.
 **********************************************************************/
int           tr_torrentScrape ( tr_handle_t *, int, int *, int * );

void          tr_torrentSetFolder( tr_handle_t *, int, const char * );
char *        tr_torrentGetFolder( tr_handle_t *, int );

/***********************************************************************
 * tr_torrentStart
 ***********************************************************************
 * Starts downloading into folder 'path'. The download is launched in a
 * seperate thread, therefore tr_torrentStart returns immediately.
 **********************************************************************/
void          tr_torrentStart  ( tr_handle_t *, int );

/***********************************************************************
 * tr_torrentStop
 ***********************************************************************
 * Stops downloading and notices the tracker that we are leaving. May
 * block for up to 3 seconds before giving up.
 **********************************************************************/
void          tr_torrentStop   ( tr_handle_t *, int );

/***********************************************************************
 * tr_torrentStat
 ***********************************************************************
 * Fills the tr_stat_t structure with updated information about a
 * torrent. The interface should call it every 0.5 second or so in order
 * to update itself.
 **********************************************************************/
typedef struct
{
    uint64_t length;                /* Length of the file, in bytes */
    char     name[MAX_PATH_LENGTH]; /* Path to the file */
}
tr_file_t;

typedef struct
{
    /* Path to torrent */
    char        torrent[MAX_PATH_LENGTH];

    /* General info */
    uint8_t     hash[SHA_DIGEST_LENGTH];
    char        name[MAX_PATH_LENGTH];

    /* Tracker info */
    char        trackerAddress[256];
    int         trackerPort;
    char        trackerAnnounce[MAX_PATH_LENGTH];

    /* Pieces info */
    int         pieceSize;
    int         pieceCount;
    uint64_t    totalSize;
    uint8_t   * pieces;

    /* Files info */
    int         fileCount;
    tr_file_t * files;
}
tr_info_t;

typedef struct
{
    tr_info_t * info;

#define TR_STATUS_PAUSE    0x01
#define TR_STATUS_CHECK    0x02
#define TR_STATUS_DOWNLOAD 0x04
#define TR_STATUS_SEED     0x08
#define TR_TRACKER_ERROR   0x20
    int         status;
    char        error[128];

    float       progress;
    float       rateDownload;
    float       rateUpload;
    int         eta;
    int         peersTotal;
    int         peersUploading;
    int         peersDownloading;
    char        pieces[120];
    uint64_t    downloaded;
    uint64_t    uploaded;
}
tr_stat_t;

void          tr_torrentStat   ( tr_handle_t *, int, tr_stat_t * );

/***********************************************************************
 * tr_torrentClose
 ***********************************************************************
 * Frees memory allocated by tr_torrentInit.
 **********************************************************************/
void          tr_torrentClose  ( tr_handle_t *, int );

/***********************************************************************
 * tr_close
 ***********************************************************************
 * Frees memory allocated by tr_init.
 **********************************************************************/
void          tr_close         ( tr_handle_t * );


#ifdef __TRANSMISSION__
#  include "internal.h"
#endif

#endif
