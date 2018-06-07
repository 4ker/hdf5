/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright the Regents of the University of California, through            *
 *  Lawrence Berkeley National Laboratory                                    *
 *  (subject to receipt of any required approvals from U.S. Dept. of Energy).*
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
 * Created:             H5MFfreedspace.c
 *
 * Purpose:             Functions for cache freed space management.
 *
 *-------------------------------------------------------------------------
 */

/****************/
/* Module Setup */
/****************/
#define H5F_FRIEND		/*suppress error about including H5Fpkg	  */
#include "H5MFmodule.h"         /* This source code file is part of the H5MF module */


/***********/
/* Headers */
/***********/
#include "H5private.h"          /* Generic Functions                    */
#include "H5CXprivate.h"        /* API Contexts                         */
#include "H5Eprivate.h"         /* Error handling                       */
#include "H5Fpkg.h"             /* File access				*/
#include "H5MFpkg.h"		/* File memory management		*/


/****************/
/* Local Macros */
/****************/


/******************/
/* Local Typedefs */
/******************/

/* Context for iterator callback */
typedef struct H5MF_freedspace_ctx_t {
    /* Down */
    H5F_t      *f;              /* File where space is being freed */
    int        ring;            /* Metadata cache ring for object who's space is freed */
    H5FD_mem_t alloc_type;      /* File space type for freed space */
    haddr_t    addr;            /* Address for freed space */
    hsize_t    size;            /* Size for freed space */

    /* Up */
    H5MF_freedspace_t *fs;      /* Freespace object created */
} H5MF_freedspace_ctx_t;


/********************/
/* Package Typedefs */
/********************/


/********************/
/* Local Prototypes */
/********************/
static int H5MF__freedspace_create_cb(H5AC_info_t *entry, void *_ctx);


/*****************************/
/* Library Private Variables */
/*****************************/


/*******************/
/* Local Variables */
/*******************/

/* Declare a free list to manage H5MF_freedspace_t objects */
H5FL_DEFINE_STATIC(H5MF_freedspace_t);



/*-------------------------------------------------------------------------
 * Function:    H5MF__freedspace_new
 *
 * Purpose:     Creates new freedspace object and insert it into the cache
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Quincey Koziol
 *              August 16, 2017
 *
 *-------------------------------------------------------------------------
 */
