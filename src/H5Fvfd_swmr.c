/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the root of the source code       *
 * distribution tree, or in https://support.hdfgroup.org/ftp/HDF5/releases.  *
 * If you do not have access to either file, you may request a copy from     *
 * help@hdfgroup.org.                                                        *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*-------------------------------------------------------------------------
 *
 * Created:             H5Fvfd_swmr.c
 *                      Oct 10 2019
 *
 * Purpose:             Functions for VFD SWMR.
 *
 *-------------------------------------------------------------------------
 */

/****************/
/* Module Setup */
/****************/

#include "H5Fmodule.h"          /* This source code file is part of the H5F module */


/***********/
/* Headers */
/***********/
#include "H5private.h"          /* Generic Functions                        */
#include "H5Aprivate.h"         /* Attributes                               */
#include "H5ACprivate.h"        /* Metadata cache                           */
#include "H5CXprivate.h"        /* API Contexts                             */
#include "H5Dprivate.h"         /* Datasets                                 */
#include "H5Eprivate.h"         /* Error handling                           */
#include "H5Fpkg.h"             /* File access                              */
#include "H5FDprivate.h"        /* File drivers                             */
#include "H5Gprivate.h"         /* Groups                                   */
#include "H5Iprivate.h"         /* IDs                                      */
#include "H5Lprivate.h"         /* Links                                    */
#include "H5MFprivate.h"        /* File memory management                   */
#include "H5MVprivate.h"        /* File memory management for VFD SWMR      */
#include "H5MMprivate.h"        /* Memory management                        */
#include "H5Pprivate.h"         /* Property lists                           */
#include "H5SMprivate.h"        /* Shared Object Header Messages            */
#include "H5Tprivate.h"         /* Datatypes                                */


/****************/
/* Local Macros */
/****************/

/* Remove an entry from the doubly linked list */
#define H5F__LL_REMOVE(entry_ptr, head_ptr, tail_ptr)               \
{                                                                   \
    if((head_ptr) == (entry_ptr)) {                                 \
          (head_ptr) = (entry_ptr)->next;                           \
          if((head_ptr) != NULL )                                   \
             (head_ptr)->prev = NULL;                               \
    } else                                                          \
        (entry_ptr)->prev->next = (entry_ptr)->next;                \
    if((tail_ptr) == (entry_ptr)) {                                 \
        (tail_ptr) = (entry_ptr)->prev;                             \
        if((tail_ptr) != NULL)                                      \
            (tail_ptr)->next = NULL;                                \
    } else                                                          \
        (entry_ptr)->next->prev = (entry_ptr)->prev;                \
    entry_ptr->next = NULL;                                         \
    entry_ptr->prev = NULL;                                         \
} /* H5F__LL_REMOVE() */

/* Append an entry to the doubly linked list */
#define H5F__LL_APPEND(entry_ptr, head_ptr, tail_ptr)               \
{                                                                   \
    if((head_ptr) == NULL ) {                                       \
       (head_ptr) = (entry_ptr);                                    \
       (tail_ptr) = (entry_ptr);                                    \
    }                                                               \
    else {                                                          \
       (tail_ptr)->next = (entry_ptr);                              \
       (entry_ptr)->prev = (tail_ptr);                              \
       (tail_ptr) = (entry_ptr);                                    \
    }                                                               \
} /* H5F__LL_APPEND() */

/* Prepend an entry to the doubly linked list */ 
#define H5F__LL_PREPEND(entry_ptr, head_ptr, tail_ptr)              \
{                                                                   \
    if((head_ptr) == NULL) {                                        \
       (head_ptr) = (entry_ptr);                                    \
       (tail_ptr) = (entry_ptr);                                    \
    } else {                                                        \
       (head_ptr)->prev = (entry_ptr);                              \
       (entry_ptr)->next = (head_ptr);                              \
       (head_ptr) = (entry_ptr);                                    \
    }                                                               \
} /* H5F__LL_PREPEND() */

/* Insert an entry after the predecessor entry "prec_ptr" on the EOT queue */
#define H5F_EOT_INSERT_AFTER(entry_ptr, prec_ptr, head_ptr, tail_ptr)       \
{                                                                           \
    /* The list is empty or has no predecessor -- prepend */                \
    if(prec_ptr == NULL)                                                    \
        H5F__LL_PREPEND(entry_ptr, head_ptr, tail_ptr)                      \
                                                                            \
    /* The predecessor entry is at head of list -- append */                \
    else if(prec_ptr->prev == NULL)                                         \
        H5F__LL_APPEND(entry_ptr, head_ptr, tail_ptr)                       \
                                                                            \
    /* The predecessor entry is in the body of list -- insert after it */   \
    else                                                                    \
    {                                                                       \
        entry_ptr->prev = prec_ptr;                                         \
        entry_ptr->next = prec_ptr->next;                                   \
        prec_ptr->next->prev = entry_ptr;                                   \
        prec_ptr->next = entry_ptr;                                         \
    }                                                                       \
} /* H5F_EOT_INSERT_AFTER() */

/********************/
/* Local Prototypes */
/********************/

static herr_t H5F__vfd_swmr_update_end_of_tick_and_tick_num(H5F_t *f, hbool_t incr_tick_num);
static herr_t H5F__vfd_swmr_construct_write_md_hdr(H5F_t *f, uint32_t num_entries);
static herr_t H5F__vfd_swmr_construct_write_md_idx(H5F_t *f, uint32_t num_entries, struct H5FD_vfd_swmr_idx_entry_t index[]);
static herr_t H5F__idx_entry_cmp(const void *_entry1, const void *_entry2);
static herr_t H5F__vfd_swmr_writer__create_index(H5F_t * f);
static herr_t H5F__vfd_swmr_writer__wait_a_tick(H5F_t *f);

/*********************/
/* Package Variables */
/*********************/

/* 
 * Globals for VFD SWMR 
 */

hbool_t vfd_swmr_writer_g = FALSE;      /* Is this the VFD SWMR writer */
struct timespec end_of_tick_g;          /* The current end_of_tick */

unsigned int vfd_swmr_api_entries_g = 0;/* Times the library was entered
                                         * and re-entered minus the times
                                         * it was exited.  We only perform
                                         * the end-of-tick processing
                                         * on the 0->1 and 1->0
                                         * transitions.
                                         */
/*
 *  The head and tail of the end of tick queue (EOT queue) for files opened in either
 *  VFD SWMR write or VFD SWMR read mode
 */
H5F_vfd_swmr_eot_queue_entry_t *vfd_swmr_eot_queue_head_g = NULL;
H5F_vfd_swmr_eot_queue_entry_t *vfd_swmr_eot_queue_tail_g = NULL;

/*******************/
/* Local Variables */
/*******************/

/* Declare a free list to manage the H5F_vfd_swmr_dl_entry_t struct */
H5FL_DEFINE(H5F_vfd_swmr_dl_entry_t);

/* Declare a free list to manage the H5F_vfd_swmr_eot_queue_entry_t struct */
H5FL_DEFINE(H5F_vfd_swmr_eot_queue_entry_t);


/*-------------------------------------------------------------------------
 *
 * Function:    H5F_vfd_swmr_init
 *
 * Purpose:     Initialize globals and the corresponding fields in 
 *              file pointer.
 *
 *              For both VFD SWMR writer and reader:
 *
 *                  --set vfd_swmr_g to TRUE
 *                  --set vfd_swmr_file_g to f
 *                  --set end_of_tick to the current time + tick length
 *
 *              For VFD SWMR writer:
 *
 *                  --set vfd_swmr_writer_g to TRUE
 *                  --set tick_num_g to 1
 *                  --create the metadata file
 *                  --when opening an existing HDF5 file, write header and 
 *                    empty index in the metadata file
 *
 *              For VFD SWMR reader:
 *
 *                  --set vfd_swmr_writer_g to FALSE
 *                  --set tick_num_g to the current tick read from the 
 *                    metadata file
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Vailin Choi -- 11/??/18
 *
 * Changes:     None.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_vfd_swmr_init(H5F_t *f, hbool_t file_create)
{
    hsize_t md_size;                /* Size of the metadata file */
    haddr_t md_addr;                /* Address returned from H5MV_alloc() */
    herr_t ret_value = SUCCEED;     /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    HDassert(H5F_VFD_SWMR_CONFIG(f));

    f->shared->vfd_swmr = TRUE;

    if(H5F_INTENT(f) & H5F_ACC_RDWR) {

        HDassert(f->shared->vfd_swmr_config.writer);

        f->shared->vfd_swmr_writer = TRUE;
        f->shared->tick_num = 1;

        if ( H5PB_vfd_swmr__set_tick(f) < 0 )

            HGOTO_ERROR(H5E_FILE, H5E_SYSTEM, FAIL, \
                        "Can't update page buffer current tick")

        /* Create the metadata file */
        if ( ((f->shared->vfd_swmr_md_fd = 
               HDopen(f->shared->vfd_swmr_config.md_file_path, O_CREAT|O_RDWR, 
                      H5_POSIX_CREATE_MODE_RW))) < 0 )

            HGOTO_ERROR(H5E_FILE, H5E_CANTOPENFILE, FAIL, \
                        "unable to create the metadata file")

        md_size = (hsize_t)f->shared->vfd_swmr_config.md_pages_reserved * 
                  f->shared->fs_page_size;

        /* Make sure that the free-space manager for the metadata file is initialized */
        if((md_addr = H5MV_alloc(f, md_size)) == HADDR_UNDEF)
            HGOTO_ERROR(H5E_FILE, H5E_WRITEERROR, FAIL, \
                            "error in allocating md_pages_reserved from the metadata file")
        HDassert(H5F_addr_eq(md_addr, H5FD_MD_HEADER_OFF));

        /* Set the metadata file size to md_pages_reserved */
        if ( -1 == HDftruncate(f->shared->vfd_swmr_md_fd, (HDoff_t)md_size) )

            HGOTO_ERROR(H5E_FILE, H5E_WRITEERROR, FAIL, \
                        "truncate fail for the metadata file")

        /* Set eof for metadata file to md_pages_reserved */
        f->shared->vfd_swmr_md_eoa = (haddr_t)md_size;

        /* When opening an existing HDF5 file, create header and empty 
         * index in the metadata file 
         */
        if ( !file_create ) {

            if ( H5F__vfd_swmr_construct_write_md_hdr(f, 0) < 0 )

                HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, \
                            "fail to create header in md")

            if ( H5F__vfd_swmr_construct_write_md_idx(f, 0, NULL) < 0 )

                HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, \
                            "fail to create index in md")
        }

    } else { /* VFD SWMR reader  */

        HDassert(!f->shared->vfd_swmr_config.writer);

        f->shared->vfd_swmr_writer = FALSE;

        HDassert(f->shared->mdf_idx == NULL);

        /* allocate an index to save the initial index */
        if ( H5F__vfd_swmr_writer__create_index(f) < 0 )

           HGOTO_ERROR(H5E_FILE, H5E_CANTALLOC, FAIL, \
                       "unable to allocate metadata file index")


        /* Set tick_num_g to the current tick read from the metadata file */
        f->shared->mdf_idx_entries_used = f->shared->mdf_idx_len;
        if ( H5FD_vfd_swmr_get_tick_and_idx(f->shared->lf, FALSE, 
                                            &f->shared->tick_num, 
                                            &(f->shared->mdf_idx_entries_used),
                                            f->shared->mdf_idx) < 0 )

            HGOTO_ERROR(H5E_FILE, H5E_CANTLOAD, FAIL, \
                        "unable to load/decode metadata file")

#if 0 /* JRM */
        HDfprintf(stderr, 
                 "##### initialized index: tick/used/len = %lld/%d/%d #####\n",
                 f->shared->tick_num, f->shared->mdf_idx_entries_used,
                 f->shared->mdf_idx_len);
#endif /* JRM */
    }

    /* Update end_of_tick */
    if ( H5F__vfd_swmr_update_end_of_tick_and_tick_num(f, FALSE) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, "unable to update end of tick")

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5F_vfd_swmr_init() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5F_vfd_swmr_close_or_flush
 *
 * Purpose:     Used by the VFD SWMR writer when the HDF5 file is closed 
 *              or flushed:
 *
 *              1) For file close:
 *                  --write header and an empty index to the metadata file
 *                  --increment tick_num
 *                  --close the metadata file
 *                  --unlink the metadata file
 *                  --close the free-space manager for the metadata file
 *
 *              2) For file flush:
 *                  --write header and an empty index to the metadata file
 *                  --increment tick_num 
 *                  --start a new tick (??check with JM for sure)
 *                    ??update end_of_tick
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Vailin Choi -- 11/??/18
 *
 * Changes:     None.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_vfd_swmr_close_or_flush(H5F_t *f, hbool_t closing)
{
    H5F_vfd_swmr_dl_entry_t *curr, *next;
    herr_t      ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    HDassert(f->shared->vfd_swmr_writer);
    HDassert(f->shared->vfd_swmr_md_fd >= 0);

    /* Write empty index to the md file */
    if ( H5F__vfd_swmr_construct_write_md_idx(f, 0, NULL) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, "fail to create index in md")


    /* Write header to the md file */
    if ( H5F__vfd_swmr_construct_write_md_hdr(f, 0) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, "fail to create header in md")

    /* Increment tick_num */
    ++f->shared->tick_num;

    if ( closing ) { /* For file close */

        /* Close the md file */
        if(HDclose(f->shared->vfd_swmr_md_fd) < 0)

            HGOTO_ERROR(H5E_FILE, H5E_CANTCLOSEFILE, FAIL, \
                        "unable to close the metadata file")
        f->shared->vfd_swmr_md_fd = -1;

        /* Unlink the md file */
        if ( HDunlink(f->shared->vfd_swmr_config.md_file_path) < 0 )

            HGOTO_ERROR(H5E_FILE, H5E_CANTREMOVE, FAIL, \
                        "unable to unlink the metadata file")

        /* Close the free-space manager for the metadata file */
        if ( H5MV_close(f) < 0 )

            HGOTO_ERROR(H5E_FILE, H5E_CANTRELEASE, FAIL, \
                "unable to close the free-space manager for the metadata file")

        /* Free the delayed list */ 
        curr = f->shared->dl_head_ptr;

        while ( curr != NULL ) {

            next = curr->next;
            curr = H5FL_FREE(H5F_vfd_swmr_dl_entry_t, curr);
            curr = next;

        } /* end while */

        f->shared->dl_head_ptr = f->shared->dl_tail_ptr = NULL;

    } else { /* For file flush */

        /* Update end_of_tick */
        if ( H5F__vfd_swmr_update_end_of_tick_and_tick_num(f, TRUE) < 0 )

            HDONE_ERROR(H5E_FILE, H5E_CANTSET, FAIL, \
                        "unable to update end of tick")
    }

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5F_vfd_swmr_close_or_flush() */



/*-------------------------------------------------------------------------
 *
 * Function: H5F_update_vfd_swmr_metadata_file()
 *
 * Purpose:  Update the metadata file with the input index
 *
 *           --Sort index
 *
 *           --For each non-null entry_ptr in the index entries:
 *               --Insert previous image of the entry onto the delayed list
 *               --Allocate space for the entry in the metadata file
 *               --Compute checksum
 *               --Update index entry
 *               --Write the entry to the metadata file
 *               --Set entry_ptr to NULL
 *
 *           --Construct on disk image of the index and write index to the 
 *             metadata file
 *
 *           --Construct on disk image of the header and write header to 
 *             the metadata file
 *
 *           --Release time out entries from the delayed list to the 
 *             free-space manager
 *
 * Return:   SUCCEED/FAIL
 *
 * Programmer: Vailin Choi  11/??/18
 *
 * Changes:  None.
 *
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_update_vfd_swmr_metadata_file(H5F_t *f, uint32_t num_entries, 
    H5FD_vfd_swmr_idx_entry_t index[])
{
    H5F_vfd_swmr_dl_entry_t *prev;          /* Points to the previous entry 
                                             * in the delayed list 
                                             */
    H5F_vfd_swmr_dl_entry_t *dl_entry;      /* Points to an entry in the 
                                             * delayed list 
                                             */
    haddr_t md_addr;                        /* Address in the metadata file */
    unsigned i;                             /* Local index variable */
    herr_t ret_value = SUCCEED;             /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sort index entries by increasing offset in the HDF5 file */
    if ( num_entries ) {

        HDqsort(index, num_entries, sizeof(H5FD_vfd_swmr_idx_entry_t), 
                H5F__idx_entry_cmp);
    }

    /* For each non-null entry_ptr in the index:
     *
     *  --Insert previous image of the entry (if exists) to the 
     *    beginning of the delayed list
     *
     *  --Allocate space for the entry in the metadata file 
     *
     *  --Compute checksum, update the index entry, write entry to 
     *    the metadata file
     *
     *  --Set entry_ptr to NULL
     */
    for ( i = 0; i < num_entries; i++ ) {

        if ( index[i].entry_ptr != NULL ) {

            /* Prepend previous image of the entry to the delayed list */
            if ( index[i].md_file_page_offset ) {

                if ( NULL == (dl_entry = H5FL_CALLOC(H5F_vfd_swmr_dl_entry_t)))

                    HGOTO_ERROR(H5E_FILE, H5E_CANTALLOC, FAIL, \
                                "unable to allocate the delayed entry")

                dl_entry->hdf5_page_offset = index[i].hdf5_page_offset;
                dl_entry->md_file_page_offset = index[i].md_file_page_offset;
                dl_entry->length = index[i].length;
                dl_entry->tick_num = f->shared->tick_num;

                H5F__LL_PREPEND(dl_entry, f->shared->dl_head_ptr, f->shared->dl_tail_ptr);
                f->shared->dl_len++;
            }

            /* Allocate space for the entry in the metadata file */
            if((md_addr = H5MV_alloc(f, index[i].length)) == HADDR_UNDEF)

                HGOTO_ERROR(H5E_FILE, H5E_WRITEERROR, FAIL, \
                            "error in allocating space from the metadata file")

            /* Compute checksum and update the index entry */
            index[i].md_file_page_offset = md_addr/f->shared->fs_page_size;
            index[i].chksum = H5_checksum_metadata(index[i].entry_ptr, 
                                                 (size_t)(index[i].length), 0);

#if 0 /* JRM */
            HDfprintf(stderr, 
       "writing index[%d] fo/mdfo/l/chksum/fc/lc = %lld/%lld/%ld/%lx/%lx/%lx\n",
                    i,
                      index[i].hdf5_page_offset,
                      index[i].md_file_page_offset,
                      index[i].length,
                      index[i].chksum,
                      (((char*)(index[i].entry_ptr))[0]),
                      (((char*)(index[i].entry_ptr))[4095]));

            HDassert(md_addr == index[i].md_file_page_offset * 
                                f->shared->fs_page_size);
            HDassert(f->shared->fs_page_size == 4096);
#endif /* JRM */

            /* Seek and write the entry to the metadata file */
            if ( HDlseek(f->shared->vfd_swmr_md_fd, (HDoff_t)md_addr, 
                         SEEK_SET) < 0)

                HGOTO_ERROR(H5E_FILE, H5E_SEEKERROR, FAIL, \
                            "unable to seek in the metadata file")

            if ( HDwrite(f->shared->vfd_swmr_md_fd, index[i].entry_ptr, 
                         index[i].length) != index[i].length )

                HGOTO_ERROR(H5E_FILE, H5E_WRITEERROR, FAIL, \
                 "error in writing the page/multi-page entry to metadata file")

            /* Set entry_ptr to NULL */
            index[i].entry_ptr = NULL;

        } /* end if */
    } /* end for */

    /* Construct and write index to the metadata file */
    if ( H5F__vfd_swmr_construct_write_md_idx(f, num_entries, index) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, \
                    "fail to construct & write index to md")

    /* Construct and write header to the md file */
    if ( H5F__vfd_swmr_construct_write_md_hdr(f, num_entries) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, \
                    "fail to construct & write header to md")

    /* 
     * Release time out entries from the delayed list by scanning the 
     * list from the bottom up:
     *
     *      --release to the metadata file free space manager all index 
     *        entries that have resided on the list for more than 
     *        max_lag ticks
     *
     *      --remove the associated entries from the list
     */
     dl_entry = f->shared->dl_tail_ptr;

     while ( dl_entry != NULL ) {

        prev = dl_entry->prev;

        /* max_lag is at least 3 */
        if ( ( f->shared->tick_num > f->shared->vfd_swmr_config.max_lag ) &&
             ( dl_entry->tick_num <= 
               f->shared->tick_num - f->shared->vfd_swmr_config.max_lag ) ) {

            if ( H5MV_free(f, dl_entry->md_file_page_offset * 
                           f->shared->fs_page_size, dl_entry->length) < 0 )

                HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, \
                            "unable to flush clean entry")

            /* Remove the entry from the delayed list */
            H5F__LL_REMOVE(dl_entry, f->shared->dl_head_ptr, f->shared->dl_tail_ptr)
            f->shared->dl_len--;

            /* Free the delayed entry struct */
            H5FL_FREE(H5F_vfd_swmr_dl_entry_t, dl_entry);

        } else {

            break;
        }

        dl_entry = prev;

    } /* end while */

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* end H5F_update_vfd_swmr_metadata_file() */