static H5MF_freedspace_t *
H5MF__freedspace_new(H5MF_freedspace_ctx_t *ctx)
{
    H5MF_freedspace_t *fs;                  /* New freedspace entry */
    haddr_t fs_addr;                        /* Address of freedspace entry */
    H5AC_ring_t orig_ring = H5AC_RING_INV;  /* Original ring value */
    H5MF_freedspace_t *ret_value = NULL;    /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(ctx);

    /* Allocate new freedspace object */
    if(NULL == (fs = H5FL_CALLOC(H5MF_freedspace_t)))
        HGOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate freed space entry")

    /* Initialize new freedspace object */
    fs->f           = ctx->f;
    fs->alloc_type  = ctx->alloc_type;
    fs->addr        = ctx->addr;
    fs->size        = ctx->size;
    fs->timestamp   = H5_now_usec();
    fs->next        = NULL;

    /* Allocate a temporary address for the freedspace entry */
    if(HADDR_UNDEF == (fs_addr = H5MF_alloc_tmp(ctx->f, 1)))
        HGOTO_ERROR(H5E_RESOURCE, H5E_CANTALLOC, NULL, "can't allocate temporary space for freed space entry")

    /* Set the ring for the new freedspace entry in the cache */
    H5AC_set_ring(ctx->ring, &orig_ring);

    /* Insert freedspace entry into the cache */
    if(H5AC_insert_entry(ctx->f, H5AC_FREEDSPACE, fs_addr, fs, H5AC__PIN_ENTRY_FLAG) < 0)
        HGOTO_ERROR(H5E_RESOURCE, H5E_CANTINSERT, NULL, "unable to insert freedspace")

    /* Set the return value */
    ret_value = fs;

done:
    /* Reset the ring in the API context */
    if(orig_ring != H5AC_RING_INV)
        H5AC_set_ring(orig_ring, NULL);

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5MF__freedspace_new() */


/*-------------------------------------------------------------------------
 * Function:    H5MF__freedspace_create_cb()
 *
 * Purpose:     Cache iteration callback, creates flush dep
 *
 * Return:      0 on success, non-zero on failure
 *
 * Programmer:  Houjun Tang
 *              June 8, 2017
 *
 *-------------------------------------------------------------------------
 */
static int
H5MF__freedspace_create_cb(H5AC_info_t *entry, void *_ctx)
{
    H5MF_freedspace_ctx_t *ctx = (H5MF_freedspace_ctx_t*)_ctx;   /* Callback context */
    int ret_value = H5_ITER_CONT;       /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(entry);
    HDassert(ctx);

    /* Only create flush dependencies on currently dirty entries in rings that
     *  will be flushed same / earlier, and are not the freed entry
     */
    if(H5F_addr_ne(entry->addr, ctx->addr) && entry->is_dirty && entry->ring <= ctx->ring) {
        int type_id;                    /* Cache client for entry */
        hbool_t create_fd = FALSE;      /* Whther to create the flush dependency */

        /* Retrieve type for entry */
        if((type_id = H5AC_get_entry_type(entry)) < 0)
            HGOTO_ERROR(H5E_RESOURCE, H5E_CANTGET, H5_ITER_ERROR, "unable to get entry type")

        /* Create freespace entry for raw data only when the dirty entry is a
         * object header or chunk index entry
         */
        if(ctx->alloc_type == H5FD_MEM_DRAW) {
            if((type_id == H5AC_BT2_HDR_ID || type_id == H5AC_BT2_INT_ID || type_id == H5AC_BT2_LEAF_ID)
                    || (type_id == H5AC_EARRAY_HDR_ID || type_id == H5AC_EARRAY_IBLOCK_ID || type_id == H5AC_EARRAY_SBLOCK_ID || type_id == H5AC_EARRAY_DBLOCK_ID || type_id == H5AC_EARRAY_DBLK_PAGE_ID)
                    || (type_id == H5AC_FARRAY_HDR_ID || type_id == H5AC_FARRAY_DBLOCK_ID || type_id == H5AC_FARRAY_DBLK_PAGE_ID)
                    || (type_id == H5AC_OHDR_ID || type_id == H5AC_OHDR_CHK_ID))
                create_fd = TRUE;
        } /* end if */
        /* Don't create flush dependency on cache-internal entries */
        else if(type_id != H5AC_FREEDSPACE_ID && type_id != H5AC_PROXY_ENTRY_ID
                && type_id != H5AC_EPOCH_MARKER_ID && type_id != H5AC_PREFETCHED_ENTRY_ID)
            create_fd = TRUE;
    
        /* Check for creating the flush dependency */
        if(create_fd) {
            /* Create freedspace object, if not already available */
            if(ctx->fs == NULL)
                /* Allocate new freedspace object */
                if(NULL == (ctx->fs = H5MF__freedspace_new(ctx)))
                    HGOTO_ERROR(H5E_RESOURCE, H5E_CANTCREATE, H5_ITER_ERROR, "can't create freed space entry")

            /* Create flush dependency between the freedspace entry and the dirty entry */
            if(H5AC_create_flush_dependency(ctx->fs, entry) < 0)
               HGOTO_ERROR(H5E_RESOURCE, H5E_CANTCREATE, H5_ITER_ERROR, "can't create flush dependency")
        } /* end if */
    } /* end if */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5MF__freedspace_create_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5MF__freedspace_create
 *
 * Purpose:     Create a new freedspace entry
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Houjun Tang
 *              June 8, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5MF__freedspace_create(H5F_t *f, H5FD_mem_t alloc_type, haddr_t addr,
    hsize_t size, H5MF_freedspace_t **fs)
{
    htri_t cache_clean;         /* Whether the cache has any dirty entries */
    herr_t ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_PACKAGE

    /* Sanity checks */
    HDassert(f);
    HDassert(H5F_addr_defined(addr));
    HDassert(fs);
    HDassert(NULL == *fs);

    /* Check if there's any dirty entries in the cache currently */
    if((cache_clean = H5AC_cache_is_clean(f, H5AC_RING_SB)) < 0)
        HGOTO_ERROR(H5E_RESOURCE, H5E_CANTGET, FAIL, "unable to check for dirty entries in cache")

    /* Check if there's any dirty entries in the cache currently */
    if(!cache_clean) {
        H5MF_freedspace_ctx_t fs_ctx;       /* Context for cache iteration */
        unsigned status = 0;    /* Cache entry status for address being freed */

        /* Check cache status of address being freed */
        if(alloc_type != H5FD_MEM_DRAW)
            if(H5AC_get_entry_status(f, addr, &status) < 0)
                HGOTO_ERROR(H5E_RESOURCE, H5E_CANTGET, FAIL, "unable to get entry status")

        /* Create a freed space ctx */
        fs_ctx.f           = f;
        if(alloc_type == H5FD_MEM_DRAW)
            fs_ctx.ring     = H5AC_RING_USER;
        else if((status & H5AC_ES__IN_CACHE) != 0) {
            if(H5AC_get_entry_ring(f, addr, &fs_ctx.ring) < 0)
                HGOTO_ERROR(H5E_RESOURCE, H5E_CANTGET, FAIL, "can't get ring of entry")
        } /* end if */
        else
            fs_ctx.ring    = H5CX_get_ring();
        fs_ctx.alloc_type  = alloc_type;
        fs_ctx.addr        = addr;
        fs_ctx.size        = size;
        fs_ctx.fs          = NULL;

        /* Iterate through cache entries, setting up flush dependencies */
        if(H5AC_iterate(f, H5MF__freedspace_create_cb, &fs_ctx) < 0)
            HGOTO_ERROR(H5E_RESOURCE, H5E_BADITER, FAIL, "unable to iterate cache entries")

        /* If there were entries in the cache that set up flush dependencies, finish setting up freedspace object */
        if(NULL != fs_ctx.fs) {
#if defined H5_DEBUG_BUILD
{
unsigned nchildren;         /* # of flush dependency children for freedspace object */

/* Get # of flush dependency children now */
if(H5AC_get_flush_dep_nchildren((H5AC_info_t *)fs_ctx.fs, &nchildren) < 0)
    HGOTO_ERROR(H5E_RESOURCE, H5E_CANTGET, FAIL, "can't get cache entry nchildren")

/* Sanity check to make certain that freedspace entry has at least one flush dependency child entry */
if(0 == nchildren)
    HGOTO_ERROR(H5E_RESOURCE, H5E_BADVALUE, FAIL, "no flush dependency children for new freedspace object")
}
#endif /* H5_DEBUG_BUILD */

            /* Set the pointer to the freedspace entry, for the calling routine */
            *fs = fs_ctx.fs;
        } /* end if */
    } /* end if */

done:
    /* Release resources on error */
    if(ret_value < 0)
        if(*fs)
            *fs = H5FL_FREE(H5MF_freedspace_t, *fs);

    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5MF__freedspace_create() */


/*-------------------------------------------------------------------------
 * Function:    H5MF__freedspace_push
 *
 * Purpose:     Push a freedspace entry to the "holding tank" 
 *                                          (singly-linked list)
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Houjun Tang
 *              August 18, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5MF__freedspace_push(H5MF_freedspace_t **head, H5MF_freedspace_t **tail, H5MF_freedspace_t *freedspace)
{
    FUNC_ENTER_PACKAGE_NOERR

    /* Sanity checks */
    HDassert(freedspace);

    if(NULL == *head) {
        /* Add the first node */
        *head = freedspace;
        *tail = freedspace;
    } /* end if */
    else {
        /* Append to the linked list */
        (*tail)->next    = freedspace;
        *tail            = freedspace;
        freedspace->next = NULL;
    } /* end else */

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5MF__freedspace_push() */


/*-------------------------------------------------------------------------
 * Function:    H5MF__freedspace_dequeue_time_limit
 *
 * Purpose:     Dequeue the oldest entry that has been in the queue for 
 *              more than time_limit (2 delta t) time from the freedspace 
 *              queue.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Houjun Tang
 *              August 22, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5MF__freedspace_dequeue_time_limit(H5F_t *f, H5MF_freedspace_t **freedspace,
    uint64_t time_limit)
{
    H5MF_freedspace_t **head = &f->shared->freedspace_head;     /* Pointer to head of freedspace list */

    FUNC_ENTER_PACKAGE_NOERR

    /* Sanity checks */
    HDassert(freedspace);

    if(NULL == *head)
        *freedspace = NULL;
    else {
        uint64_t now_time, entry_time;

        /* Get relevant times */
        now_time = H5_now_usec();
        entry_time = (*head)->timestamp;

        /* Only dequeue when the entry has been in the queue longer than time limit (2 deltat) */
        if(now_time - entry_time > (uint64_t)time_limit) {
            *freedspace = *head;
            if(NULL != (*head)->next)
                *head = (*head)->next;
            else {
                /* queue is empty now */
                f->shared->freedspace_head = NULL;
                f->shared->freedspace_tail = NULL;
            } /* end else */
        } /* end if */
    } /* end else */

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5MF__freedspace_dequeue() */


/*-------------------------------------------------------------------------
 * Function:    H5MF__freedspace_queue_is_empty
 *
 * Purpose:     Check if the freedspace queue is empty
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Houjun Tang
 *              August 22, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5MF__freedspace_queue_is_empty(H5MF_freedspace_t *head, hbool_t *is_empty)
{
    FUNC_ENTER_PACKAGE_NOERR

    /* Sanity checks */
    HDassert(is_empty);

    if(NULL == head) 
        *is_empty = TRUE;
    else 
        *is_empty = FALSE;

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5MF__freedspace_queue_is_empty() */


/*-------------------------------------------------------------------------
 * Function:    H5MF__freedspace_dest
 *
 * Purpose:     Destroys a freedspace entry in memory.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Houjun Tang
 *              June 8, 2017
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5MF__freedspace_dest(H5MF_freedspace_t *freedspace)
{
    FUNC_ENTER_PACKAGE_NOERR

    /* Sanity checks */
    HDassert(freedspace);

    /* Free the freedspace entry object */
    freedspace = H5FL_FREE(H5MF_freedspace_t, freedspace);

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5MF__freedspace_dest() */