/*-------------------------------------------------------------------------
 *
 * Function: H5F_vfd_swmr_writer__delay_write
 *
 * Purpose:  Given the base address of a page of metadata, or of a multi-
 *           page metadata entry, determine whether the write must be 
 *           delayed.
 *
 *           At the conceptual level, the VFD SWMR writer must delay the 
 *           write of any metadata page or multi-page metadata that 
 *           overwrites an existing metadata page or multi-page metadata 
 *           entry until it has appeared in the metadata file index for 
 *           at least max_lag ticks.  Since the VFD SWMR reader goes 
 *           to the HDF5 file for any piece of metadata not listed in 
 *           the metadata file index, failure to delay such writes can 
 *           result in message from the future bugs.
 *
 *           The easy case is pages or multi-page metadata entries
 *           have just been allocated.  Obviously, these can be written 
 *           immediately.  This case is tracked and tested by the page 
 *           buffer proper.
 *
 *           This routine looks up the supplied page in the metadata file 
 *           index.
 *
 *           If the entry doesn't exist, the function sets 
 *           *delay_write_until_ptr to the current tick plus max_lag.
 *
 *           If the entry exists, the function sets *delay_write_until_ptr
 *           equal to the entries delayed flush field if it is greater than
 *           or equal to the current tick, or zero otherwise.
 *
 * Return:   SUCCEED/FAIL
 *
 * Programmer: John Mainzer 11/4/18
 *
 * Changes:  None.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_vfd_swmr_writer__delay_write(H5F_t *f, uint64_t page, 
    uint64_t * delay_write_until_ptr)
{
    int32_t top = -1;
    int32_t bottom = 0;
    int32_t probe;
    uint64_t delay_write_until = 0;
    H5FD_vfd_swmr_idx_entry_t * ie_ptr = NULL;
    H5FD_vfd_swmr_idx_entry_t * idx = NULL;
    herr_t ret_value = SUCCEED;              /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->vfd_swmr);
    HDassert(f->shared->vfd_swmr_writer);

    idx = f->shared->mdf_idx;

    HDassert((idx) ||( f->shared->tick_num <= 1));

    /* do a binary search on the metadata file index to see if
     * it already contains an entry for *pbe_ptr.
     */

    ie_ptr = NULL;

    if ( idx ) {

        top = f->shared->mdf_idx_entries_used - 1;
        bottom = 0;
    }

    while ( top >= bottom ) {

        HDassert(idx);

        probe = (top + bottom) / 2;

        if ( idx[probe].hdf5_page_offset < page ) {

            bottom = probe + 1;

        } else if ( idx[probe].hdf5_page_offset > page ) {

            top = probe - 1;

        } else { /* found it */

            ie_ptr = idx + probe;
            bottom = top + 1; /* to exit loop */
        }
    }

    if ( ie_ptr ) {

        if ( ie_ptr->delayed_flush >= f->shared->tick_num ) {

             delay_write_until = ie_ptr->delayed_flush;
        }
    } else {

        delay_write_until = f->shared->tick_num +
                            f->shared->vfd_swmr_config.max_lag;
    }

    if ( ( delay_write_until != 0 ) &&
          ( ! ( ( delay_write_until >= f->shared->tick_num ) &&
                ( delay_write_until <=
                   (f->shared->tick_num + f->shared->vfd_swmr_config.max_lag) )
               )
           )
         )
        HGOTO_ERROR(H5E_PAGEBUF, H5E_SYSTEM, FAIL, \
                    "VFD SWMR write delay out of range")

    *delay_write_until_ptr = delay_write_until;
  
done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5F_vfd_swmr_writer__delay_write() */


/*-------------------------------------------------------------------------
 *
 * Function: H5F_vfd_swmr_writer__prep_for_flush_or_close
 *
 * Purpose:  In the context of the VFD SWMR writer, two issues must be 
 *           addressed before the page buffer can be flushed -- as is 
 *           necessary on both HDF5 file flush or close:
 *
 *           1) We must force an end of tick so as to clean the tick list
 *              in the page buffer.
 *              
 *           2) If the page buffer delayed write list is not empty, we 
 *              must repeatedly wait a tick and then run the writer end 
 *              of tick function until the delayed write list drains.
 *
 *           This function manages these details.
 *
 * Return:   SUCCEED/FAIL
 *
 * Programmer: John Mainzer 11/27/18
 *
 * Changes:  None.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_vfd_swmr_writer__prep_for_flush_or_close(H5F_t *f)
{
    herr_t ret_value = SUCCEED;              /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->vfd_swmr);
    HDassert(f->shared->vfd_swmr_writer);
    HDassert(f->shared->pb_ptr);

    /* since we are about to flush the page buffer, force and end of
     * tick so as to avoid attempts to flush entries on the page buffer 
     * tick list that were modified during the current tick.
     */
    if ( H5F_vfd_swmr_writer_end_of_tick(f) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_SYSTEM, FAIL, \
                    "H5F_vfd_swmr_writer_end_of_tick() failed.")

    while(f->shared->pb_ptr->dwl_len > 0) {

        if(H5F__vfd_swmr_writer__wait_a_tick(f) < 0)

            HGOTO_ERROR(H5E_FILE, H5E_CANTFLUSH, FAIL, "wait a tick failed.")
    }

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5F_vfd_swmr_writer__prep_for_flush_or_close() */


/*-------------------------------------------------------------------------
 *
 * Function: H5F_vfd_swmr_writer_end_of_tick
 *
 * Purpose:  Main routine for managing the end of tick for the VFD 
 *           SWMR writer.  
 *
 *           This function performs all end of tick operations for the 
 *           writer -- specifically:
 * 
 *            1) If requested, flush all raw data to the HDF5 file.
 *
 *               (Not for first cut.)
 *
 *            2) Flush the metadata cache to the page buffer.
 *
 *               Note that we must run a tick after the destruction 
 *               of the metadata cache, since this operation will usually
 *               dirty the first page in the HDF5 file.  However, the 
 *               metadata cache will no longer exist at this point.
 *
 *               Thus, we must check for the existance of the metadata 
 *               cache, and only attempt to flush it if it exists.
 *
 *            3) If this is the first tick (i.e. tick == 1), create the
 *               in memory version of the metadata file index.
 *
 *            4) Scan the page buffer tick list, and use it to update 
 *               the metadata file index, adding or modifying entries as 
 *               appropriate.
 *
 *            5) Scan the metadata file index for entries that can be 
 *               removed -- specifically entries that have been written 
 *               to the HDF5 file more than max_lag ticks ago, and haven't
 *               been modified since. 
 *
 *               (This is an optimization -- address it later)
 *
 *            6) Update the metadata file.  Must do this before we 
 *               release the tick list, as otherwise the page buffer 
 *               entry images may not be available.
 *
 *            7) Release the page buffer tick list.
 *
 *            8) Release any delayed writes whose delay has expired.
 *
 *            9) Increment the tick, and update the end of tick.
 *
 *           In passing, generate log entries as appropriate.
 *
 * Return:   SUCCEED/FAIL
 *
 * Programmer: John Mainzer 11/4/18
 *
 * Changes:  None.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_vfd_swmr_writer_end_of_tick(H5F_t *f)
{
    int32_t idx_entries_added = 0;
    int32_t idx_entries_modified = 0;
    int32_t idx_ent_not_in_tl = 0;
    int32_t idx_ent_not_in_tl_flushed = 0;
    herr_t ret_value = SUCCEED;              /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* When called from FUNC ENTER/EXIT, get the first entry on the EOT queue */
    if(f == NULL)
        f = vfd_swmr_eot_queue_head_g->vfd_swmr_file;

    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->pb_ptr);
    HDassert(f->shared->vfd_swmr_writer);

  
    /* 1) If requested, flush all raw data to the HDF5 file.
     *
     *    (Not for first cut.)
     */
    if ( f->shared->vfd_swmr_config.flush_raw_data ) {

        HDassert(FALSE);
    }

#if 1
    /* Test to see if b-tree corruption seen in VFD SWMR tests 
     * is caused by client hiding data from the metadata cache.  Do 
     * this by calling H5D_flush_all(), which flushes any cached 
     * dataset storage.  Eventually, we will do this regardless 
     * when the above flush_raw_data flag is set.
     */

    if ( H5D_flush_all(f) < 0 )

        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, \
                    "unable to flush dataset cache")


    if(H5MF_free_aggrs(f) < 0)

        HGOTO_ERROR(H5E_FILE, H5E_CANTRELEASE, FAIL, "can't release file space")


    if ( f->shared->cache ) {

        if ( H5AC_flush(f) < 0 ) 

            HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, \
                        "Can't flush metadata cache to the page buffer")
    }



    if ( H5FD_truncate(f->shared->lf, FALSE) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_WRITEERROR, FAIL, "low level truncate failed")
#endif

    /* 2) If it exists, flush the metadata cache to the page buffer. */
    if ( f->shared->cache ) {

        if ( H5AC_flush(f) < 0 ) 

            HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, \
                        "Can't flush metadata cache to the page buffer")
    }


    /* 3) If this is the first tick (i.e. tick == 1), create the
     *    in memory version of the metadata file index.
     */
    if ( ( f->shared->tick_num == 1 ) &&
         ( H5F__vfd_swmr_writer__create_index(f) < 0 ) )

       HGOTO_ERROR(H5E_FILE, H5E_CANTALLOC, FAIL, \
                   "unable to allocate metadata file index")


    /* 4) Scan the page buffer tick list, and use it to update 
     *    the metadata file index, adding or modifying entries as 
     *    appropriate.
     */
    if ( H5PB_vfd_swmr__update_index(f, &idx_entries_added, 
                                     &idx_entries_modified, 
                                     &idx_ent_not_in_tl, 
                                     &idx_ent_not_in_tl_flushed) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_SYSTEM, FAIL, "can't update MD file index")


    /* 5) Scan the metadata file index for entries that can be 
     *    removed -- specifically entries that have been written 
     *    to the HDF5 file more than max_lag ticks ago, and haven't
     *    been modified since. 
     *
     *    (This is an optimization -- adress it later)
     */


    /* 6) Update the metadata file.  Must do this before we 
     *    release the tick list, as otherwise the page buffer 
     *    entry images may not be available.
     *
     *    Note that this operation will restore the index to 
     *    sorted order.
     */
    if ( (uint32_t)(f->shared->mdf_idx_entries_used + idx_entries_added) > 0 ) {

        if ( H5F_update_vfd_swmr_metadata_file(f, 
                (uint32_t)(f->shared->mdf_idx_entries_used + idx_entries_added),
                f->shared->mdf_idx) < 0 )

            HGOTO_ERROR(H5E_FILE, H5E_SYSTEM, FAIL, "can't update MD file")
    } else {

        if ( H5F_update_vfd_swmr_metadata_file(f, 0, NULL) < 0 )

            HGOTO_ERROR(H5E_FILE, H5E_SYSTEM, FAIL, "can't update MD file")
    }

    /* at this point the metadata file index should be sorted -- update
     * f->shared->mdf_idx_entries_used.
     */
    f->shared->mdf_idx_entries_used += idx_entries_added;

    HDassert(f->shared->mdf_idx_entries_used <= f->shared->mdf_idx_len);

#if 0 /* JRM */
    H5F__vfd_swmr_writer__dump_index(f);
#endif /* JRM */

    /* 7) Release the page buffer tick list. */
    if ( H5PB_vfd_swmr__release_tick_list(f) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_SYSTEM, FAIL, "can't release tick list")


    /* 8) Release any delayed writes whose delay has expired */
    if ( H5PB_vfd_swmr__release_delayed_writes(f) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_SYSTEM, FAIL, "can't release delayed writes")
 

    /* 9) Increment the tick, and update the end of tick. */
    if( f) {

        /* Update end_of_tick */
        if ( H5F__vfd_swmr_update_end_of_tick_and_tick_num(f, TRUE) < 0 )

            HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, \
                        "unable to update end of tick")
    }

    /* Remove the entry from the EOT queue */
    if(H5F_vfd_swmr_remove_entry_eot(f) < 0)
        HDONE_ERROR(H5E_FILE, H5E_CANTCLOSEFILE, FAIL, "unable to remove entry from EOT queue")

     /* Re-insert the entry that corresponds to f onto the EOT queue */
    if(H5F_vfd_swmr_insert_entry_eot(f) < 0)
        HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, "unable to insert entry into the EOT queue")

#if 0 /* JRM */
    HDfprintf(stderr, "*** writer EOT %lld exiting. idx len = %d ***\n", 
              f->shared->tick_num, 
              (int32_t)(f->shared->mdf_idx_entries_used));
#endif /* JRM */
done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* end H5F_vfd_swmr_writer_end_of_tick() */


/*-------------------------------------------------------------------------
 *
 * Function: H5F_vfd_swmr_writer__dump_index
 *
 * Purpose:  Dump a summary of the metadata file index.
 *
 * Return:   SUCCEED/FAIL
 *
 * Programmer: John Mainzer 12/14/19
 *
 * Changes:  None.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_vfd_swmr_writer__dump_index(H5F_t * f)
{
    int i;
    int32_t mdf_idx_len;
    int32_t mdf_idx_entries_used;
    H5FD_vfd_swmr_idx_entry_t * index = NULL;
    herr_t ret_value = SUCCEED;              /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->vfd_swmr);
    HDassert(f->shared->mdf_idx);


    index                = f->shared->mdf_idx;
    mdf_idx_len          = f->shared->mdf_idx_len;
    mdf_idx_entries_used = f->shared->mdf_idx_entries_used;

    HDfprintf(stderr, "\n\nDumping Index:\n\n");
    HDfprintf(stderr, "index len / entries used = %d / %d\n\n", 
              mdf_idx_len, mdf_idx_entries_used);

    for ( i = 0; i < mdf_idx_entries_used; i++ ) {

        HDfprintf(stderr, "%d: %lld %lld %d\n", i, index[i].hdf5_page_offset,
                  index[i].md_file_page_offset, index[i].length);
    }

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* end H5F_vfd_swmr_writer__dump_index() */


/*-------------------------------------------------------------------------
 * Function: H5F_vfd_swmr_reader_end_of_tick
 *
 * Purpose:  Main routine for VFD SWMR reader end of tick operations.
 *           The following operations must be performed:
 *
 *           1) Direct the VFD SWMR reader VFD to load the current header
 *              from the metadata file, and report the current tick.
 *
 *              If the tick reported has not increased since the last 
 *              call, do nothing and exit.
 *
 *           2) If the tick has increased, obtain a copy of the new
 *              index from the VFD SWMR reader VFD, and compare it with
 *              the old index to identify all pages that have been updated
 *              in the previous tick.  
 *
 *              If any such pages or multi-page metadata entries are found:
 *
 *                 a) direct the page buffer to evict any such superceeded
 *                    pages, and 
 *
 *                 b) direct the metadata cache to either evict or refresh
 *                    any entries residing in the superceeded pages.
 *
 *              Note that this operation MUST be performed in this order,
 *              as the metadata cache will refer to the page buffer 
 *              when refreshing entries.
 *
 *           9) Increment the tick, and update the end of tick.
 *
 * Return:   SUCCEED/FAIL
 *
 * Programmer: John Mainzer 12/29/18
 *
 * Changes:  None.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_vfd_swmr_reader_end_of_tick(H5F_t *f)
{
    int pass = 0;
    uint64_t tmp_tick_num = 0;
    H5FD_vfd_swmr_idx_entry_t * tmp_mdf_idx;
    int32_t entries_added = 0;
    int32_t entries_removed = 0;
    int32_t entries_changed = 0;
    int32_t tmp_mdf_idx_len;
    int32_t tmp_mdf_idx_entries_used;
    uint32_t mdf_idx_entries_used;
    herr_t ret_value = SUCCEED;              /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* When called from FUNC ENTER/EXIT, get the first entry on the EOT queue */
    if(f == NULL)
        f = vfd_swmr_eot_queue_head_g->vfd_swmr_file;

    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->pb_ptr);
    HDassert(f->shared->vfd_swmr);
    HDassert(!f->shared->vfd_swmr_writer);
    HDassert(f->shared->lf);
#if 0 /* JRM */
    HDfprintf(stderr, "--- reader EOT entering ---\n");
    HDfprintf(stderr, "--- reader EOT init index used / len = %d / %d ---\n",
              f->shared->mdf_idx_entries_used, f->shared->mdf_idx_len);
#endif /* JRM */
    /* 1) Direct the VFD SWMR reader VFD to load the current header
     *    from the metadata file, and report the current tick.
     *
     *    If the tick reported has not increased since the last
     *    call, do nothing and exit.
     */
    if ( H5FD_vfd_swmr_get_tick_and_idx(f->shared->lf, TRUE, &tmp_tick_num, 
                                        NULL, NULL) < 0 )

        HGOTO_ERROR(H5E_ARGS, H5E_CANTGET, FAIL, \
                    "error in retrieving tick_num from driver")

#if 0 /* JRM */
    HDfprintf(stderr, "--- reader EOT curr/new tick = %lld/%lld ---\n",
              tick_num_g, tmp_tick_num);
#endif /* JRM */

    if ( tmp_tick_num != f->shared->tick_num ) {

        /* swap the old and new metadata file indexes */

        tmp_mdf_idx              = f->shared->old_mdf_idx;
        tmp_mdf_idx_len          = f->shared->old_mdf_idx_len;
        tmp_mdf_idx_entries_used = f->shared->old_mdf_idx_entries_used;

        f->shared->old_mdf_idx              = f->shared->mdf_idx;
        f->shared->old_mdf_idx_len          = f->shared->mdf_idx_len;
        f->shared->old_mdf_idx_entries_used = (int32_t)f->shared->mdf_idx_entries_used;

        f->shared->mdf_idx              = tmp_mdf_idx;
        f->shared->mdf_idx_len          = tmp_mdf_idx_len;
        f->shared->mdf_idx_entries_used = tmp_mdf_idx_entries_used;

        /* if f->shared->mdf_idx is NULL, allocate an index */
        if ( ( f->shared->mdf_idx == NULL ) &&
             ( H5F__vfd_swmr_writer__create_index(f) < 0 ) )

           HGOTO_ERROR(H5E_FILE, H5E_CANTALLOC, FAIL, \
                       "unable to allocate metadata file index")


        mdf_idx_entries_used = (uint32_t)(f->shared->mdf_idx_len);

#if 0 /* JRM */
        HDfprintf(stderr, "--- reader EOT mdf_idx_entries_used = %d ---\n",
                  mdf_idx_entries_used);
#endif /* JRM */

        if ( H5FD_vfd_swmr_get_tick_and_idx(f->shared->lf, FALSE, NULL,
                                            &mdf_idx_entries_used, 
                                            f->shared->mdf_idx) < 0 )

            HGOTO_ERROR(H5E_ARGS, H5E_CANTGET, FAIL, \
                        "error in retrieving tick_num from driver")

        HDassert(mdf_idx_entries_used <= (uint32_t)(f->shared->mdf_idx_len));

        f->shared->mdf_idx_entries_used = mdf_idx_entries_used;

#if 0 /* JRM */
        HDfprintf(stderr, "--- reader EOT index used / len = %d/%d ---\n",
                  f->shared->mdf_idx_entries_used, f->shared->mdf_idx_len);
#endif /* JRM */


        /* if an old metadata file index exists, compare it with the 
         * new index and evict any modified, new, or deleted pages
         * and any associated metadata cache entries.
         *
         * Note that we must do this in two passes -- page buffer first,
         * and then metadata cache.  This is necessary as the metadata 
         * cache may attempt to refresh entries rather than evict them,
         * in which case it may access an entry in the page buffer. 
         */
        pass = 0;
        while ( pass <= 1 ) {

            haddr_t page_addr;
            int32_t i = 0;
            int32_t j = 0;
            H5FD_vfd_swmr_idx_entry_t * new_mdf_idx;
            H5FD_vfd_swmr_idx_entry_t * old_mdf_idx;
            int32_t new_mdf_idx_entries_used;
            int32_t old_mdf_idx_entries_used;

            new_mdf_idx              = f->shared->mdf_idx;
            new_mdf_idx_entries_used = f->shared->mdf_idx_entries_used;

            old_mdf_idx              = f->shared->old_mdf_idx;
            old_mdf_idx_entries_used = f->shared->old_mdf_idx_entries_used;

            while ( ( i < old_mdf_idx_entries_used ) &&
                    ( j < new_mdf_idx_entries_used ) ) {

                if ( old_mdf_idx[i].hdf5_page_offset == 
                     new_mdf_idx[j].hdf5_page_offset ) {

                    if ( old_mdf_idx[i].md_file_page_offset != 
                         new_mdf_idx[j].md_file_page_offset ) {

                        /* the page has been altered -- evict it and 
                         * any contained metadata cache entries.
                         */
                        if ( pass == 0 ) {

                            entries_changed++;

                            page_addr = (haddr_t)
                                (new_mdf_idx[j].hdf5_page_offset *
                                 f->shared->pb_ptr->page_size);
                                    
                            if ( H5PB_remove_entry(f, page_addr) < 0 )

                                HGOTO_ERROR(H5E_FILE, H5E_CANTFLUSH, FAIL, \
                                            "remove page buffer entry failed")
                        } else {

                           if ( H5C_evict_or_refresh_all_entries_in_page(f,
                                               new_mdf_idx[j].hdf5_page_offset, 
                                               tmp_tick_num) < 0 )

                                HGOTO_ERROR(H5E_FILE, H5E_CANTFLUSH, FAIL, \
                                    "evict or refresh stale MDC entries failed")
                        }
                    }
                    i++;
                    j++;

                } else if ( old_mdf_idx[i].hdf5_page_offset < 
                            new_mdf_idx[j].hdf5_page_offset ) {

                   /* the page has been removed from the new version 
                    * of the index.  Evict it and any contained metadata
                    * cache entries.  
                    *
                    * If we are careful about removing entries from the 
                    * the index so as to ensure that they haven't changed 
                    * for several ticks, we can probably omit this.  However,
                    * lets not worry about this for the first cut.
                    */
                    if ( pass == 0 ) {

                        entries_removed++;

                        page_addr = (haddr_t)(old_mdf_idx[i].hdf5_page_offset *
                                              f->shared->pb_ptr->page_size);
                                    
                        if ( H5PB_remove_entry(f, page_addr) < 0 )

                            HGOTO_ERROR(H5E_FILE, H5E_CANTFLUSH, FAIL, \
                                        "remove page buffer entry failed")
                    } else {

                       if ( H5C_evict_or_refresh_all_entries_in_page(f,
                                                old_mdf_idx[i].hdf5_page_offset,
                                                tmp_tick_num) < 0 )

                            HGOTO_ERROR(H5E_FILE, H5E_CANTFLUSH, FAIL, \
                                    "evict or refresh stale MDC entries failed")
                    }

                    i++;

                } else { /* ( old_mdf_idx[i].hdf5_page_offset > */
                         /*   new_mdf_idx[j].hdf5_page_offset ) */

                    /* the page has been added to the index.  No action 
                     * is required.
                     */
                    if ( pass == 0 ) {

                        entries_added++;
                    }
                    j++;
               
                }

                /* sanity checks to verify that the old and new indicies
                 * are sorted as expected.
                 */
                HDassert( ( i == 0 ) ||
                          ( i >= old_mdf_idx_entries_used ) ||
                          ( old_mdf_idx[i - 1].hdf5_page_offset <
                            old_mdf_idx[i].hdf5_page_offset ) );

                HDassert( ( j == 0 ) ||
                          ( j >= new_mdf_idx_entries_used ) ||
                          ( new_mdf_idx[j - 1].hdf5_page_offset <
                            new_mdf_idx[j].hdf5_page_offset ) );

            }

            /* cleanup any left overs in the old index */
            while ( i < old_mdf_idx_entries_used ) {

                /* the page has been removed from the new version of the 
                 * index.  Evict it from the page buffer and also evict any 
                 * contained metadata cache entries
                 */
                if ( pass == 0 ) {

                    entries_removed++;

                    page_addr = (haddr_t)(old_mdf_idx[i].hdf5_page_offset *
                                          f->shared->pb_ptr->page_size);
                                    
                    if ( H5PB_remove_entry(f, page_addr) < 0 )

                        HGOTO_ERROR(H5E_FILE, H5E_CANTFLUSH, FAIL, \
                                        "remove page buffer entry failed")
                } else {

                   if ( H5C_evict_or_refresh_all_entries_in_page(f,
                                               old_mdf_idx[i].hdf5_page_offset,
                                               tmp_tick_num) < 0 )

                        HGOTO_ERROR(H5E_FILE, H5E_CANTFLUSH, FAIL, \
                                "evict or refresh stale MDC entries failed")
                }

                i++;
            }

            pass++;

        } /* while ( pass <= 1 ) */
#if 0 /* JRM */
        HDfprintf(stderr, 
                  "--- reader EOT pre new tick index used/len = %d/ %d ---\n",
                  f->shared->mdf_idx_entries_used, f->shared->mdf_idx_len);
#endif /* JRM */
        /* At this point, we should have evicted or refreshed all stale 
         * page buffer and metadata cache entries.  
         *
         * Start the next tick.
         */
        f->shared->tick_num = tmp_tick_num;

        /* Update end_of_tick */
        if ( H5F__vfd_swmr_update_end_of_tick_and_tick_num(f, FALSE) < 0 )

            HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, \
                        "unable to update end of tick")

    } /* if ( tmp_tick_num != tick_num_g ) */

    /* Remove the entry from the EOT queue */
    if(H5F_vfd_swmr_remove_entry_eot(f) < 0)
        HDONE_ERROR(H5E_FILE, H5E_CANTCLOSEFILE, FAIL, "unable to remove entry from EOT queue")

     /* Re-insert the entry that corresponds to f onto the EOT queue */
    if(H5F_vfd_swmr_insert_entry_eot(f) < 0)
        HGOTO_ERROR(H5E_FILE, H5E_CANTSET, FAIL, "unable to insert entry into the EOT queue")

#if 0 /* JRM */
    HDfprintf(stderr, "--- reader EOT final index used / len = %d / %d ---\n",
              f->shared->mdf_idx_entries_used, f->shared->mdf_idx_len);
    HDfprintf(stderr, "--- reader EOT old index used / len = %d / %d ---\n",
              f->shared->old_mdf_idx_entries_used, f->shared->old_mdf_idx_len);
    HDfprintf(stderr, "--- reader EOT %lld exiting t/a/r/c = %d/%d/%d/%d---\n", 
              f->shared->tick_num, (int32_t)(f->shared->mdf_idx_entries_used),
              entries_added, entries_removed, entries_changed);
#endif /* JRM */

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* end H5F_vfd_swmr_reader_end_of_tick() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5F__vfd_swmr_remove_entry_eot
 *
 * Purpose:     Remove an entry from the EOT queue
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Vailin Choi -- 11/18/2019
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_vfd_swmr_remove_entry_eot(H5F_t *f)
{
    H5F_vfd_swmr_eot_queue_entry_t *curr;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    /* Free the entry on the EOT queue that corresponds to f */
    curr = vfd_swmr_eot_queue_head_g;
    while(curr != NULL) {
        if(curr->vfd_swmr_file == f) {
            H5F__LL_REMOVE(curr, vfd_swmr_eot_queue_head_g, vfd_swmr_eot_queue_tail_g)
            curr = H5FL_FREE(H5F_vfd_swmr_eot_queue_entry_t, curr);
            break;
        }
        curr = curr->next;
    }

    if(vfd_swmr_eot_queue_head_g) {
        vfd_swmr_writer_g = vfd_swmr_eot_queue_head_g->vfd_swmr_writer;
        end_of_tick_g = vfd_swmr_eot_queue_head_g->end_of_tick;
    } else
        vfd_swmr_writer_g = FALSE;

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* H5F_vfd_swmr_remove_entry_eot() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5F_vfd_swmr_insert_entry_eot
 *
 * Purpose:     Insert an entry onto the EOT queue
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Vailin Choi -- 11/18/2019
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_vfd_swmr_insert_entry_eot(H5F_t *f)
{
    H5F_vfd_swmr_eot_queue_entry_t *entry_ptr;    /* An entry on the EOT end of tick queue */
    H5F_vfd_swmr_eot_queue_entry_t *prec_ptr;     /* The predecessor entry on the EOT end of tick queue */
    herr_t      ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Allocate an entry to be inserted onto the EOT queue */
    if (NULL == (entry_ptr = H5FL_CALLOC(H5F_vfd_swmr_eot_queue_entry_t)))
        HGOTO_ERROR(H5E_FILE, H5E_CANTALLOC, FAIL, "unable to allocate the endo of tick queue entry")

    /* Initialize the entry */
    entry_ptr->vfd_swmr_writer = f->shared->vfd_swmr_writer;
    entry_ptr->tick_num = f->shared->tick_num;
    entry_ptr->end_of_tick = f->shared->end_of_tick;
    entry_ptr->vfd_swmr_file = f;
    entry_ptr->next = NULL;
    entry_ptr->prev = NULL;

    /* Found the position to insert the entry on the EOT queue */
    prec_ptr = vfd_swmr_eot_queue_tail_g;

    while (prec_ptr) {
        if(timespeccmp(&prec_ptr->end_of_tick, &entry_ptr->end_of_tick, >))
           prec_ptr = prec_ptr->prev;                                     
        else 
            break;
    }

    /* Insert the entry onto the EOT queue */
    H5F_EOT_INSERT_AFTER(entry_ptr, prec_ptr, vfd_swmr_eot_queue_head_g, vfd_swmr_eot_queue_tail_g);

    /* Set up globals accordinly */
    if(vfd_swmr_eot_queue_head_g) {
        vfd_swmr_writer_g = vfd_swmr_eot_queue_head_g->vfd_swmr_writer;
        end_of_tick_g = vfd_swmr_eot_queue_head_g->end_of_tick;
    } else
        vfd_swmr_writer_g = FALSE;

done:
    FUNC_LEAVE_NOAPI(ret_value)

} /* H5F_vfd_swmr_insert_entry_eot() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5F_dump_eot_queue()
 *
 * Purpose:     Dump the contents of the EOT queue
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Vailin Choi -- 11/18/2019
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5F_dump_eot_queue(void)
{
    int i = 0;
    H5F_vfd_swmr_eot_queue_entry_t *curr;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    if(vfd_swmr_eot_queue_head_g == NULL)
        HDfprintf(stderr, "EOT head is null\n");

    curr = vfd_swmr_eot_queue_head_g;
    while(curr != NULL) {
        ++i;
        HDfprintf(stderr, "%d: vfd_swmr_writer=%d tick_num=%lld, end_of_tick:%lld, %lld, vfd_swmr_file=0x%x\n", 
                  i, curr->vfd_swmr_writer, curr->tick_num, 
                  curr->end_of_tick.tv_sec, curr->end_of_tick.tv_nsec, curr->vfd_swmr_file);

        curr = curr->next;
    }

    FUNC_LEAVE_NOAPI(SUCCEED)

} /* H5F_dump_eot_queue() */

/*
 * Beginning of static functions
 */


/*-------------------------------------------------------------------------
 *
 * Function:    H5F__vfd_swmr_update_end_of_tick_and_tick_num
 *
 * Purpose:     Update end_of_tick (end_of_tick_g, f->shared->end_of_tick)
 *              Update tick_num (tick_num_g, f->shared->tick_num)
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Vailin Choi -- 11/??/18
 *
 * Changes:     None.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5F__vfd_swmr_update_end_of_tick_and_tick_num(H5F_t *f, hbool_t incr_tick_num)
{
    struct timespec curr;               /* Current time in struct timespec */
    struct timespec new_end_of_tick;    /* new end_of_tick in struct timespec */
    long curr_nsecs;                    /* current time in nanoseconds */
    long tlen_nsecs;                    /* tick_len in nanoseconds */
#if 0 /* JRM */
    long end_nsecs;                     /* end_of_tick in nanoseconds */
#endif /* JRM */
    long new_end_nsecs;                 /* new end_of_tick in nanoseconds */
    herr_t ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_STATIC

    /* Get current time in struct timespec */
    if ( HDclock_gettime(CLOCK_MONOTONIC, &curr) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_CANTGET, FAIL, \
                    "can't get time via clock_gettime")

    /* Convert curr to nsecs */
    curr_nsecs = curr.tv_sec * SECOND_TO_NANOSECS + curr.tv_nsec;

    /* Convert tick_len to nanosecs */
    tlen_nsecs = f->shared->vfd_swmr_config.tick_len * TENTH_SEC_TO_NANOSECS;

    /* 
     *  Update tick_num_g, f->shared->tick_num 
     */
    if ( incr_tick_num ) {

        f->shared->tick_num++;

        if ( H5PB_vfd_swmr__set_tick(f) < 0 )

            HGOTO_ERROR(H5E_FILE, H5E_SYSTEM, FAIL, \
                        "Can't update page buffer current tick")
    }

    /* 
     * Update end_of_tick_g, f->shared->end_of_tick
     */
    /* Calculate new end_of_tick */

    /* TODO: The modulo operation is very expensive on most machines -- 
     *       re-work this code so as to avoid it.
     *
     *                                    JRM -- 11/12/18
     */

    new_end_nsecs = curr_nsecs + tlen_nsecs;
    new_end_of_tick.tv_nsec = new_end_nsecs % SECOND_TO_NANOSECS;
    new_end_of_tick.tv_sec = new_end_nsecs / SECOND_TO_NANOSECS;

    /* Update end_of_tick */
    HDmemcpy(&end_of_tick_g, &new_end_of_tick, sizeof(struct timespec));
    HDmemcpy(&f->shared->end_of_tick, &new_end_of_tick, 
             sizeof(struct timespec));

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5F__vfd_swmr_update_end_of_tick_and_tick_num() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5F__vfd_swmr_construct_write_md_hdr
 *
 * Purpose:     Encode and write header to the metadata file.
 *
 *              This is used by the VFD SWMR writer:
 *
 *                  --when opening an existing HDF5 file
 *                  --when closing the HDF5 file
 *                  --after flushing an HDF5 file
 *                  --when updating the metadata file
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Vailin Choi -- 11/??/18
 *
 * Changes:     None.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5F__vfd_swmr_construct_write_md_hdr(H5F_t *f, uint32_t num_entries)
{
    uint8_t image[H5FD_MD_HEADER_SIZE]; /* Buffer for header */
    uint8_t *p = NULL;                  /* Pointer to buffer */
    uint32_t metadata_chksum;           /* Computed metadata checksum value */
    unsigned hdr_size = H5FD_MD_HEADER_SIZE;    /* Size of header and index */
    herr_t ret_value = SUCCEED;        /* Return value */

    FUNC_ENTER_STATIC

    /*
     * Encode metadata file header
     */
    p = image;

    /* Encode magic for header */
    HDmemcpy(p, H5FD_MD_HEADER_MAGIC, (size_t)H5_SIZEOF_MAGIC);
    p += H5_SIZEOF_MAGIC;

    /* Encode page size, tick number, index offset, index length */
    UINT32ENCODE(p, f->shared->fs_page_size);
    UINT64ENCODE(p, f->shared->tick_num);
    UINT64ENCODE(p, hdr_size);
    UINT64ENCODE(p, H5FD_MD_INDEX_SIZE(num_entries));

    /* Calculate checksum for header */
    metadata_chksum = H5_checksum_metadata(image, (size_t)(p - image), 0);

    /* Encode checksum for header */
    UINT32ENCODE(p, metadata_chksum);

    /* Sanity checks on header */
    HDassert((size_t)(p - image == hdr_size));

    /* Set to beginning of the file */
    if ( HDlseek(f->shared->vfd_swmr_md_fd, (HDoff_t)H5FD_MD_HEADER_OFF, SEEK_SET) < 0 )

        HGOTO_ERROR(H5E_VFL, H5E_SEEKERROR, FAIL, \
                    "unable to seek in metadata file")

    /* Write header to the metadata file */
    if ( HDwrite(f->shared->vfd_swmr_md_fd, image, hdr_size) !=  hdr_size )

        HGOTO_ERROR(H5E_FILE, H5E_WRITEERROR, FAIL, \
                    "error in writing header to metadata file")

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5F__vfd_swmr_construct_write_md_hdr() */


/*-------------------------------------------------------------------------

 * Function:    H5F__vfd_swmr_construct_write_md_idx
 *
 * Purpose:     Encode and write index to the metadata file.
 *
 *              This is used by the VFD SWMR writer:
 *
 *                  --when opening an existing HDF5 file
 *                  --when closing the HDF5 file
 *                  --after flushing an HDF5 file
 *                  --when updating the metadata file
 *
 * Return:      Success:        SUCCEED
 *              Failure:        FAIL
 *
 * Programmer:  Vailin Choi -- 11/??/18
 *
 * Changes:     None.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5F__vfd_swmr_construct_write_md_idx(H5F_t *f, uint32_t num_entries, 
    struct H5FD_vfd_swmr_idx_entry_t index[])
{
    uint8_t *image = NULL;      /* Pointer to buffer */
    uint8_t *p = NULL;          /* Pointer to buffer */
    uint32_t metadata_chksum;   /* Computed metadata checksum value */
    unsigned idx_size = H5FD_MD_INDEX_SIZE(num_entries);  /* Size of index */
    unsigned i;                 /* Local index variable */
    herr_t ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_STATIC

    HDassert((num_entries!= 0 && index != NULL) || 
             (num_entries == 0 && index == NULL));

    /* Allocate space for the buffer to hold the index */
    if ( (image = (uint8_t *)HDmalloc(idx_size)) == NULL )

        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, \
                    "memory allocation failed for md index")

    /*
     * Encode metadata file index
     */
    p = image;

    /* Encode magic for index */
    HDmemcpy(p, H5FD_MD_INDEX_MAGIC, (size_t)H5_SIZEOF_MAGIC);
    p += H5_SIZEOF_MAGIC;

    /* Encode tick number */
    UINT64ENCODE(p, f->shared->tick_num);

    /* Encode number of entries in index */
    UINT32ENCODE(p, num_entries);

    /* Encode the index entries */
    for(i = 0; i < num_entries; i++) {
        UINT32ENCODE(p, index[i].hdf5_page_offset); 
        UINT32ENCODE(p, index[i].md_file_page_offset);
        UINT32ENCODE(p, index[i].length); 
        UINT32ENCODE(p, index[i].chksum); 
    }

    /* Calculate checksum for index */
    metadata_chksum = H5_checksum_metadata(image, (size_t)(p - image), 0);

    /* Encode checksum for index */
    UINT32ENCODE(p, metadata_chksum);

    /* Sanity checks on index */
    HDassert((size_t)(p - image ==  idx_size));

    /* Verify the md file descriptor exists */
    HDassert(f->shared->vfd_swmr_md_fd >= 0);

    /* Set to right after the header */
    if ( HDlseek(f->shared->vfd_swmr_md_fd, (HDoff_t)(H5FD_MD_HEADER_OFF + H5FD_MD_HEADER_SIZE), 
                 SEEK_SET) < 0)

        HGOTO_ERROR(H5E_VFL, H5E_SEEKERROR, FAIL, \
                    "unable to seek in metadata file")

    /* Write index to the metadata file */
    if ( HDwrite(f->shared->vfd_swmr_md_fd, image, idx_size) != idx_size )

        HGOTO_ERROR(H5E_FILE, H5E_WRITEERROR, FAIL, \
                    "error in writing index to metadata file")

done:

    if ( image ) {

        HDfree(image);
    }

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5F__vfd_swmr_construct_write_idx() */


/*-------------------------------------------------------------------------
 * Function: H5F__idx_entry_cmp()
 *
 * Purpose:  Callback used by HDqsort to sort entries in the index
 *
 * Return:   0 if the entries are the same
 *           -1 if entry1's offset is less than that of entry2
 *           1 if entry1's offset is greater than that of entry2
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5F__idx_entry_cmp(const void *_entry1, const void *_entry2)
{
    const H5FD_vfd_swmr_idx_entry_t *entry1 = (const H5FD_vfd_swmr_idx_entry_t *)_entry1;
    const H5FD_vfd_swmr_idx_entry_t *entry2 = (const H5FD_vfd_swmr_idx_entry_t *)_entry2;

    int ret_value = 0;          /* Return value */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(entry1);
    HDassert(entry2);

    if(entry1->hdf5_page_offset < entry2->hdf5_page_offset)
        ret_value = -1;
    else if(entry1->hdf5_page_offset > entry2->hdf5_page_offset)
        ret_value = 1;

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5F__idx_entry_cmp() */


/*-------------------------------------------------------------------------
 *
 * Function: H5F__vfd_swmr_writer__create_index
 *
 * Purpose:  Allocate and initialize the index for the VFD SWMR metadata 
 *           file.
 *
 *           In the first cut at VFD SWMR, the index is of fixed size,
 *           as specified by the md_pages_reserved field of the VFD 
 *           SWMR configuration.  If we exceed this size we will simply
 *           abort.  Needless to say, this will have to change in the
 *           production version, but it is good enough for the working
 *           prototype.
 *
 * Return:   SUCCEED/FAIL
 *
 * Programmer: John Mainzer 11/5/18
 *
 * Changes:  None.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5F__vfd_swmr_writer__create_index(H5F_t * f)
{
    int i;
    size_t bytes_available;
    int32_t entries_in_index;
    size_t index_size;
    H5FD_vfd_swmr_idx_entry_t * index = NULL;
    herr_t ret_value = SUCCEED;              /* Return value */

    FUNC_ENTER_STATIC

    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->vfd_swmr);
    HDassert(f->shared->mdf_idx == NULL);
    HDassert(f->shared->mdf_idx_len == 0);
    HDassert(f->shared->mdf_idx_entries_used == 0);

    bytes_available = (size_t)f->shared->fs_page_size * 
                      (size_t)(f->shared->vfd_swmr_config.md_pages_reserved) -
                      H5FD_MD_HEADER_SIZE;

    HDassert(bytes_available > 0);

    entries_in_index = (int32_t)(bytes_available / H5FD_MD_INDEX_ENTRY_SIZE);

    HDassert(entries_in_index > 0);

    index_size = sizeof(H5FD_vfd_swmr_idx_entry_t) * (size_t)entries_in_index;
    index = (H5FD_vfd_swmr_idx_entry_t *)HDmalloc(index_size);

    if ( index == NULL ) 

        HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, \
                    "memory allocation failed for md index")
  
    for ( i = 0; i < entries_in_index; i++ ) {

        index[i].hdf5_page_offset    = 0;
        index[i].md_file_page_offset = 0;
        index[i].length              = 0;
        index[i].chksum              = 0;
        index[i].entry_ptr           = NULL;
        index[i].tick_of_last_change = 0;
        index[i].clean               = FALSE;
        index[i].tick_of_last_flush  = 0;
        index[i].delayed_flush       = 0;
        index[i].moved_to_hdf5_file  = FALSE;
    }

    f->shared->mdf_idx              = index;
    f->shared->mdf_idx_len          = entries_in_index;
    f->shared->mdf_idx_entries_used = 0;

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* end H5F__vfd_swmr_writer__create_index() */


/*-------------------------------------------------------------------------
 *
 * Function: H5F__vfd_swmr_writer__wait_a_tick
 *
 * Purpose:  Before a file that has been opened by a VFD SWMR writer,
 *           all pending delayed writes must be allowed drain.
 *
 *           This function facilitates this by sleeping for a tick, and
 *           the running the writer end of tick function.  
 *
 *           It should only be called as part the flush or close operations.
 *
 * Return:   SUCCEED/FAIL
 *
 * Programmer: John Mainzer 11/23/18
 *
 * Changes:  None.
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5F__vfd_swmr_writer__wait_a_tick(H5F_t *f)
{
    int result;
    struct timespec req;
    struct timespec rem;
    uint64_t tick_in_nsec;
    herr_t ret_value = SUCCEED;              /* Return value */

    FUNC_ENTER_STATIC

    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->vfd_swmr);
    HDassert(f->shared->vfd_swmr_writer);

    tick_in_nsec = f->shared->vfd_swmr_config.tick_len * TENTH_SEC_TO_NANOSECS;
    req.tv_nsec = (long)(tick_in_nsec % SECOND_TO_NANOSECS);
    req.tv_sec = (time_t)(tick_in_nsec / SECOND_TO_NANOSECS);

    result = HDnanosleep(&req, &rem);

    while ( result == -1 ) {

        req.tv_nsec = rem.tv_nsec;
        req.tv_sec = rem.tv_sec;
        result = HDnanosleep(&req, &rem);
    }

    if ( result != 0 )

        HGOTO_ERROR(H5E_FILE, H5E_SYSTEM, FAIL, "HDnanosleep() failed.")
        
    if ( H5F_vfd_swmr_writer_end_of_tick(f) < 0 )

        HGOTO_ERROR(H5E_FILE, H5E_SYSTEM, FAIL, \
                    "H5F_vfd_swmr_writer_end_of_tick() failed.")
    
done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5F__vfd_swmr_writer__wait_a_tick() */
